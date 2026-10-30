import importlib.util
import json
import os
import stat
import subprocess
import sys
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent.parent / "sep_targets.py"


def load():
    spec = importlib.util.spec_from_file_location("sep_targets", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


LIST = """# comment
TestA
TestB
OnFloor
on:OnlyOn
TestA
NotRegistered
PyTest
"""
FLOOR = "# floor header\nOnFloor\nOnlyOnFloor\n"


def tests_json(build):
    def t(name, exe):
        return {"name": name, "command": [exe, "--arg"]}
    return [
        t("TestA", os.path.join(build, "test_a")),
        t("TestB", os.path.join(build, "sub", "test_b.exe")),
        t("OnFloor", os.path.join(build, "test_floor")),
        t("OnlyOn", os.path.join(build, "test_only_on")),
        t("PyTest", "/usr/bin/python3"),
        t("Unlisted", os.path.join(build, "test_unlisted")),
    ]


def test_k4_names_filters_config_and_floor():
    m = load()
    floor = m.read_names(FLOOR)
    assert m.k4_names(LIST, "off", floor) == ["TestA", "TestB", "NotRegistered", "PyTest"]
    assert m.k4_names(LIST, "on", floor) == ["TestA", "TestB", "OnlyOn", "NotRegistered", "PyTest"]


def test_floor_name_never_contributes_a_target(tmp_path):
    m = load()
    names = m.k4_names(LIST, "on", m.read_names(FLOOR))
    targets, _, _ = m.k4_targets(names, tests_json(str(tmp_path)), str(tmp_path))
    assert "test_floor" not in targets


def test_k4_targets_registered_in_build_only(tmp_path):
    m = load()
    b = str(tmp_path)
    names = m.k4_names(LIST, "on", m.read_names(FLOOR))
    targets, unregistered, unresolved = m.k4_targets(names, tests_json(b), b)
    # .exe stripped; interpreter outside the build dir skipped; unlisted ignored
    assert targets == ["test_a", "test_b", "test_only_on"]
    assert unregistered == ["NotRegistered"]
    assert unresolved == []


def test_select_keeps_only_existing_targets():
    m = load()
    avail = m.available("dinerod: phony\ntest_a: phony\nbuild.ninja: CUSTOM_COMMAND\nnoise\n")
    have, missing = m.select(["dinerod", "test_a", "nodecore_ffi", "test_a"], avail)
    assert have == ["dinerod", "test_a"]
    assert missing == ["nodecore_ffi"]


def _exe(path, body):
    path.write_text("#!%s\n%s" % (sys.executable, body))
    path.chmod(path.stat().st_mode | stat.S_IEXEC)
    return str(path)


def test_cli_end_to_end(tmp_path):
    b = tmp_path / "build"
    b.mkdir()
    lst = tmp_path / "list.txt"
    lst.write_text(LIST)
    floor = tmp_path / "floor.txt"
    floor.write_text(FLOOR)
    (tmp_path / "tests.json").write_text(json.dumps({"tests": tests_json(str(b))}))
    ctest = _exe(tmp_path / "ctest", "import sys; print(open(%r).read())\n" % str(tmp_path / "tests.json"))
    ninja = _exe(tmp_path / "ninja", "print('dinerod: phony\\ndinero_wallet: phony\\ntest_a: phony\\n"
                                      "test_floor: phony\\ntest_only_on: phony')\n")
    out = tmp_path / "targets.txt"
    r = subprocess.run([sys.executable, str(SCRIPT), "--build", str(b), "--config", "on",
                        "--out", str(out), "--list", str(lst), "--floor", str(floor),
                        "--ctest", ctest, "--ninja", ninja], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    assert out.read_text().split() == ["dinero_wallet", "dinerod", "test_a", "test_only_on"]
    assert "::warning::target test_b does not exist" in r.stdout
    assert "test_floor" not in out.read_text()


def test_cli_missing_floor_is_input_error(tmp_path):
    lst = tmp_path / "list.txt"
    lst.write_text(LIST)
    r = subprocess.run([sys.executable, str(SCRIPT), "--build", str(tmp_path), "--config", "off",
                        "--out", str(tmp_path / "o.txt"), "--list", str(lst),
                        "--floor", str(tmp_path / "absent.txt")], capture_output=True, text=True)
    assert r.returncode == 2
    assert r.stderr.startswith("ERROR ")


# --- C1: ctest --show-only=json-v1 omits "command" until the executable is built ---

def prebuild_json():
    # What ctest emits on a fresh configure: names and properties, no "command".
    return [{"name": n, "properties": []} for n in
            ("TestA", "TestB", "OnFloor", "OnlyOn", "PyTest", "Unlisted")]


def write_ctestfiles(build):
    (build / "sub").mkdir(parents=True, exist_ok=True)
    (build / "CTestTestfile.cmake").write_text(
        '# CMake generated Testfile\n'
        'add_test([=[TestA]=] "%s/test_a")\n'
        'set_tests_properties([=[TestA]=] PROPERTIES  LABELS "consensus")\n'
        'add_test([=[PyTest]=] "python3" "/src/check.py" "--self-test")\n'
        'add_test([=[OnFloor]=] "%s/test_floor")\n'
        'subdirs("sub")\n' % (build, build))
    (build / "sub" / "CTestTestfile.cmake").write_text(
        'add_test(TestB "%s/sub/test_b.exe" "--x")\n'
        'add_test([==[OnlyOn]==] "%s/test_only_on" "--gtest_filter=OnlyOn.*")\n' % (build, build))


def test_prebuild_json_resolves_targets_from_ctesttestfile(tmp_path):
    m = load()
    b = tmp_path / "build"
    write_ctestfiles(b)
    names = m.k4_names(LIST, "on", m.read_names(FLOOR))
    cmds = m.ctesttestfile_commands(str(b))
    targets, unregistered, unresolved = m.k4_targets(names, prebuild_json(), str(b), cmds)
    assert targets == ["test_a", "test_b", "test_only_on"]
    assert unregistered == ["NotRegistered"]
    assert unresolved == []


def test_registered_without_resolvable_command_is_unresolved(tmp_path):
    m = load()
    b = tmp_path / "build"
    b.mkdir()
    (b / "CTestTestfile.cmake").write_text('add_test([=[TestA]=] "%s/test_a")\n' % b)
    names = m.k4_names(LIST, "off", m.read_names(FLOOR))
    targets, unregistered, unresolved = m.k4_targets(
        names, prebuild_json(), str(b), m.ctesttestfile_commands(str(b)))
    assert targets == ["test_a"]
    # registered (present in json-v1) but no add_test line found: loud, not skipped
    assert unresolved == ["TestB", "PyTest"]
    assert unregistered == ["NotRegistered"]


def test_cli_unresolved_fails_and_is_written(tmp_path):
    b = tmp_path / "build"
    b.mkdir()
    (b / "CTestTestfile.cmake").write_text('add_test([=[TestA]=] "%s/test_a")\n' % b)
    lst = tmp_path / "list.txt"
    lst.write_text("TestA\nTestB\n")
    floor = tmp_path / "floor.txt"
    floor.write_text(FLOOR)
    (tmp_path / "tests.json").write_text(json.dumps({"tests": prebuild_json()}))
    ctest = _exe(tmp_path / "ctest", "print(open(%r).read())\n" % str(tmp_path / "tests.json"))
    ninja = _exe(tmp_path / "ninja", "print('test_a: phony')\n")
    out, unres = tmp_path / "t.txt", tmp_path / "unresolved.txt"
    r = subprocess.run([sys.executable, str(SCRIPT), "--build", str(b), "--config", "off",
                        "--out", str(out), "--unresolved-out", str(unres), "--list", str(lst),
                        "--floor", str(floor), "--ctest", ctest, "--ninja", ninja],
                       capture_output=True, text=True)
    assert r.returncode == 3, r.stdout + r.stderr
    assert "test_a" in out.read_text().split()
    assert unres.read_text().split() == ["TestB"]
    assert "::error::K4 test TestB is registered but its executable target is unresolved" in r.stdout
