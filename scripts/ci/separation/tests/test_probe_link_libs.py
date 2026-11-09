import json
import subprocess
import sys
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent.parent / "probe_link_libs.py"


def make_reply(build, fragments, target="sep_wallet_forced_link", configs=("Release",),
               artifacts=("dinero_consensus.lib", "a.lib")):
    reply = build / ".cmake" / "api" / "v1" / "reply"
    reply.mkdir(parents=True)
    (reply / "target-x.json").write_text(json.dumps(
        {"name": target, "link": {"commandFragments": fragments}}))
    (reply / "target-lib.json").write_text(json.dumps(
        {"name": "libs", "artifacts": [{"path": a} for a in artifacts]}))
    (reply / "codemodel-v2-x.json").write_text(json.dumps({"configurations": [
        {"name": c, "targets": [{"name": target, "jsonFile": "target-x.json"},
                                {"name": "libs", "jsonFile": "target-lib.json"}]} for c in configs]}))
    (reply / "index-2026.json").write_text(json.dumps(
        {"objects": [{"kind": "codemodel", "jsonFile": "codemodel-v2-x.json"}]}))


def run(build, target="sep_wallet_forced_link", *extra):
    return subprocess.run([sys.executable, str(SCRIPT), "--build", str(build), "--target", target, *extra],
                          capture_output=True, text=True)


def test_lists_in_build_archives_in_link_order(tmp_path):
    b = tmp_path / "build"
    outside = tmp_path / "openssl" / "libcrypto.lib"
    make_reply(b, [
        {"role": "flags", "fragment": "/WHOLEARCHIVE:dinero_wallet.lib"},
        {"role": "libraries", "fragment": "src\\wallet\\dinero_wallet.lib".replace("\\", "/")},
        {"role": "libraries", "fragment": "dinero_consensus.lib"},
        {"role": "libraries", "fragment": "\"%s\"" % (b / "_deps" / "rocksdb.lib")},
        {"role": "libraries", "fragment": str(outside)},
        {"role": "libraries", "fragment": "ws2_32.lib"},
        {"role": "libraries", "fragment": "-lpthread"},
        {"role": "libraries", "fragment": "dinero_consensus.lib"},
    ])
    r = run(b)
    assert r.returncode == 0, r.stderr
    assert r.stdout.splitlines() == [str(b / "src/wallet/dinero_wallet.lib"),
                                     str(b / "dinero_consensus.lib"),
                                     str(b / "_deps/rocksdb.lib")]
    # bare system library (not a codemodel artifact) and outside archives skipped
    assert "ws2_32.lib" in r.stderr
    assert "libcrypto.lib" in r.stderr and "-lpthread" in r.stderr


def test_unknown_target_is_input_error(tmp_path):
    b = tmp_path / "build"
    make_reply(b, [{"role": "libraries", "fragment": "a.lib"}])
    r = run(b, "sep_other")
    assert r.returncode == 2 and r.stderr.startswith("ERROR ")


def test_no_reply_is_input_error(tmp_path):
    r = run(tmp_path)
    assert r.returncode == 2 and "codemodel-v2" in r.stderr


def test_no_archive_is_input_error(tmp_path):
    b = tmp_path / "build"
    make_reply(b, [{"role": "libraries", "fragment": "-lpthread"}])
    r = run(b)
    assert r.returncode == 2


def test_multi_config_needs_config(tmp_path):
    b = tmp_path / "build"
    make_reply(b, [{"role": "libraries", "fragment": "a.lib"}], configs=("Debug", "Release"))
    assert run(b).returncode == 2
    assert run(b, "sep_wallet_forced_link", "--config", "Release").returncode == 0
