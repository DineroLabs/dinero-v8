"""GATE O1 checker tests (2026-11-11-consensus-orchard-separation-design.md §4).

The full scan needs a built Orchard-ON tree (nm + ninja deps), which the
script-tests CI job does not have — the gate step in the gates workflow is
the live enforcement there. These tests cover everything build-independent:
rules shape (the neuter surface), ratchet semantics (regression and stale
both fail; exact match passes), baseline round-trip, and a fixture-driven
scan via monkeypatched nm/ninja outputs proving the symbol-tier logic finds
a planted dependency and the header-tier logic counts forbidden reaches.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import check_orchard_boundary as cob

REPO = Path(__file__).resolve().parents[4]


def test_rules_shape_is_the_neuter_surface():
    rules = cob.load_rules()
    assert set(rules["definer_archives"]) == {
        "libdinero_core.a", "libdinero_chainstate.a", "libdinero_shielded.a"
    }  # removing a definer would hide its violations: pin the list
    assert "libdinero_orchard_transparent.a" in rules["gated_archives"]
    assert "libdinero_transaction_reader.a" in rules["gated_archives"]
    assert len(rules["gated_archives"]) == 6
    assert "include/storage/" in rules["forbidden_header_prefixes"]
    assert "src/daemon/" in rules["forbidden_header_prefixes"]


def test_compare_regression_and_stale_both_fail():
    current = [("S", "libdinero_orchard_transparent.a", "libdinero_core.a", 2)]
    regressions, stale = cob.compare(current, [])
    assert regressions and not stale  # new symbol dependency = regression
    regressions, stale = cob.compare([], current)
    assert stale and not regressions  # vanished row without --write-baseline = stale
    regressions, stale = cob.compare(current, current)
    assert not regressions and not stale


def test_baseline_roundtrip(tmp_path):
    rows = [("H", "orchard_state_root.cpp", "include/storage/", 4),
            ("S", "libdinero_orchard_block_coins.a", "libdinero_shielded.a", 1)]
    p = tmp_path / "b.tsv"
    cob.write_baseline(sorted(rows), p)
    assert cob.read_baseline(p) == sorted(rows)


def test_fixture_scan_finds_planted_symbol_and_header(monkeypatch, tmp_path):
    rules = {
        "gated_archives": ["libgated.a"],
        "definer_archives": ["libdefiner.a"],
        "forbidden_header_prefixes": ["include/storage/"],
    }
    (tmp_path / "libgated.a").write_text("")
    (tmp_path / "libdefiner.a").write_text("")
    obj = tmp_path / "gated_thing.cpp.o"
    obj.write_text("")

    def fake_nm_lines(nm, args, path):
        if "--defined-only" in args:
            return [f"{path}: 0000 T _NeededFromDefiner"]
        return [f"{path}: U _NeededFromDefiner", f"{path}: U _free_function"]

    monkeypatch.setattr(cob, "nm_lines", fake_nm_lines)
    monkeypatch.setattr(cob, "gated_objects", lambda b, a: ["gated_thing.cpp.o"])
    monkeypatch.setattr(cob, "object_deps",
                        lambda n, b, o: ["../include/storage/chain_db.h",
                                         "../include/consensus/fine.h"])
    rows = cob.scan(tmp_path, rules)
    assert ("S", "libgated.a", "libdefiner.a", 1) in rows
    assert ("H", "gated_thing.cpp", "include/storage/", 1) in rows
    # neuter: dropping the definer hides the symbol violation
    rows2 = cob.scan(tmp_path, {**rules, "definer_archives": []})
    assert not any(t == "S" for t, *_ in rows2)


def test_live_baseline_matches_committed_shape():
    # Build-independent sanity on the committed baseline: symbol tier must be
    # EMPTY (the clusters are free-standing today) and every header row is an
    # include/storage/ reach from a known orchard source.
    rows = cob.read_baseline(REPO / "scripts/ci/separation/orchard_boundary_baseline.tsv")
    assert rows, "committed baseline missing"
    assert not [r for r in rows if r[0] == "S"]
    for tier, src, family, count in rows:
        assert tier == "H" and family == "include/storage/" and count >= 1
        assert src.startswith("orchard_")
