"""prereq_status.py: UNMEASURED gates on a prerequisite build failure (owner decisions 2, item 2)."""
import json
import subprocess
import sys
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent.parent / "prereq_status.py"

# Build-order dependencies as the real codemodel-v2 reply records them
# (checked against the macOS off/on build dirs): every base target and probe
# depends on dinero_consensus; dinerod, the FFI archives, the wallet probe and
# the W4 checker also on dinero_wallet.
GRAPH = {
    "dinero_crypto": ([], "libdinero_crypto.a"),
    "dinero_consensus": (["dinero_crypto"], "libdinero_consensus.a"),
    "dinero_wallet": (["dinero_consensus"], "src/wallet/libdinero_wallet.a"),
    "dinerod": (["dinero_wallet", "dinero_consensus"], "dinerod"),
    "dinero_wallet_ffi": (["dinero_wallet"], "libdinero_wallet_ffi.a"),
    "nodecore_ffi": (["dinero_wallet"], "libnodecore_ffi.a"),
    "sep_consensus_forced_link": (["dinero_consensus"], "tests/separation/sep_consensus_forced_link"),
    "sep_wallet_forced_link": (["dinero_wallet"], "tests/separation/sep_wallet_forced_link"),
    "sep_wallet_fixture_compat": (["dinero_wallet"], "tests/separation/sep_wallet_fixture_compat"),
    "sep_wallet_ffi_standalone": (["dinero_wallet_ffi", "dinero_wallet", "dinero_consensus"],
                                  "tests/separation/sep_wallet_ffi_standalone"),
    # K4 test executables (real codemodel: DAAGoldenVectors, MerkleGolden and
    # HeaderPoWVerification also depend on dinero_wallet)
    "test_bip341_sighash_vectors": (["dinero_consensus"], "tests/test_bip341_sighash_vectors"),
    "test_daa_golden_vectors": (["dinero_consensus", "dinero_wallet"], "tests/test_daa_golden_vectors"),
}
K4_TARGETS = "test_bip341_sighash_vectors\ntest_daa_golden_vectors\n"
WINDOWS_ART = {"dinerod": ["dinerod.exe", "dinerod.pdb"]}


def codemodel(build, windows=False):
    reply = build / ".cmake" / "api" / "v1" / "reply"
    reply.mkdir(parents=True)
    targets = []
    for name, (deps, art) in GRAPH.items():
        arts = [art]
        if windows:
            arts = WINDOWS_ART.get(name) or [art.replace("lib", "", 1).replace(".a", ".lib")
                                              if art.endswith(".a") else art + ".exe"]
        tj = {"id": name + "::@1", "name": name,
              "dependencies": [{"id": d + "::@1"} for d in deps],
              "artifacts": [{"path": a} for a in arts]}
        (reply / ("target-%s.json" % name)).write_text(json.dumps(tj))
        targets.append({"name": name, "id": name + "::@1", "jsonFile": "target-%s.json" % name})
    (reply / "codemodel-v2-x.json").write_text(json.dumps(
        {"version": {"major": 2, "minor": 9}, "configurations": [{"name": "Release", "targets": targets}]}))
    (reply / "index-1.json").write_text(json.dumps(
        {"reply": {"codemodel-v2": {"kind": "codemodel", "jsonFile": "codemodel-v2-x.json"}}}))
    return {n: art for n, (_, art) in GRAPH.items()}


def built(build, paths):
    for p in paths:
        (build / p).parent.mkdir(parents=True, exist_ok=True)
        (build / p).write_text("")


def cli(*args):
    r = subprocess.run([sys.executable, str(SCRIPT)] + [str(a) for a in args],
                       capture_output=True, text=True)
    return r.returncode, r.stdout.strip(), r.stderr.strip()


def scan_cli(tmp_path, outcome, plat, sep_rc="0", k4=K4_TARGETS):
    out = tmp_path / "prereq.json"
    rcf, k4f = tmp_path / "sep_targets_rc.txt", tmp_path / "k4_targets.txt"
    for f, v in ((rcf, sep_rc), (k4f, k4)):
        if f.exists():
            f.unlink()
        if v is not None:
            f.write_text(v)
    rc, stdout, err = cli("scan", "--build", tmp_path / "build", "--build-outcome", outcome,
                          "--platform", plat, "--out", out, "--sep-targets-rc", rcf, "--k4-targets", k4f)
    return rc, stdout, err, out


def scan(tmp_path, outcome, plat, **kw):
    rc, stdout, err, out = scan_cli(tmp_path, outcome, plat, **kw)
    assert rc == 0, err
    return json.loads(out.read_text()), stdout, out


def gate(status, gid, gh=None):
    args = ["gate", "--status", status, "--gate", gid]
    if gh:
        args += ["--github-output", gh]
    return cli(*args)


def test_run1_macos_lane_is_unmeasured(tmp_path):
    # PR #860 run 1, macos-arm64 orchard=off (gates_macos-arm64_orchard_off.log):
    # build step line 4452 "FAILED: ... dinero_consensus.dir/src/consensus/
    # global_utxo_set_impl.cpp.o" (+ dinero_core, dinero_rpc_handlers, dinerod
    # objects); "[75/661] Linking CXX static library libdinero_crypto.a" is
    # the only one of these targets that linked; no link line for
    # dinero_wallet, dinerod or sep_wallet_fixture_compat; "ninja: build
    # stopped", step outcome failure.
    b = tmp_path / "build"
    codemodel(b)
    built(b, ["libdinero_crypto.a"])
    st, stdout, out = scan(tmp_path, "failure", "macos")
    lane = ("LANE UNMEASURED: prerequisite build failed: "
            "dinero_consensus, dinero_wallet, dinerod, sep_wallet_fixture_compat")
    assert st["failed"] == ["dinero_consensus", "dinero_wallet", "dinerod", "sep_wallet_fixture_compat"]
    assert lane in stdout.splitlines()
    want = {
        "K1": "dinero_consensus", "K3": "dinero_consensus",
        "K4": "dinero_consensus, dinero_wallet",   # via test_daa_golden_vectors
        "W1": "dinero_consensus, dinero_wallet", "W3": "dinero_consensus, dinero_wallet",
        "W4": "dinero_consensus, dinero_wallet, sep_wallet_fixture_compat",
        "W5": "dinero_consensus, dinero_wallet, dinerod",
        "W6": "dinero_consensus, dinero_wallet",
    }
    for gid, targets in want.items():
        gh = tmp_path / ("gh-%s.txt" % gid)
        rc, line, _ = gate(out, gid, gh)
        why = "prerequisite build failed: " + targets
        assert (rc, line) == (3, "GATE %s UNMEASURED (%s)" % (gid, why))
        assert gh.read_text() == "state=UNMEASURED\nwhy=%s\n" % why
    # K2/W2 (closure, no build) and K5 still run and report their real result
    for gid in ("K2", "W2", "K5"):
        assert gate(out, gid) == (0, "", "")
    assert cli("lane", "--status", out)[:2] == (0, lane)


def test_gate_whose_inputs_built_reports_its_real_result(tmp_path):
    # only dinerod failed: W5 is UNMEASURED, every other gate is measured
    b = tmp_path / "build"
    arts = codemodel(b)
    built(b, [p for n, p in arts.items() if n != "dinerod"])
    st, stdout, out = scan(tmp_path, "failure", "linux")
    assert st["failed"] == ["dinerod"]
    assert gate(out, "W5")[:2] == (3, "GATE W5 UNMEASURED (prerequisite build failed: dinerod)")
    for gid in ("K1", "K3", "K4", "W1", "W3", "W4", "W6"):
        assert gate(out, gid) == (0, "", "")


def test_posix_nodecore_ffi_failure_is_not_unmeasured(tmp_path):
    # nodecore_ffi is not a base target: W5 runs and FAILs "check did not run"
    b = tmp_path / "build"
    arts = codemodel(b)
    built(b, [p for n, p in arts.items() if n != "nodecore_ffi"])
    st, stdout, out = scan(tmp_path, "failure", "linux")
    assert st["failed"] == [] and "LANE" not in stdout
    assert gate(out, "W5") == (0, "", "")
    assert cli("lane", "--status", out) == (0, "", "")


def test_run1_windows_shape_has_no_unmeasured_gate(tmp_path):
    # PR #860 run 1 windows-msvc: the build step failed only on nodecore_ffi
    # (maintenance_journal.cpp.obj); every base target built. dinerod.pdb is
    # listed in the codemodel but not produced: it does not count.
    b = tmp_path / "build"
    codemodel(b, windows=True)
    built(b, ["dinero_crypto.lib", "dinero_consensus.lib", "src/wallet/dinero_wallet.lib",
              "dinerod.exe", "dinero_wallet_ffi.lib", "tests/separation/sep_wallet_fixture_compat.exe"])
    st, stdout, out = scan(tmp_path, "failure", "windows")
    assert st["failed"] == [] and "LANE" not in stdout
    for gid in ("K1", "K3", "K4", "W1", "W3", "W4", "W5", "W6"):
        assert gate(out, gid) == (0, "", "")


def test_successful_build_reports_nothing_failed(tmp_path):
    b = tmp_path / "build"
    codemodel(b)          # no artifacts on disk: outcome success is authoritative
    st, stdout, out = scan(tmp_path, "success", "macos")
    assert st["failed"] == [] and "LANE" not in stdout
    assert gate(out, "K1") == (0, "", "")


def test_missing_or_malformed_status_is_did_not_run(tmp_path):
    rc, line, _ = gate(tmp_path / "absent.json", "W4")
    assert rc == 2 and line.startswith("GATE W4 FAIL rc=2 (check did not run: no prerequisite build status")
    bad = tmp_path / "bad.json"
    bad.write_text('{"failed": "x"}')
    rc, line, _ = gate(bad, "K1")
    assert rc == 2 and "check did not run" in line and "UNMEASURED" not in line
    # a gate missing from the status is not silently measured
    partial = tmp_path / "partial.json"
    partial.write_text('{"failed": [], "gates": {}}')
    assert gate(partial, "K1")[0] == 2
    assert cli("lane", "--status", tmp_path / "absent.json")[0] == 2


def test_scan_without_codemodel_is_an_input_error(tmp_path):
    (tmp_path / "build").mkdir()
    rc, stdout, err, out = scan_cli(tmp_path, "failure", "linux")
    assert rc == 2 and stdout.startswith("LANE FAIL: harness error: no codemodel reply")
    assert gate(out, "K1")[0] == 2


# Fix round (review minors 2-4)
def test_k4_anchors_include_the_k4_test_targets(tmp_path):
    # only dinero_wallet failed: K4 depends on it through test_daa_golden_vectors
    b = tmp_path / "build"
    arts = codemodel(b)
    built(b, [p for n, p in arts.items() if n not in ("dinero_wallet", "dinerod", "dinero_wallet_ffi",
              "nodecore_ffi", "sep_wallet_forced_link", "sep_wallet_fixture_compat",
              "sep_wallet_ffi_standalone", "test_daa_golden_vectors")])
    st, stdout, out = scan(tmp_path, "failure", "linux")
    assert st["failed"] == ["dinero_wallet", "dinerod", "sep_wallet_fixture_compat"]
    assert gate(out, "K4")[:2] == (3, "GATE K4 UNMEASURED (prerequisite build failed: dinero_wallet)")
    assert gate(out, "K1") == (0, "", "")
    # without that K4 target, K4 stays measured
    st, stdout, out = scan(tmp_path, "failure", "linux", k4="test_bip341_sighash_vectors\n")
    assert gate(out, "K4") == (0, "", "")


def test_sep_targets_failure_is_a_harness_error_not_lane_unmeasured(tmp_path):
    b = tmp_path / "build"
    codemodel(b)            # nothing built: sep_targets.py failed before the build
    for kw, why in (({"sep_rc": "2"}, "sep_targets.py rc=2"),
                    ({"sep_rc": None}, "no sep_targets.py exit code"),
                    ({"k4": None}, "no K4 target list")):
        rc, stdout, err, out = scan_cli(tmp_path, "failure", "linux", **kw)
        assert rc == 2 and "LANE UNMEASURED" not in stdout
        assert stdout.startswith("LANE FAIL: harness error: prerequisite build did not run: ") and why in stdout
        gh = tmp_path / "gh.txt"
        if gh.exists():
            gh.unlink()
        rc, line, _ = gate(out, "W4", gh)
        assert rc == 2 and line.startswith("GATE W4 FAIL rc=2 (check did not run: harness error: "
                                           "prerequisite build did not run")
        assert gh.read_text().startswith("why=check did not run: harness error: ")
        assert gate(out, "K2") == (0, "", "")
        assert cli("lane", "--status", out)[1].startswith("LANE FAIL: harness error: ")


def test_base_target_missing_from_codemodel_is_a_harness_error(tmp_path):
    b = tmp_path / "build"
    arts = codemodel(b)
    built(b, list(arts.values()))
    (b / ".cmake/api/v1/reply/codemodel-v2-x.json").write_text(
        (b / ".cmake/api/v1/reply/codemodel-v2-x.json").read_text().replace('"name": "dinerod"', '"name": "dinerod2"'))
    for outcome in ("failure", "success"):
        rc, stdout, err, out = scan_cli(tmp_path, outcome, "linux")
        assert rc == 2 and stdout == "LANE FAIL: harness error: base target(s) not in the codemodel: dinerod"
        assert gate(out, "W5")[0] == 2
