"""Text checks of the separation-gate workflow wiring (owner decisions 2).

actionlint cannot see these: every build-dependent gate consults the
prerequisite status first, K2/W2/K5 never do, the summary carries every
UNMEASURED state, and Windows W5 is partial (no nodecore_ffi, never bare PASS).
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
POSIX = (ROOT / ".github/workflows/separation-gates.yml").read_text()
WINDOWS = (ROOT / ".github/workflows/separation-gates-windows.yml").read_text()
CMAKE = (ROOT / "tests/separation/CMakeLists.txt").read_text()
PREREQ_GATES = ("K1", "K3", "K4", "W1", "W3", "W4", "W5", "W6")
NO_BUILD_GATES = ("K2", "W2", "K5")


def steps(text):
    """{step id: step text} of the gates job."""
    job = text.split("\n  k4-compact-regtest:")[0]
    out = {}
    for block in job.split("\n      - name: ")[1:]:
        m = re.search(r"^        id: (\S+)$", block, re.M)
        if m:
            out[m.group(1)] = block
    return out


def order(text):
    return [i for i in steps(text)]


def first_command(block, source):
    run = block.split("run: |", 1)[1]
    lines = [l.strip() for l in run.splitlines() if l.strip() and not l.strip().startswith("#")]
    return lines[lines.index(source) + 1]


def test_posix_gates_consult_prerequisite_status_first():
    st = steps(POSIX)
    for g in PREREQ_GATES:
        assert first_command(st[g.lower()], '. "$RUNNER_TEMP/sep_gate.sh"') == "sep_prereq %s || exit $?" % g
    for g in NO_BUILD_GATES:
        assert "sep_prereq" not in st[g.lower()]


def test_windows_gates_consult_prerequisite_status_first():
    st = steps(WINDOWS)
    for g in PREREQ_GATES:
        assert first_command(st[g.lower()], '. "$env:RUNNER_TEMP\\sep_gate.ps1"') == (
            "$p = [int](Test-SepPrereq '%s'); if ($p -ne 0) { exit $p }" % g)
    for g in ("K2", "W2"):
        assert "Test-SepPrereq" not in st[g.lower()]
    # pwsh: native output inside a function would become its return value
    assert "--github-output $env:GITHUB_OUTPUT | Out-Host\n            return $LASTEXITCODE" in WINDOWS


def test_prerequisite_scan_runs_after_build_before_gates():
    for text, plat in ((POSIX, '"$SEP_PLAT"'), (WINDOWS, "windows")):
        ids = order(text)
        assert ids.index("build") < ids.index("prereq") < ids.index("k1")
        block = steps(text)["prereq"]
        assert "O_BUILD: ${{ steps.build.outcome }}" in block
        assert "prereq_status.py scan" in block and "--platform " + plat in block
        assert "continue-on-error: true" in block


def test_summary_carries_every_unmeasured_state_and_the_lane_line():
    for text in (POSIX, WINDOWS):
        summary = text.split("- name: Gate summary", 1)[1]
        for g in PREREQ_GATES:
            assert "S_%s: ${{ steps.%s.outputs.state }}" % (g, g.lower()) in summary
            assert "Y_%s: ${{ steps.%s.outputs.why }}" % (g, g.lower()) in summary
        assert "prereq_status.py lane" in summary


def test_windows_w5_is_partial_and_never_builds_nodecore():
    st = steps(WINDOWS)
    assert "--platform windows" in st["w5"]
    assert "--platform windows" in st["build"]
    record = WINDOWS.split("- name: Record baselines for review", 1)[1].split("- name: Gate summary")[0]
    assert "nodecore_ffi.lib" not in record and "--nodecore-ffi" not in record
    summary = WINDOWS.split("- name: Gate summary", 1)[1]
    assert "'PASS (partial: wallet FFI + RPC only; NodeCore N/A on Windows)'" in summary
    assert '--platform "$SEP_PLAT"' in steps(POSIX)["w5"]


def test_cmake_selects_windows_w5_scope_by_platform():
    win = CMAKE.split("if(WIN32 AND TARGET dinero_wallet_ffi)", 1)[1].split("elseif(", 1)[0]
    code = "\n".join(l for l in win.splitlines() if not l.strip().startswith("#"))
    assert "-DSEP_TARGETS=dinero_wallet_ffi\n" in code and "nodecore" not in code
    posix = CMAKE.split("elseif(NOT WIN32 AND TARGET nodecore_ffi AND TARGET dinero_wallet_ffi)", 1)[1]
    assert "-DSEP_TARGETS=nodecore_ffi|dinero_wallet_ffi" in posix.split("else()", 1)[0]
