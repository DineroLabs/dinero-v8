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


# Fix round: decision 3 (why= in every row) and minors 3/5
def test_summary_rows_always_carry_a_reason():
    for text, gates in ((POSIX, ("K1", "K2", "K3", "K4", "K5", "W1", "W2", "W3", "W4", "W5", "W6")),
                        (WINDOWS, ("K1", "K2", "K3", "K4", "W1", "W2", "W3", "W4", "W5", "W6"))):
        summary = text.split("- name: Gate summary", 1)[1].split("\n  k4-compact-regtest:")[0]
        for g in gates:
            assert "Y_%s: ${{ steps.%s.outputs.why }}" % (g, g.lower()) in summary
        assert "no reason recorded" in summary
        assert "N_K4" not in summary and "N_W5" not in summary   # the reason comes from why=
    assert 'case "$r" in PASS|NOT_RUN*) ;; *) r="$r (${!y:-no reason recorded})" ;; esac' in POSIX
    assert "if ($r -eq 'FAIL' -or $r -eq 'UNMEASURED') {" in WINDOWS
    compact = POSIX.split("\n  k4-compact-regtest:")[1]
    assert 'Y_K4C: ${{ steps.k4c.outputs.why }}' in compact and "no reason recorded" in compact


def test_gate_results_write_why_on_failure():
    assert 'echo "why=rc=$2: ${4:-see the step log}${3:+ ($3)}" >> "$GITHUB_OUTPUT"' in POSIX
    assert '"why=rc=${Rc}: $What$suffix" | Out-File -FilePath $env:GITHUB_OUTPUT' in WINDOWS
    assert POSIX.count("printf 'state=UNMEASURED\\nwhy=%s\\n'") == 2      # K3, W3
    assert '"why=$reason" | Out-File' in WINDOWS                           # COFF scan UNMEASURED


def test_build_step_records_the_harness_state_for_the_prereq_scan():
    for text, rcw in ((POSIX, 'echo "$trc" > "$RUNNER_TEMP/sep_targets_rc.txt"'),
                      (WINDOWS, '"$trc" | Set-Content -Path (Join-Path $env:RUNNER_TEMP \'sep_targets_rc.txt\')')):
        build = steps(text)["build"]
        assert "--k4-out" in build and rcw in build
        # written before the early exit on a sep_targets.py failure
        assert build.index(rcw) < build.index("exit")
        prereq = steps(text)["prereq"]
        assert "--sep-targets-rc" in prereq and "--k4-targets" in prereq


def test_windows_expectation_keeps_w5_missing_reference_after_b1():
    summary = WINDOWS.split("- name: Gate summary", 1)[1]
    exp = summary.split("Stage-0 expectation:", 1)[1].split("\n")[0]
    assert "After B1 is fixed through its own review, W5 stays FAIL rc 2 (missing reference)" in exp
    assert "symbols_windows_<cfg>.txt" in exp
    assert "'" not in exp[:-1]      # single-quoted pwsh string: no stray quote inside


def test_no_step_saves_a_cache():
    # Owner rule: cache saves are uploads. Restoring existing caches is allowed.
    for name, text in (("posix", POSIX), ("windows", WINDOWS)):
        uses = re.findall(r"^\s*uses:\s*(\S+)", text, re.M)
        assert not [u for u in uses if re.match(r"actions/(cache@|cache/save@|upload-artifact@)", u)], name
        for block in text.split("\n      - name: ")[1:]:
            if "hendrikmuhs/ccache-action@" in block:
                assert re.search(r"^\s+save:\s*false\s*$", block, re.M), name
            if re.search(r"uses:\s*actions/setup-(python|node|go|java)@", block):
                assert not re.search(r"^\s+cache:", block, re.M), name
    assert "actions/cache/restore@" in POSIX and "actions/cache/restore@" in WINDOWS


def test_macos_lane_is_macos15_with_pinned_xcode_and_its_own_reference_key():
    # macos-14 images retire 2026-11-02 (actions/runner-images#13518).
    matrix = POSIX.split("    strategy:", 1)[1].split("    env:", 1)[0]
    assert not re.search(r"^\s*(- )?os: .*macos-14", matrix, re.M)
    assert "os: [ubuntu-latest, ubuntu-24.04, macos-15]" in matrix
    mac = matrix.split("- os: macos-15", 1)[1]
    assert re.search(r"^\s+plat: macos$", mac, re.M)
    assert re.search(r"^\s+ref_tag: macos15$", mac, re.M)
    assert re.search(r"^\s+xcode: '16\.4'$", mac, re.M)
    for os_ in ("ubuntu-latest", "ubuntu-24.04"):
        lane = matrix.split("- os: %s\n" % os_, 1)[1].split("- os:", 1)[0]
        assert re.search(r"^\s+ref_tag: linux$", lane, re.M), os_
    assert "SEP_REF_TAG: ${{ matrix.ref_tag }}" in POSIX
    deps = POSIX.split("- name: Install macOS build dependencies", 1)[1].split("\n      - name: ", 1)[0]
    assert 'sudo xcode-select -s "/Applications/Xcode_${XCODE}.app"' in deps
    assert 'test "$(xcodebuild -version | head -1)" = "Xcode ${XCODE}"' in deps


def test_reference_paths_use_the_reference_key_not_the_platform():
    st = steps(POSIX)
    assert 'if [ "$SEP_REF_TAG" = macos ]; then ref="$d/consensus_vectors_ref_$SEP_CFG.json"' in st["k4"]
    assert 'else ref="$d/consensus_vectors_ref_${SEP_REF_TAG}_$SEP_CFG.json"; fi' in st["k4"]
    assert 'base="tests/separation/interface_baseline/symbols_${SEP_REF_TAG}_${SEP_CFG}.txt"' in st["w5"]
    rec = POSIX.split("- name: Record baselines for review", 1)[1].split("\n      - name: ", 1)[0]
    assert "SEP_PLAT" not in rec
    assert '--record "$out/consensus_vectors_ref_${SEP_REF_TAG}_$SEP_CFG.json"' in rec


def test_macos14_era_references_are_preserved():
    # Recorded locally (AppleClang 21 / Xcode 27, macOS 27.0.1) and matched by the
    # macos-14 / Xcode 15.4 lane in run 2. Kept byte-for-byte with their provenance.
    import hashlib, json
    pins = {
        "scripts/ci/separation/consensus_vectors_ref_off.json": "466af11bf3dfdf5666b4ce87b7738c640c486d405b16b5dc311ef5aa9fdcea0a",
        "scripts/ci/separation/consensus_vectors_ref_on.json": "78dbd04d6d28a5daf792ebcf363d07f341e83607cee2fbc780afeab99d278de6",
        "tests/separation/interface_baseline/symbols_macos_off.txt": "2adc40d15e990f33dd9975e891ac0c68e0fa01a3b53e1726e32335a4e5246feb",
        "tests/separation/interface_baseline/symbols_macos_on.txt": "2adc40d15e990f33dd9975e891ac0c68e0fa01a3b53e1726e32335a4e5246feb",
    }
    for rel, digest in pins.items():
        data = (ROOT / rel).read_bytes().replace(b"\r\n", b"\n")
        assert hashlib.sha256(data).hexdigest() == digest, rel
    for cfg, sha in (("off", "33d1ca772ebf1f26d538cbd0fc47d1b8f189bb5d"),
                     ("on", "6e57af6150e990c02aa53d2772ec476bd219585f")):
        ref = json.loads((ROOT / ("scripts/ci/separation/consensus_vectors_ref_%s.json" % cfg)).read_text())
        assert ref["provenance_sha"] == sha
