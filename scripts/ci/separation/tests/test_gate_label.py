import subprocess
import sys
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent.parent / "gate_label.py"


def run(tmp_path, gate, **kw):
    args = [sys.executable, str(SCRIPT), gate, "--github-output", str(tmp_path / "gh.txt")]
    for k, v in kw.items():
        args += ["--" + k.replace("_", "-"), str(v)]
    r = subprocess.run(args, capture_output=True, text=True)
    gh = (tmp_path / "gh.txt").read_text() if (tmp_path / "gh.txt").exists() else ""
    return r.returncode, r.stdout.strip(), gh


def k4files(tmp_path, out, unresolved=""):
    o = tmp_path / "k4.out"
    o.write_text(out)
    u = tmp_path / "unres.txt"
    if unresolved is not None:
        u.write_text(unresolved)
    return o, u


OK_OUT = "OK 24 tests match reference (provenance abc)\n"


def test_k4_pass_with_reference(tmp_path):
    o, u = k4files(tmp_path, OK_OUT)
    assert run(tmp_path, "k4", output=o, unresolved=u, rc=0, reference_present=1) == (0, "GATE K4 PASS", "")


def test_k4_missing_reference_only_when_tests_ran(tmp_path):
    o, u = k4files(tmp_path, OK_OUT)
    rc, line, gh = run(tmp_path, "k4", output=o, unresolved=u, rc=0, reference_present=0)
    assert (rc, line) == (2, "GATE K4 FAIL rc=2 (missing reference — not a vector mismatch)")
    assert gh == "note=missing reference\n"


def test_k4_not_run_is_not_labelled_missing_reference(tmp_path):
    # run 1 of PR #860: every K4 test NOT_RUN, no reference for the platform
    o, u = k4files(tmp_path, "NOT_RUN BIP341SighashVectors\nNOT_RUN BIP119CTVVectors\n")
    rc, line, gh = run(tmp_path, "k4", output=o, unresolved=u, rc=1, reference_present=0)
    assert (rc, line) == (1, "GATE K4 FAIL rc=1 (check did not run: NOT_RUN BIP341SighashVectors)")
    assert gh == ""


def test_k4_unresolved_targets_fail(tmp_path):
    o, u = k4files(tmp_path, OK_OUT, "BIP341SighashVectors\n")
    rc, line, _ = run(tmp_path, "k4", output=o, unresolved=u, rc=0, reference_present=1)
    assert (rc, line) == (1, "GATE K4 FAIL rc=1 (check did not run: K4 targets unresolved: BIP341SighashVectors)")


def test_k4_missing_unresolved_file_fails(tmp_path):
    o, u = k4files(tmp_path, OK_OUT, None)
    rc, line, _ = run(tmp_path, "k4", output=o, unresolved=u, rc=0, reference_present=1)
    assert rc == 1 and "target resolution (sep_targets.py) did not run" in line


def test_k4_digest_mismatch_and_failed_test(tmp_path):
    o, u = k4files(tmp_path, "DIGEST_MISMATCH MerkleGolden\n")
    assert run(tmp_path, "k4", output=o, unresolved=u, rc=1, reference_present=1)[:2] == (
        1, "GATE K4 FAIL rc=1 (vector mismatch: DIGEST_MISMATCH MerkleGolden)")
    o, u = k4files(tmp_path, "FAILED MerkleGolden\n")
    assert run(tmp_path, "k4", output=o, unresolved=u, rc=1, reference_present=0)[:2] == (
        1, "GATE K4 FAIL rc=1 (test failed: MerkleGolden)")


# W5 ctest logs (shapes from PR #860 run 1 and the local macOS run)
W5_WINDOWS_RUN1 = """\
1/2 Test #866: SepInterfaceFreeze ...............***Failed    1.82 sec
FAILED: [code=2] CMakeFiles/nodecore_ffi.dir/src/nodecore/maintenance_journal.cpp.obj
CMake Error at D:/a/tests/separation/run_built_script.cmake:21 (message):
  SEPARATION TEST BUILD FAILED: nodecore_ffi;dinero_wallet_ffi (rc=2)
2/2 Test #867: SepRpcTranscript .................***Failed   15.77 sec
rpc_transcript: RUN ERROR: s exited rc=1 during startup
"""
W5_MISSING_REF_ONLY = """\
1/2 Test #974: SepInterfaceFreeze ...............***Failed    3.00 sec
PASS nodecore_ffi.h
PASS wallet_ffi.h
PASS jni_exports.txt
interface_freeze: INPUT ERROR: no baseline /x/symbols_linux_off.txt (record it at the baseline for this platform/config)
2/2 Test #975: SepRpcTranscript .................   Passed   58.21 sec
rpc_transcript: PASS (40 entries match off.json)
"""
W5_PASS = """\
1/2 Test #974: SepInterfaceFreeze ...............   Passed    3.00 sec
interface_freeze: PASS
2/2 Test #975: SepRpcTranscript .................   Passed   58.21 sec
rpc_transcript: PASS (40 entries match off.json)
"""


def w5(tmp_path, text, rc, ref):
    log = tmp_path / "w5.log"
    log.write_text(text)
    return run(tmp_path, "w5", log=log, rc=rc, reference_present=ref)


def test_w5_build_break_is_not_missing_reference(tmp_path):
    rc, line, gh = w5(tmp_path, W5_WINDOWS_RUN1, 8, 0)
    assert (rc, line) == (8, "GATE W5 FAIL rc=8 (check did not run: build failed: nodecore_ffi;dinero_wallet_ffi (rc=2))")
    assert gh == ""


def test_w5_run_error_is_not_missing_reference(tmp_path):
    text = W5_MISSING_REF_ONLY.replace("rpc_transcript: PASS (40 entries match off.json)",
                                       "rpc_transcript: RUN ERROR: dinerod exited rc=1 during startup")
    rc, line, gh = w5(tmp_path, text, 8, 0)
    assert rc == 8 and line.startswith("GATE W5 FAIL rc=8 (check did not run: rpc_transcript: RUN ERROR")
    assert gh == ""


def test_w5_missing_reference_only(tmp_path):
    rc, line, gh = w5(tmp_path, W5_MISSING_REF_ONLY, 8, 0)
    assert (rc, line) == (2, "GATE W5 FAIL rc=2 (missing reference — not an interface mismatch)")
    assert gh == "note=missing reference\n"


def test_w5_header_diff_with_missing_reference_is_a_mismatch(tmp_path):
    text = W5_MISSING_REF_ONLY.replace("PASS wallet_ffi.h", "FAIL wallet_ffi.h")
    assert w5(tmp_path, text, 8, 0)[:2] == (1, "GATE W5 FAIL rc=1 (interface mismatch (symbol reference also missing))")


def test_w5_test_not_run(tmp_path):
    text = W5_PASS.split("2/2")[0]
    assert w5(tmp_path, text, 8, 1)[:2] == (1, "GATE W5 FAIL rc=1 (check did not run: SepRpcTranscript not run)")


def test_w5_pass_and_fail_with_reference(tmp_path):
    assert w5(tmp_path, W5_PASS, 0, 1)[:2] == (0, "GATE W5 PASS")
    text = W5_PASS.replace("interface_freeze: PASS", "FAIL symbols_macos_off.txt\ninterface_freeze: FAIL")
    assert w5(tmp_path, text, 8, 1)[:2] == (1, "GATE W5 FAIL rc=1")


def test_w5_missing_reference_without_transcript_output(tmp_path):
    # --output-on-failure: a passing SepRpcTranscript prints only its result line
    text = W5_MISSING_REF_ONLY.replace("rpc_transcript: PASS (40 entries match off.json)\n", "")
    assert w5(tmp_path, text, 8, 0)[:2] == (2, "GATE W5 FAIL rc=2 (missing reference — not an interface mismatch)")


def test_w5_transcript_failed_with_missing_reference(tmp_path):
    text = W5_MISSING_REF_ONLY.replace("SepRpcTranscript .................   Passed",
                                       "SepRpcTranscript .................***Failed")
    assert w5(tmp_path, text, 8, 0)[:2] == (1, "GATE W5 FAIL rc=1 (transcript mismatch (symbol reference also missing))")
