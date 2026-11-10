"""GATE P1 checker tests (spec 2026-11-10-consensus-globals-purity-design.md §4).

Covers: CMake source-list parsing on the live tree, tier dedup and exemption,
a positive-control fixture (a planted violation is found; comments, strings
and longer identifiers are not), the neuter proof (removing the token from
the deny list hides the planted violation, so the live deny list must carry
it), stale-baseline strictness (improvements must regenerate the baseline in
the same commit), and the live-repo test pinning the committed baseline to a
fresh scan — any drift in either direction fails here and in CI.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import check_consensus_globals as ccg

REPO = Path(__file__).resolve().parents[4]

MINI_CMAKE = """
add_library(dinero_consensus_core STATIC
  src/consensus/pure_a.cpp   # a kernel source
)
add_library(dinero_consensus STATIC
  src/consensus/pure_a.cpp
  src/consensus/full_b.cpp
)
"""

PLANTED = """
#include "consensus/chainparams.h"
// Params() in a comment is not a read
static const char* kDoc = "call Params() for details";
int f() { return SelectParams(Chain::MAIN), Params().genesis_height; }
"""

CLEAN = "int g() { return 0; }\n"


def make_fixture(tmp_path, b_body=CLEAN):
    (tmp_path / "src/consensus").mkdir(parents=True)
    (tmp_path / "src/consensus/CMakeLists.txt").write_text(MINI_CMAKE)
    (tmp_path / "src/consensus/pure_a.cpp").write_text(PLANTED)
    (tmp_path / "src/consensus/full_b.cpp").write_text(b_body)
    return tmp_path


def fixture_rules():
    rules = ccg.load_rules()
    rules = {**rules, "extra_files": {"K": [], "L": []}, "exempt_files": []}
    return rules


def test_parse_cmake_sets_live():
    sets = ccg.parse_cmake_sets(REPO)
    core, full = sets["dinero_consensus_core"], sets["dinero_consensus"]
    assert "src/consensus/utreexo_accumulator.cpp" in core
    assert "src/consensus/utreexo_accumulator.cpp" in full
    assert "src/consensus/chainparams_impl.cpp" in full
    assert "src/consensus/chainparams_impl.cpp" not in core
    assert len(core) > 20 and len(full) > 40


def test_tier_dedup_and_exemption_live():
    rows = ccg.scan_repo(REPO)
    files_k = {f for t, f, _, _ in rows if t == "K"}
    files_l = {f for t, f, _, _ in rows if t == "L"}
    assert not files_k & files_l  # a shared source reports only under K
    assert not any(f.endswith("chainparams_impl.cpp") for f in files_k | files_l)


def test_positive_control_fixture(tmp_path):
    rows = ccg.scan_repo(make_fixture(tmp_path), fixture_rules())
    # exactly one real read: not the comment, not the string, not SelectParams(
    assert rows == [("K", "src/consensus/pure_a.cpp", "Params_call", 1)]


def test_neuter_removing_token_hides_violation(tmp_path):
    rules = fixture_rules()
    rules["_compiled"] = [(n, rx) for n, rx in rules["_compiled"] if n != "Params_call"]
    assert ccg.scan_repo(make_fixture(tmp_path), rules) == []
    # therefore the live deny list must carry the token
    assert "Params_call" in dict(ccg.load_rules()["_compiled"])


def test_regression_and_stale_are_both_failures(tmp_path):
    current = ccg.scan_repo(make_fixture(tmp_path), fixture_rules())
    regressions, stale = ccg.compare(current, [])
    assert regressions and not stale  # new row above an empty baseline
    regressions, stale = ccg.compare(current, [("K", "src/consensus/pure_a.cpp", "Params_call", 3)])
    assert stale and not regressions  # improvement without --write-baseline
    regressions, stale = ccg.compare(current, current)
    assert not regressions and not stale


def test_live_repo_baseline_matches_fresh_scan_exactly():
    # The ratchet's atomicity: any commit that changes an ambient-read count
    # in either direction without regenerating the baseline fails here.
    assert ccg.scan_repo(REPO) == ccg.read_baseline()
