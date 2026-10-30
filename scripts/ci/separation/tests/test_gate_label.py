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
    assert (rc, line) == (2, "GATE K4 FAIL rc=2 (missing reference -- not a vector mismatch)")
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


def w5(tmp_path, text, rc, ref, platform="linux"):
    log = tmp_path / "w5.log"
    log.write_text(text)
    return run(tmp_path, "w5", log=log, rc=rc, reference_present=ref, platform=platform)


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
    assert (rc, line) == (2, "GATE W5 FAIL rc=2 (missing reference -- not an interface mismatch)")
    assert gh == "note=missing reference\n"


def test_w5_header_diff_with_missing_reference_is_a_mismatch(tmp_path):
    text = W5_MISSING_REF_ONLY.replace("PASS wallet_ffi.h", "FAIL wallet_ffi.h")
    assert w5(tmp_path, text, 8, 0)[:2] == (1, "GATE W5 FAIL rc=1 (interface mismatch (symbol reference also missing))")


def test_w5_test_not_run(tmp_path):
    text = W5_PASS.split("2/2")[0]
    assert w5(tmp_path, text, 8, 1)[:2] == (1, "GATE W5 FAIL rc=1 (check did not run: SepRpcTranscript not run)")


def test_w5_pass_and_fail_with_reference(tmp_path):
    assert w5(tmp_path, W5_PASS, 0, 1)[:2] == (0, "GATE W5 PASS")
    text = W5_PASS.replace("interface_freeze: PASS", "FAIL symbols_macos_off.txt\ninterface_freeze: FAIL").replace(
        "SepInterfaceFreeze ...............   Passed", "SepInterfaceFreeze ...............***Failed")
    assert w5(tmp_path, text, 8, 1)[:2] == (1, "GATE W5 FAIL rc=1 (mismatch: FAIL symbols_macos_off.txt)")


def test_w5_missing_reference_without_transcript_output(tmp_path):
    # --output-on-failure: a passing SepRpcTranscript prints only its result line
    text = W5_MISSING_REF_ONLY.replace("rpc_transcript: PASS (40 entries match off.json)\n", "")
    assert w5(tmp_path, text, 8, 0)[:2] == (2, "GATE W5 FAIL rc=2 (missing reference -- not an interface mismatch)")


def test_w5_transcript_failed_with_missing_reference(tmp_path):
    text = W5_MISSING_REF_ONLY.replace("SepRpcTranscript .................   Passed",
                                       "SepRpcTranscript .................***Failed").replace(
        "rpc_transcript: PASS (40 entries match off.json)",
        "-  \"height\": 1\n+  \"height\": 2\nrpc_transcript: FAIL transcript differs from /x/off.json")
    assert w5(tmp_path, text, 8, 0)[:2] == (1, "GATE W5 FAIL rc=1 (transcript mismatch (symbol reference also missing))")


# Round 4 (I1): "missing reference" / "vector mismatch" need positive evidence
# that the comparison ran; crashed, empty or not-run shapes are "did not run".
K4_TRACEBACK = """\
Traceback (most recent call last):
  File "/x/run_consensus_vectors.py", line 188, in <module>
    sys.exit(main())
KeyError: 'tests'
"""


def test_k4_traceback_without_reference_is_not_missing_reference(tmp_path):
    o, u = k4files(tmp_path, K4_TRACEBACK)
    rc, line, gh = run(tmp_path, "k4", output=o, unresolved=u, rc=1, reference_present=0)
    assert (rc, line) == (1, "GATE K4 FAIL rc=1 (check did not run: run_consensus_vectors.py traceback (rc=1))")
    assert gh == ""


def test_k4_traceback_with_reference_is_not_a_vector_mismatch(tmp_path):
    o, u = k4files(tmp_path, K4_TRACEBACK)
    assert run(tmp_path, "k4", output=o, unresolved=u, rc=1, reference_present=1)[:2] == (
        1, "GATE K4 FAIL rc=1 (check did not run: run_consensus_vectors.py traceback (rc=1))")


def test_k4_traceback_after_mismatch_lines_is_not_a_verdict(tmp_path):
    o, u = k4files(tmp_path, "DIGEST_MISMATCH MerkleGolden\n" + K4_TRACEBACK)
    rc, line, gh = run(tmp_path, "k4", output=o, unresolved=u, rc=1, reference_present=0)
    assert rc == 1 and "check did not run" in line and gh == ""


def test_k4_empty_output_is_not_a_verdict(tmp_path):
    for ref in (0, 1):
        for vrc in (0, 1):
            o, u = k4files(tmp_path, "")
            rc, line, gh = run(tmp_path, "k4", output=o, unresolved=u, rc=vrc, reference_present=ref)
            assert (rc, line, gh) == (1, "GATE K4 FAIL rc=1 (check did not run: unrecognized output rc=%d)" % vrc, "")


def test_k4_rc_contradicting_output_is_not_a_verdict(tmp_path):
    # rc 1 with only an OK line, rc 0 with a mismatch line
    o, u = k4files(tmp_path, OK_OUT)
    assert run(tmp_path, "k4", output=o, unresolved=u, rc=1, reference_present=0)[:2] == (
        1, "GATE K4 FAIL rc=1 (check did not run: unrecognized output rc=1)")
    o, u = k4files(tmp_path, "DIGEST_MISMATCH MerkleGolden\n")
    assert run(tmp_path, "k4", output=o, unresolved=u, rc=0, reference_present=1)[:2] == (
        1, "GATE K4 FAIL rc=1 (check did not run: unrecognized output rc=0)")


def test_k4_mismatch_lines_without_reference_is_missing_reference(tmp_path):
    # informational comparison against the macOS reference differed
    o, u = k4files(tmp_path, "DIGEST_MISMATCH MerkleGolden\n")
    rc, line, gh = run(tmp_path, "k4", output=o, unresolved=u, rc=1, reference_present=0)
    assert (rc, line, gh) == (2, "GATE K4 FAIL rc=2 (missing reference -- not a vector mismatch)",
                              "note=missing reference\n")


W5_FREEZE_TRACEBACK = W5_MISSING_REF_ONLY.replace(
    "interface_freeze: INPUT ERROR: no baseline /x/symbols_linux_off.txt (record it at the baseline for this platform/config)\n",
    "Traceback (most recent call last):\n  File \"/x/interface_freeze.py\", line 9, in current\nOSError: [Errno 2] nm\n")


def test_w5_crashed_freeze_with_transcript_pass_is_not_missing_reference(tmp_path):
    rc, line, gh = w5(tmp_path, W5_FREEZE_TRACEBACK, 8, 0)
    assert (rc, line) == (8, "GATE W5 FAIL rc=8 (check did not run: Python traceback in the ctest log)")
    assert gh == ""


def test_w5_crashed_freeze_without_traceback_is_not_missing_reference(tmp_path):
    # freeze died before comparing (no PASS lines, no no-baseline error)
    text = W5_MISSING_REF_ONLY
    for l in ("PASS nodecore_ffi.h\n", "PASS wallet_ffi.h\n", "PASS jni_exports.txt\n"):
        text = text.replace(l, "")
    text = text.replace(
        "interface_freeze: INPUT ERROR: no baseline /x/symbols_linux_off.txt (record it at the baseline for this platform/config)\n",
        "Segmentation fault\n")
    rc, line, gh = w5(tmp_path, text, 8, 0)
    assert rc == 8 and line.startswith("GATE W5 FAIL rc=8 (check did not run: interface_freeze did not reach")
    assert "no nodecore_ffi.h" in line and "no no-baseline error" in line and gh == ""


def test_w5_missing_reference_needs_every_item_pass(tmp_path):
    text = W5_MISSING_REF_ONLY.replace("PASS jni_exports.txt\n", "")
    rc, line, gh = w5(tmp_path, text, 8, 0)
    assert (rc, line, gh) == (8, "GATE W5 FAIL rc=8 (check did not run: interface_freeze did not reach the "
                                 "symbol comparison (no jni_exports.txt))", "")


def test_w5_freeze_not_run_is_not_missing_reference(tmp_path):
    for ref in (0, 1):
        text = W5_MISSING_REF_ONLY.replace("SepInterfaceFreeze ...............***Failed",
                                           "SepInterfaceFreeze ...............***Not Run")
        rc, line, gh = w5(tmp_path, text, 8, ref)
        assert (rc, line, gh) == (8, "GATE W5 FAIL rc=8 (check did not run: SepInterfaceFreeze ***Not Run)", "")


def test_w5_transcript_not_run_is_not_a_transcript_verdict(tmp_path):
    text = W5_MISSING_REF_ONLY.replace("SepRpcTranscript .................   Passed",
                                       "SepRpcTranscript .................***Not Run")
    assert w5(tmp_path, text, 8, 0)[:2] == (8, "GATE W5 FAIL rc=8 (check did not run: SepRpcTranscript ***Not Run)")


def test_w5_other_no_baseline_error_is_not_missing_reference(tmp_path):
    text = W5_MISSING_REF_ONLY.replace("/x/symbols_linux_off.txt", "/x/wallet_ffi.h")
    rc, line, gh = w5(tmp_path, text, 8, 0)
    assert rc == 8 and "check did not run: interface_freeze: INPUT ERROR: no baseline /x/wallet_ffi.h" in line
    assert gh == ""


def test_w5_run1_linux_log_shape_is_missing_reference(tmp_path):
    # verbatim (timestamps stripped) from PR #860 run 1, linux-gcc15 orchard=off
    text = """\
ctest -R '^(SepInterfaceFreeze|SepRpcTranscript)$'
Test project /home/runner/work/dinero-v8/dinero-v8/build-sep
    Start 970: SepInterfaceFreeze
1/2 Test #970: SepInterfaceFreeze ...............***Failed    0.21 sec
[1/1] Building Bulletproofs FFI library (Rust)...
    Finished `release` profile [optimized] target(s) in 0.02s
interface_freeze: INPUT ERROR: no baseline /home/runner/work/dinero-v8/dinero-v8/tests/separation/interface_baseline/symbols_linux_off.txt (record it at the baseline for this platform/config)
PASS nodecore_ffi.h
PASS wallet_ffi.h
PASS jni_exports.txt
CMake Error at /home/runner/work/dinero-v8/dinero-v8/tests/separation/run_built_script.cmake:26 (message):
  SEPARATION TEST FAILED:
  /opt/hostedtoolcache/Python/3.12.15/x64/bin/python3.12;/home/runner/work/dinero-v8/dinero-v8/tests/separation/interface_freeze.py;check (rc=2)



    Start 971: SepRpcTranscript
2/2 Test #971: SepRpcTranscript .................   Passed   50.70 sec

50% tests passed, 1 tests failed out of 2

The following tests FAILED:
\t970 - SepInterfaceFreeze (Failed)                       separation
Errors while running CTest
"""
    assert w5(tmp_path, text, 8, 0) == (2, "GATE W5 FAIL rc=2 (missing reference -- not an interface mismatch)",
                                        "note=missing reference\n")
    # the same shape with CRLF line endings (Windows Tee-Object)
    assert w5(tmp_path, text.replace("\n", "\r\n"), 8, 0)[:2] == (
        2, "GATE W5 FAIL rc=2 (missing reference -- not an interface mismatch)")


def test_gate_line_is_ascii(tmp_path):
    o, u = k4files(tmp_path, OK_OUT)
    line = run(tmp_path, "k4", output=o, unresolved=u, rc=0, reference_present=0)[1]
    assert line.isascii() and " -- " in line
    line = w5(tmp_path, W5_MISSING_REF_ONLY, 8, 0)[1]
    assert line.isascii() and " -- " in line


def test_w5_transcript_failed_without_verdict_is_not_a_mismatch(tmp_path):
    # SepRpcTranscript ***Failed with no FAIL line (crashed, killed, unrecognized)
    text = W5_MISSING_REF_ONLY.replace("SepRpcTranscript .................   Passed",
                                       "SepRpcTranscript .................***Failed").replace(
        "rpc_transcript: PASS (40 entries match off.json)\n", "")
    rc, line, gh = w5(tmp_path, text, 8, 0)
    assert (rc, line, gh) == (8, "GATE W5 FAIL rc=8 (check did not run: SepRpcTranscript failed without a "
                                 "transcript verdict)", "")


# Round 5. N1: with the reference present, PASS needs rc 0 AND both ctest
# results Passed AND no FAIL line; rc 0 against a contrary log is not PASS.
W5_FREEZE_FAILED_REF = W5_PASS.replace(
    "SepInterfaceFreeze ...............   Passed", "SepInterfaceFreeze ...............***Failed").replace(
    "interface_freeze: PASS", "FAIL symbols_macos_off.txt\ninterface_freeze: FAIL")


def test_w5_rc0_with_failed_freeze_is_not_pass(tmp_path):
    rc, line, gh = w5(tmp_path, W5_FREEZE_FAILED_REF, 0, 1)
    assert (rc, gh) == (1, "") and line.startswith("GATE W5 FAIL rc=1 (check did not run: log contradicts rc 0")


def test_w5_rc0_with_failed_freeze_and_no_body_is_not_pass(tmp_path):
    text = W5_PASS.replace("SepInterfaceFreeze ...............   Passed",
                           "SepInterfaceFreeze ...............***Failed").replace("interface_freeze: PASS\n", "")
    assert w5(tmp_path, text, 0, 1)[0] == 1


def test_w5_rc0_with_fail_lines_under_passed_results_is_not_pass(tmp_path):
    text = W5_PASS.replace("rpc_transcript: PASS (40 entries match off.json)",
                           "rpc_transcript: FAIL transcript differs from /x/off.json")
    assert w5(tmp_path, text, 0, 1)[:2] == (
        1, "GATE W5 FAIL rc=1 (check did not run: log contradicts rc 0 (SepInterfaceFreeze Passed, "
           "SepRpcTranscript Passed))")


# N2: with the reference present, a FAIL needs the failed test's verdict line.
def test_w5_ref_failed_freeze_without_verdict_is_not_a_measured_fail(tmp_path):
    text = W5_PASS.replace("SepInterfaceFreeze ...............   Passed",
                           "SepInterfaceFreeze ...............***Failed").replace(
        "interface_freeze: PASS", "Segmentation fault")
    assert w5(tmp_path, text, 8, 1)[:2] == (
        8, "GATE W5 FAIL rc=8 (check did not run: SepInterfaceFreeze failed without an interface verdict)")


def test_w5_ref_failed_transcript_without_verdict_is_not_a_measured_fail(tmp_path):
    text = W5_PASS.replace("SepRpcTranscript .................   Passed",
                           "SepRpcTranscript .................***Failed").replace(
        "rpc_transcript: PASS (40 entries match off.json)\n", "")
    assert w5(tmp_path, text, 8, 1)[:2] == (
        8, "GATE W5 FAIL rc=8 (check did not run: SepRpcTranscript failed without a transcript verdict)")


def test_w5_ref_transcript_mismatch_is_labelled(tmp_path):
    text = W5_PASS.replace("SepRpcTranscript .................   Passed",
                           "SepRpcTranscript .................***Failed").replace(
        "rpc_transcript: PASS (40 entries match off.json)",
        "rpc_transcript: FAIL transcript differs from /x/off.json")
    assert w5(tmp_path, text, 8, 1)[:2] == (
        1, "GATE W5 FAIL rc=1 (mismatch: rpc_transcript: FAIL transcript differs from /x/off.json)")


def test_w5_ref_rc_nonzero_with_both_passed_is_not_a_measured_fail(tmp_path):
    assert w5(tmp_path, W5_PASS, 8, 1)[:2] == (
        8, "GATE W5 FAIL rc=8 (check did not run: log contradicts rc 8 (both tests Passed))")


# N3: note=missing reference (and exit 2) only for the missing-reference labels.
def test_rc2_did_not_run_is_not_noted_as_missing_reference(tmp_path):
    text = W5_MISSING_REF_ONLY.replace("SepInterfaceFreeze ...............***Failed",
                                       "SepInterfaceFreeze ...............***Not Run")
    rc, line, gh = w5(tmp_path, text, 2, 0)
    assert (rc, line, gh) == (1, "GATE W5 FAIL rc=1 (check did not run: SepInterfaceFreeze ***Not Run)", "")
    o, u = k4files(tmp_path, "NOT_RUN BIP341SighashVectors\n")
    rc, line, gh = run(tmp_path, "k4", output=o, unresolved=u, rc=2, reference_present=0)
    assert (rc, line, gh) == (1, "GATE K4 FAIL rc=1 (check did not run: NOT_RUN BIP341SighashVectors)", "")


# Owner decisions 2, item 1: Windows W5 is PARTIAL by platform (NodeCore is not
# a Windows target). A pass is never a bare PASS there; POSIX is unchanged.
W5_PARTIAL_LINE = "GATE W5 PASS (partial: wallet FFI + RPC only; NodeCore N/A on Windows)"
W5_WINDOWS_MISSING_REF_ONLY = """\
1/2 Test #866: SepInterfaceFreeze ...............***Failed    1.82 sec
N/A nodecore_ffi.h (NodeCore not a Windows target)
N/A jni_exports.txt (NodeCore not a Windows target)
N/A nodecore_ffi symbols (NodeCore not a Windows target)
PASS wallet_ffi.h
interface_freeze: INPUT ERROR: no baseline D:/a/tests/separation/interface_baseline/symbols_windows_off.txt (record it at the baseline for this platform/config)
2/2 Test #867: SepRpcTranscript .................   Passed   58.21 sec
""".replace("\n", "\r\n")   # Windows Tee-Object writes CRLF


def test_w5_windows_pass_is_labelled_partial(tmp_path):
    rc, line, gh = w5(tmp_path, W5_PASS, 0, 1, "windows")
    assert (rc, line, gh) == (0, W5_PARTIAL_LINE, "")
    assert line.isascii()


def test_w5_posix_pass_stays_bare(tmp_path):
    for plat in ("linux", "macos"):
        assert w5(tmp_path, W5_PASS, 0, 1, plat)[:2] == (0, "GATE W5 PASS")


def test_w5_platform_is_required(tmp_path):
    log = tmp_path / "w5.log"
    log.write_text(W5_PASS)
    r = subprocess.run([sys.executable, str(SCRIPT), "w5", "--log", str(log), "--rc", "0",
                        "--reference-present", "1"], capture_output=True, text=True)
    assert r.returncode == 2 and "--platform" in r.stderr and "GATE" not in r.stdout


def test_w5_windows_missing_reference_needs_wallet_pass_and_nodecore_na(tmp_path):
    rc, line, gh = w5(tmp_path, W5_WINDOWS_MISSING_REF_ONLY, 8, 0, "windows")
    assert (rc, line, gh) == (2, "GATE W5 FAIL rc=2 (missing reference -- not an interface mismatch)",
                              "note=missing reference\n")
    (tmp_path / "gh.txt").unlink()
    text = W5_WINDOWS_MISSING_REF_ONLY.replace("N/A jni_exports.txt (NodeCore not a Windows target)\r\n", "")
    rc, line, gh = w5(tmp_path, text, 8, 0, "windows")
    assert (rc, gh) == (8, "") and line == ("GATE W5 FAIL rc=8 (check did not run: interface_freeze did not "
                                            "reach the symbol comparison (no N/A jni_exports.txt))")


def test_w5_windows_nodecore_item_checked_is_a_contradiction(tmp_path):
    # the POSIX run-1 shape (PASS nodecore_ffi.h / jni_exports.txt) on Windows
    rc, line, gh = w5(tmp_path, W5_MISSING_REF_ONLY, 8, 0, "windows")
    assert (rc, gh) == (8, "") and line.startswith(
        "GATE W5 FAIL rc=8 (check did not run: NodeCore item checked on Windows (PASS nodecore_ffi.h;")
    text = W5_PASS.replace("SepInterfaceFreeze ...............   Passed",
                           "SepInterfaceFreeze ...............***Failed").replace(
        "interface_freeze: PASS", "FAIL jni_exports.txt\ninterface_freeze: FAIL")
    rc, line, _ = w5(tmp_path, text, 8, 1, "windows")
    assert rc == 8 and "NodeCore item checked on Windows (FAIL jni_exports.txt" in line


def test_w5_posix_nodecore_na_is_a_contradiction(tmp_path):
    # NodeCore checks stay required on Linux/macOS: the partial shape there is "did not run"
    for plat in ("linux", "macos"):
        rc, line, gh = w5(tmp_path, W5_WINDOWS_MISSING_REF_ONLY, 8, 0, plat)
        assert (rc, gh) == (8, "") and line.startswith(
            "GATE W5 FAIL rc=8 (check did not run: NodeCore reported N/A on %s" % plat)


def test_w5_posix_nodecore_build_failure_is_still_did_not_run(tmp_path):
    # selection is by platform, not by "nodecore_ffi failed to build"
    text = """\
1/2 Test #970: SepInterfaceFreeze ...............***Failed    1.82 sec
FAILED: [code=1] CMakeFiles/nodecore_ffi.dir/src/nodecore/nodecore_ffi.cpp.o
CMake Error at /x/tests/separation/run_built_script.cmake:21 (message):
  SEPARATION TEST BUILD FAILED: nodecore_ffi;dinero_wallet_ffi (rc=1)
2/2 Test #971: SepRpcTranscript .................   Passed   50.70 sec
"""
    for ref in (0, 1):
        rc, line, gh = w5(tmp_path, text, 8, ref, "linux")
        assert (rc, line, gh) == (8, "GATE W5 FAIL rc=8 (check did not run: build failed: "
                                     "nodecore_ffi;dinero_wallet_ffi (rc=1))", "")


def test_w5_windows_failures_are_never_partial_pass(tmp_path):
    # run-1 Windows shape (B1: dinerod exits at startup): FAIL, no partial PASS
    rc, line, gh = w5(tmp_path, W5_WINDOWS_RUN1, 8, 0, "windows")
    assert rc == 8 and line.startswith("GATE W5 FAIL rc=8 (check did not run: build failed") and gh == ""
    text = W5_WINDOWS_MISSING_REF_ONLY.replace(
        "SepRpcTranscript .................   Passed", "SepRpcTranscript .................***Failed") + \
        "rpc_transcript: RUN ERROR: s exited rc=1 during startup\r\n"
    rc, line, gh = w5(tmp_path, text, 8, 0, "windows")
    assert rc == 8 and "check did not run: rpc_transcript: RUN ERROR: s exited rc=1" in line and gh == ""
