"""Positive controls for the chainstate block-graph ownership ratchet."""

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import graph_owner_gate as gate


def put(repo, rel, content):
    path = repo / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    return path


@pytest.fixture
def fixture_repo(tmp_path):
    put(tmp_path, "CMakeLists.txt", """
add_library(dinero_core STATIC
  src/consensus/block_index.cpp
  src/consensus/block_lifecycle.cpp
  src/daemon/graph_user.cpp)
""")
    put(tmp_path, "src/consensus/CMakeLists.txt",
        "add_library(dinero_consensus STATIC src/consensus/block_index.cpp)\n")
    put(tmp_path, "tests/CMakeLists.txt",
        "add_executable(test_graph src/consensus/block_index.cpp)\n")
    put(tmp_path, "src/consensus/block_index.cpp", "// owner\n")
    put(tmp_path, "src/consensus/block_lifecycle.cpp", "// owner\n")
    put(tmp_path, "src/daemon/graph_user.cpp", "void f() { (void)g_block_index; }\n")
    return tmp_path


def test_live_repo_has_no_new_owner_or_raw_consumer():
    repo = Path(__file__).resolve().parents[4]
    current = gate.scan(repo)
    baseline = json.loads(gate.BASELINE.read_text(encoding="utf-8"))
    assert gate.regressions(current, baseline) == []
    assert gate.stale_baseline(current, baseline) == []
    assert current["cmake_sources"]["CMakeLists.txt"]["src/consensus/block_index.cpp"] == 1
    assert current["cmake_sources"]["src/consensus/CMakeLists.txt"]["src/consensus/block_index.cpp"] == 1
    assert current["raw_globals"]["src/daemon/services/chainstate_service.cpp"]["g_block_index_mutex"] > 0


def test_new_direct_use_and_new_compiled_source_are_rejected(fixture_repo):
    baseline = gate.scan(fixture_repo)
    put(fixture_repo, "src/daemon/graph_user.cpp",
        "void f() { (void)g_block_index; (void)g_candidates; }\n")
    put(fixture_repo, "src/daemon/new_user.cpp", "void g() { (void)g_orphan_pool; }\n")
    with (fixture_repo / "CMakeLists.txt").open("a", encoding="utf-8") as file:
        file.write("target_sources(dinero_core PRIVATE src/daemon/new_user.cpp)\n")
    added = gate.regressions(gate.scan(fixture_repo), baseline)
    assert ("raw_globals", "src/daemon/graph_user.cpp", "g_candidates", 0, 1) in added
    assert ("raw_globals", "src/daemon/new_user.cpp", "g_orphan_pool", 0, 1) in added


def test_new_header_reference_is_rejected(fixture_repo):
    baseline = gate.scan(fixture_repo)
    put(fixture_repo, "include/daemon/new_graph_access.h",
        "inline void read_graph() { (void)g_candidates; }\n")
    assert ("raw_globals", "include/daemon/new_graph_access.h", "g_candidates", 0, 1) in \
        gate.regressions(gate.scan(fixture_repo), baseline)


def test_extra_library_owner_and_test_compile_are_rejected(fixture_repo):
    baseline = gate.scan(fixture_repo)
    put(fixture_repo, "src/storage/CMakeLists.txt",
        'add_library(dinero_chainstate STATIC "${CMAKE_SOURCE_DIR}/src/consensus/block_index.cpp")\n')
    with (fixture_repo / "tests/CMakeLists.txt").open("a", encoding="utf-8") as file:
        file.write("target_sources(test_graph PRIVATE src/consensus/block_lifecycle.cpp)\n")
    added = gate.regressions(gate.scan(fixture_repo), baseline)
    assert ("cmake_sources", "src/storage/CMakeLists.txt", "src/consensus/block_index.cpp", 0, 1) in added
    assert ("cmake_sources", "tests/CMakeLists.txt", "src/consensus/block_lifecycle.cpp", 0, 1) in added


def test_owner_in_another_first_party_directory_is_rejected(fixture_repo):
    baseline = gate.scan(fixture_repo)
    put(fixture_repo, "mobile/runtime/CMakeLists.txt",
        "add_library(mobile STATIC src/consensus/block_index.cpp)\n")
    assert ("cmake_sources", "mobile/runtime/CMakeLists.txt", "src/consensus/block_index.cpp", 0, 1) in \
        gate.regressions(gate.scan(fixture_repo), baseline)


def test_generated_build_tree_is_not_a_source_owner(fixture_repo):
    baseline = gate.scan(fixture_repo)
    put(fixture_repo, "cmake-build-debug/CMakeLists.txt",
        "add_library(generated STATIC src/consensus/block_index.cpp)\n")
    put(fixture_repo, "out/CMakeLists.txt",
        "add_library(generated STATIC src/consensus/block_lifecycle.cpp)\n")
    assert gate.regressions(gate.scan(fixture_repo), baseline) == []


def test_comment_and_literal_decoys_are_ignored(fixture_repo):
    baseline = gate.scan(fixture_repo)
    with (fixture_repo / "src/daemon/graph_user.cpp").open("a", encoding="utf-8") as file:
        file.write('''
// g_candidates
/* g_orphan_pool */
const char* note = "g_invalid_blocks";
const char* raw = R"tag(g_inflight_blocks)tag";
''')
    with (fixture_repo / "CMakeLists.txt").open("a", encoding="utf-8") as file:
        file.write("# src/consensus/block_index.cpp\n")
    assert gate.regressions(gate.scan(fixture_repo), baseline) == []


def test_reduction_requires_baseline_rewrite(fixture_repo, tmp_path):
    baseline = gate.scan(fixture_repo)
    baseline_path = tmp_path / "baseline.json"
    assert gate.main(["--repo", str(fixture_repo), "--baseline", str(baseline_path),
                      "--write-baseline"]) == 0
    put(fixture_repo, "src/daemon/graph_user.cpp", "void f() {}\n")
    current = gate.scan(fixture_repo)
    assert gate.regressions(current, baseline) == []
    assert ("raw_globals", "src/daemon/graph_user.cpp", "g_block_index", 1, 0) in \
        gate.stale_baseline(current, baseline)
    assert gate.main(["--repo", str(fixture_repo), "--baseline", str(baseline_path)]) == 1
    assert gate.main(["--repo", str(fixture_repo), "--baseline", str(baseline_path),
                      "--write-baseline"]) == 0
    assert gate.main(["--repo", str(fixture_repo), "--baseline", str(baseline_path)]) == 0


def test_neutering_raw_scanner_breaks_positive_control(fixture_repo, monkeypatch):
    baseline = gate.scan(fixture_repo)
    put(fixture_repo, "src/daemon/graph_user.cpp",
        "void f() { (void)g_block_index; (void)g_candidates; }\n")
    assert gate.regressions(gate.scan(fixture_repo), baseline)
    with monkeypatch.context() as edit:
        edit.setattr(gate, "RAW_GLOBALS", ())
        with pytest.raises(AssertionError):
            assert gate.regressions(gate.scan(fixture_repo), baseline)


def test_neutering_source_scanner_breaks_positive_control(fixture_repo, monkeypatch):
    baseline = gate.scan(fixture_repo)
    put(fixture_repo, "src/storage/CMakeLists.txt",
        "add_library(dinero_chainstate STATIC src/consensus/block_index.cpp)\n")
    assert gate.regressions(gate.scan(fixture_repo), baseline)
    with monkeypatch.context() as edit:
        edit.setattr(gate, "SOURCE_FILES", ())
        with pytest.raises(AssertionError):
            assert gate.regressions(gate.scan(fixture_repo), baseline)


def test_cli_returns_failure_for_new_consumer(fixture_repo, tmp_path, capsys):
    baseline = tmp_path / "baseline.json"
    assert gate.main(["--repo", str(fixture_repo), "--baseline", str(baseline),
                      "--write-baseline"]) == 0
    put(fixture_repo, "src/daemon/graph_user.cpp",
        "void f() { (void)g_block_index; (void)g_candidates; }\n")
    assert gate.main(["--repo", str(fixture_repo), "--baseline", str(baseline)]) == 1
    assert "GRAPH OWNER REGRESSION" in capsys.readouterr().out
