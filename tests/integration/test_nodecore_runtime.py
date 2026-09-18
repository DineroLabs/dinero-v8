#!/usr/bin/env python3
"""Actual linked NodeCore lifecycle and snapshot consumers, synthetic/offline only.

This does not grant external maintenance permission or qualify iOS file locking.
The callback barrier observes real shutdown; there are no substituted services.
"""
import json
import hashlib
import copy
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import tempfile
import time
import traceback

DRIVER = Path(os.environ["NODECORE_DRIVER"])
EVIDENCE = os.environ.get("NODECORE_EVIDENCE_DIR")
if EVIDENCE:
    Path(EVIDENCE).mkdir(parents=True, exist_ok=True)
WORK = Path(tempfile.mkdtemp(prefix="dinero_nodecore_runtime_", dir=EVIDENCE))
CHECKS = []


def check(name, condition, observed=None):
    CHECKS.append({"name": name, "passed": bool(condition), "observed": observed})
    (WORK / "checks.json").write_text(json.dumps(CHECKS, indent=2) + "\n")
    print(("PASS " if condition else "FAIL ") + name, flush=True)
    assert condition, f"{name}: {observed!r}; evidence: {WORK}"


class Embedded:
    def __init__(self):
        self.log = (WORK / "nodecore.log").open("wb")
        self.reply_path = WORK / "replies.jsonl"
        self.reply_path.touch()
        self.replies = self.reply_path.open()
        self.process = subprocess.Popen([str(DRIVER), str(self.reply_path)],
            stdin=subprocess.PIPE, text=True, stdout=self.log, stderr=subprocess.STDOUT)

    def call(self, op, **kwargs):
        self.process.stdin.write(json.dumps({"op": op, **kwargs}) + "\n")
        self.process.stdin.flush()
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            line = self.replies.readline()
            if line:
                result = json.loads(line)
                assert not isinstance(result, dict) or "driver_error" not in result, result
                return result
            returncode = self.process.poll()
            assert returncode is None, f"NodeCore exited during {op} (returncode={returncode}): {WORK}"
            time.sleep(0.05)
        raise AssertionError(f"NodeCore operation {op} timed out: {WORK}")

    def rpc(self, method, params=None):
        result = self.call("rpc", method=method, params=[] if params is None else params)
        assert not isinstance(result, dict) or not result.get("error"), (method, result)
        return result

    def start(self, path, **config):
        return self.call("start", datadir=str(path), config={
            "network": "regtest", "verbose_logging": True, "sync_profile": "mac_fullblock",
            **config})

    def close(self):
        self.process.stdin.close()
        try:
            self.process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=10)
            raise AssertionError(f"NodeCore did not close: {WORK}")
        finally:
            self.log.close()
            self.replies.close()


def datadir(name):
    path = WORK / name
    path.mkdir()
    # max_peers=0 does NOT disable networking in the C ABI. Use the real
    # daemon configuration, which is loaded before CLI overrides.
    (path / "dinero.conf").write_text(
        "p2p.offline=1\nlisten=0\nrpc=0\nwallet.socket.enable=0\n"
        "utreexo.checkpoint_interval=1000\nassumeutxo_bg_stall_timeout=3600\n")
    return path


def lifecycle(node):
    path = datadir("lifecycle")
    check("already stopped is idempotent", node.call("stop") == 0)
    started = node.start(path)
    check("actual FFI starts", started == 0, started)
    status = node.call("status")
    check("real offline regtest is running", status["running"] and status["network"] == "regtest", status)
    check("duplicate start rejected", node.start(path) == -1)
    check("RPC is wired to live chainstate", node.rpc("getblockcount") == 0)
    barrier = node.call("stop_at_callback")
    check("stop waits for the real emitting thread", barrier == {
        "callback_entered": True, "running_during_callback": False,
        "stop_waits_for_callback": True, "stop_result": 0, "callback_timed_out": False}, barrier)
    check("queries unavailable after stop", node.call("status") is None)
    check("same datadir reopens after completed close", node.start(path) == 0)
    check("restarted RPC uses new live context", node.rpc("getblockcount") == 0)
    barrier = node.call("stop_at_callback", restart_datadir=str(path), config={
        "network": "regtest", "verbose_logging": True, "sync_profile": "mac_fullblock"})
    check("competing real start waits for completed stop", barrier == {
        "callback_entered": True, "running_during_callback": False,
        "stop_waits_for_callback": True, "stop_result": 0, "callback_timed_out": False,
        "start_attempted": True, "start_waits_for_stop": True, "restart_result": 0}, barrier)
    check("competing start acquires fresh live context", node.rpc("getblockcount") == 0)
    check("restart closes cleanly", node.call("stop") == 0)


def state(node):
    commitment = node.rpc("blockchain.getutreexocommitment")
    return {"height": node.rpc("getblockcount"), "hash": node.rpc("getbestblockhash"),
            "utreexo": {key: commitment[key] for key in
                ("commitment", "num_leaves", "num_roots", "verified_height")},
            "shielded": node.rpc("daemon.shieldedroot")}


def stopped_wallet_birth_height(path, name):
    # Only inspect the synthetic wallet after nodecore_stop has closed it.
    database = path / "wallets" / f"wallet_{name}.db"
    connection = sqlite3.connect(database.as_uri() + "?mode=ro", uri=True)
    try:
        return connection.execute("SELECT birth_height FROM sync_meta WHERE id=1").fetchone()[0]
    finally:
        connection.close()


def snapshot(node):
    # Public deterministic regtest mnemonic, never a user's wallet.
    mnemonic = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about"
    source = datadir("source")
    generation_a = WORK / "generation-a"
    generation_b = WORK / "generation-b"
    staging_b = WORK / "staging-b"
    generation_a.mkdir()
    staging_b.mkdir()
    selected = generation_a / "snapshot.dat"
    newer = staging_b / "snapshot.dat"
    check("source NodeCore starts", node.start(source) == 0)
    imported = node.rpc("wallet.importmnemonic", {
        "mnemonic": mnemonic, "rescan": False, "initial_address_count": 4})
    check("source owns fixture scripts", imported["success"] and imported["watch_scripts"] > 0, imported)
    owner = node.rpc("wallet.listaddresses")[0]["address"]
    mined = node.rpc("generatetoaddress", [130, owner])
    check("source mines snapshot base", len(mined["blocks"]) == 130)
    exported = node.rpc("dumptxoutset", [str(selected)])
    check("real nonempty snapshot exported", exported["base_height"] == 130 and exported["coins_written"] > 0, exported)
    expected = state(node)
    expected_coins = {(coin["txid"], coin["vout"]) for coin in node.rpc("wallet.listunspent", [0])}
    assert expected_coins, "source must own snapshot coins"
    digest = hashlib.sha256(selected.read_bytes()).hexdigest()
    def manifest(path, tip):
        metadata = {"snapshot_file": path.name, "height": tip["height"],
                    "block_hash": tip["hash"], "bytes": path.stat().st_size,
                    "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
        Path(str(path) + ".manifest.json").write_text(json.dumps(metadata) + "\n")
    manifest(selected, expected)
    # Supply verified ancestry beyond the base for the v5 binding/burial checks.
    node.rpc("generatetoaddress", [8, owner])
    node.rpc("dumptxoutset", [str(newer)])
    manifest(newer, state(node))
    node.rpc("wallet.createhd", ["later", 12, "", ""])
    check("source closes before copying header store", node.call("stop") == 0)
    check("closed source leaves no legacy ChainDB pointer", node.call("legacy_chain_db_bound") is False)
    check("created wallet birthday uses the owning node tip",
          stopped_wallet_birth_height(source, "later") == 138)

    manifest_config = {"sync_profile": "ios_utreexo",
              "assumeutxo_manifest": str(selected) + ".manifest.json",
              "assumeutxo_require_manifest": True}
    # Exercise rejection unconditionally in a separate empty consumer. It has
    # no automatic snapshot candidate, so startup cannot race the assertion.
    rejector = datadir("rejector")
    shutil.copytree(source / "headers", rejector / "headers")
    check("empty consumer starts for rejection control", node.start(rejector, **manifest_config) == 0)
    before_rejection = state(node)
    damaged_dir = WORK / "damaged"
    damaged_dir.mkdir()
    damaged = damaged_dir / selected.name
    bad_bytes = bytearray(selected.read_bytes())
    bad_bytes[-1] ^= 1
    damaged.write_bytes(bad_bytes)
    rejected = node.call("rpc", method="loadtxoutset", params=[str(damaged)])
    check("manifest rejects changed payload", bool(rejected.get("error"))
          and "SHA256 mismatch" in str(rejected), rejected)
    check("failed import leaves consensus state unchanged", state(node) == before_rejection, state(node))
    check("rejection control closes", node.call("stop") == 0)
    check("new node wallet does not inherit the previous node tip",
          stopped_wallet_birth_height(rejector, "default") == 0)

    consumer = datadir("consumer")
    shutil.copytree(source / "headers", consumer / "headers")
    config = {**manifest_config, "assumeutxo_snapshot": str(selected)}
    started = node.start(consumer, **config)
    check("CSN starts with configured snapshot", started == 0, started)
    # Fresh bootstrap may be deferred past nodecore_start's return (headers
    # processing normally triggers it). Offline, explicitly drive the same
    # importer through the real embedded RPC bridge; do not mistake running
    # for import-complete or sleep until it happens accidentally.
    startup_height = node.rpc("getblockcount")
    if startup_height == 0:
        loaded = node.rpc("loadtxoutset", [str(selected)])
        check("deferred import completes through embedded RPC", loaded["base_height"] == 130, loaded)
    else:
        check("startup imported the configured base", startup_height == 130, startup_height)
    check("import preserves source roots and tip", state(node) == expected, state(node))
    check("unrelated consumer wallet owns no fixture coins", len(node.rpc("wallet.listunspent", [0])) == 0)

    # The consumer has already returned from nodecore_start. Publish another
    # generation without replacing A. The real late-import handler must reopen
    # A: it has no pre-base bodies from which to recover this wallet.
    staging_b.rename(generation_b)
    newer = generation_b / selected.name
    check("another complete generation is published", newer.is_file()
          and Path(str(newer) + ".manifest.json").is_file())
    check("later generation has different bytes", hashlib.sha256(newer.read_bytes()).hexdigest() != digest)
    imported = node.rpc("wallet.importmnemonic", {
        "mnemonic": mnemonic, "rescan": True, "initial_address_count": 4})
    check("late wallet recovery reads selected generation", imported.get("success") is True
          and imported.get("snapshot_utxo_rescan", {}).get("base_height") == 130
          and imported["snapshot_utxo_rescan"].get("recorded", 0) > 0, imported)
    coins = node.rpc("wallet.listunspent", [0])
    check("late reader recovers exactly the base wallet coins",
          {(coin["txid"], coin["vout"]) for coin in coins} == expected_coins, len(coins))
    outpoint = {"txid": coins[0]["txid"], "vout": coins[0]["vout"]}
    proof = node.rpc("blockchain.getutxoproofs_batch", [[outpoint]])
    check("recovered snapshot coin is provable", proof["successful"] == 1 and proof["failed"] == 0, proof)
    verified = node.rpc("blockchain.verifyutxoproofs_batch", [proof["proofs"]])
    check("real verifier accepts snapshot proof", verified["valid"] == 1 and verified["invalid"] == 0, verified)
    altered = copy.deepcopy(proof["proofs"])
    assert altered[0]["proof"]["siblings"], "fixture proof must be nontrivial"
    altered[0]["proof"]["siblings"][0] = "0" * 64
    rejected = node.rpc("blockchain.verifyutxoproofs_batch", [altered])
    check("real verifier rejects changed sibling", rejected["valid"] == 0 and rejected["invalid"] == 1, rejected)
    check("consumer closes", node.call("stop") == 0)
    check("snapshot consumer restarts", node.start(consumer, **config) == 0)
    check("restart preserves Utreexo and shielded roots", state(node) == expected, state(node))
    check("restart stays out of safe mode", node.rpc("safemode.status")["active"] is False)
    proof = node.rpc("blockchain.getutxoproofs_batch", [[outpoint]])
    check("snapshot proof remains available after restart", proof["successful"] == 1 and proof["failed"] == 0, proof)
    verified = node.rpc("blockchain.verifyutxoproofs_batch", [proof["proofs"]])
    check("snapshot proof verifies after restart", verified["valid"] == 1 and verified["invalid"] == 0, verified)
    check("selected artifact remains unchanged", hashlib.sha256(selected.read_bytes()).hexdigest() == digest)
    check("consumer closes after restart", node.call("stop") == 0)


node = Embedded()
success = False
try:
    try:
        {"lifecycle": lifecycle, "snapshot": snapshot}[os.environ["NODECORE_SCENARIO"]](node)
        success = True
    finally:
        node.close()
        if success:
            check("driver exits cleanly", node.process.returncode == 0, node.process.returncode)
except BaseException:
    (WORK / "controller-failure.txt").write_text(traceback.format_exc())
    raise
finally:
    print(f"Evidence: {WORK}", flush=True)
