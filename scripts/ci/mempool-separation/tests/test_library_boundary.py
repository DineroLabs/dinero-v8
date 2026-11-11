"""Exercise the boundary checker against real CMake File API graphs."""
import json
import importlib.util
import subprocess
import sys
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "library_boundary.py"


def load_checker():
    spec = importlib.util.spec_from_file_location("mempool_library_boundary", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def project(tmp_path, extra=""):
    source = tmp_path / "source"
    build = tmp_path / "build"
    source.mkdir()
    (source / "pool.cpp").write_text("int pool() { return 1; }\n")
    (source / "node.cpp").write_text("int node() { return 2; }\n")
    (source / "stub.cpp").write_text("int stub() { return 3; }\n")
    (source / "tests/stubs").mkdir(parents=True)
    (source / "tests/stubs/mempool_test_stubs.cpp").write_text("int stub() { return 3; }\n")
    (source / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 4.1)\nproject(boundary LANGUAGES CXX)\n"
        "set(CMAKE_EXPORT_COMPILE_COMMANDS ON)\n"
        "add_library(dinero_mempool STATIC pool.cpp)\n" + extra)
    query = build / ".cmake/api/v1/query"
    query.mkdir(parents=True)
    (query / "codemodel-v2").touch()
    configured = subprocess.run(["cmake", "-S", str(source), "-B", str(build), "-G", "Ninja"],
                                capture_output=True, text=True)
    assert configured.returncode == 0, configured.stdout + configured.stderr
    return source, build


def run(build):
    return subprocess.run([sys.executable, str(SCRIPT), "--build", str(build),
                           "--skip-source-owner-check"], capture_output=True, text=True)


def test_clean_graph_passes(tmp_path):
    _, build = project(tmp_path)
    result = run(build)
    assert result.returncode == 0, result.stdout + result.stderr
    assert json.loads(result.stdout)["owned_boundary"] == "PASS"


def test_private_core_edge_fails(tmp_path):
    _, build = project(tmp_path, "add_library(dinero_core STATIC node.cpp)\n"
                      "target_link_libraries(dinero_mempool PRIVATE dinero_core)\n")
    result = run(build)
    assert result.returncode == 1
    assert "FORBIDDEN_TARGET dinero_core" in result.stdout


def test_wallet_reverse_edge_fails(tmp_path):
    _, build = project(tmp_path, "add_library(dinero_wallet STATIC node.cpp)\n"
                      "target_link_libraries(dinero_wallet PRIVATE dinero_mempool)\n")
    result = run(build)
    assert result.returncode == 1
    assert "REVERSE_EDGE dinero_wallet" in result.stdout


def test_transitive_stub_collision_fails(tmp_path):
    _, build = project(tmp_path, "add_library(dinero_core STATIC node.cpp)\n"
                      "target_link_libraries(dinero_core PUBLIC dinero_mempool)\n"
                      "add_library(consumer STATIC tests/stubs/mempool_test_stubs.cpp)\n"
                      "target_link_libraries(consumer PRIVATE dinero_core)\n")
    result = run(build)
    assert result.returncode == 1
    assert "STUB_WITH_REAL_MEMPOOL consumer" in result.stdout


def test_inherited_sqlite_is_visible_not_full_pass(tmp_path):
    _, build = project(tmp_path, "add_library(sqlite3 INTERFACE)\n"
                      "add_library(dinero_shielded STATIC node.cpp)\n"
                      "target_link_libraries(dinero_shielded PRIVATE sqlite3)\n"
                      "target_link_libraries(dinero_mempool PUBLIC dinero_shielded)\n")
    result = run(build)
    assert result.returncode == 1
    report = json.loads(result.stdout)
    assert report["owned_boundary"] == "PASS"
    assert report["status"] == "INHERITED_DEBT"
    assert any("sqlite3" in item for item in report["inherited_debt"])


def test_direct_sqlite_edge_is_not_hidden_by_inherited_shielded_edge(tmp_path):
    _, build = project(tmp_path, "add_library(sqlite3 INTERFACE)\n"
                      "add_library(dinero_shielded STATIC node.cpp)\n"
                      "target_link_libraries(dinero_shielded PRIVATE sqlite3)\n"
                      "target_link_libraries(dinero_mempool PUBLIC dinero_shielded sqlite3)\n")
    result = run(build)
    assert result.returncode == 1
    report = json.loads(result.stdout)
    assert report["owned_boundary"] == "FAIL"
    assert "STORAGE_TARGET sqlite3" in report["violations"]


def test_direct_sqlite_fragment_is_not_hidden_by_inherited_shielded_edge(tmp_path):
    _, build = project(tmp_path, "add_library(dinero_shielded STATIC node.cpp)\n"
                      "target_link_libraries(dinero_shielded PRIVATE sqlite3)\n"
                      "target_link_libraries(dinero_mempool PUBLIC dinero_shielded sqlite3)\n")
    result = run(build)
    assert result.returncode == 1
    report = json.loads(result.stdout)
    assert report["owned_boundary"] == "FAIL"
    assert "STORAGE_LIBRARY sqlite3 from=dinero_mempool" in report["violations"]


def test_missing_model_is_error_not_pass(tmp_path):
    result = run(tmp_path)
    assert result.returncode == 2
    assert "no index" in result.stderr


def test_orchard_reader_define_must_follow_moved_source():
    checker = load_checker()
    source = "/source/src/daemon/relay_transaction_reader.cpp"
    command = {"file": source, "command": "c++ -c " + source}
    assert checker.orchard_reader_define_violations([command], False) == []
    assert checker.orchard_reader_define_violations([command], True) == [
        "MISSING_ORCHARD_RELAY_READER_DEFINE"]
    command["command"] += " -DDINERO_HAS_ORCHARD_RELAY_READER=1"
    assert checker.orchard_reader_define_violations([command], True) == []


def test_compile_owner_accepts_windows_ninja_paths():
    checker = load_checker()
    command = {"output": r"CMakeFiles\dinero_mempool.dir\src\daemon\mempool.cpp.obj",
               "command": "cl.exe /c src\\daemon\\mempool.cpp"}
    assert checker.compile_owner(command) == "dinero_mempool"
