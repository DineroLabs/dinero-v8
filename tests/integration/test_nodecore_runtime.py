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
import subprocess
import tempfile
import time

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
    sequence = 0
    def __init__(self):
        Embedded.sequence += 1
        suffix = "" if Embedded.sequence == 1 else f"-{Embedded.sequence}"
        self.log = (WORK / f"nodecore{suffix}.log").open("wb")
        self.reply_path = WORK / f"replies{suffix}.jsonl"
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
            assert self.process.poll() is None, f"NodeCore exited: {WORK}"
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
        if self.process.stdin.closed:
            return
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

    def crash(self):
        self.process.kill()
        self.process.wait(timeout=10)
        self.close()


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
    check("source closes before copying header store", node.call("stop") == 0)

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


def maintenance(node):
    path = datadir("maintenance")
    sentinel = path / "inspection-sentinel"
    sentinel.write_bytes(b"unchanged")
    check("maintenance fixture starts", node.start(path) == 0)
    original = state(node)
    unsupported = node.call("maintenance_begin", datadir=str(path), plan="reset-chain-v1")
    check("reset remains unsupported", unsupported == {"code": -11, "token": None}, unsupported)
    check("unsupported plan leaves node running", node.call("status")["running"])
    other = datadir("unrelated-datadir")
    unrelated = node.call("maintenance_begin", datadir=str(other), plan="inspect-unchanged-v1")
    check("Begin cannot stop a different datadir", unrelated == {"code": -5, "token": None}, unrelated)
    check("different target refusal keeps node running", node.call("status")["running"])
    held = node.call("maintenance_at_callback", datadir=str(path), restart_datadir=str(path), config={"network": "regtest"})
    check("Begin waits for actual worker completion", held["callback_entered"] and held["stop_waits_for_callback"]
          and held["running_during_callback"] is False and not held["callback_timed_out"], held)
    check("callback lifecycle reentry fails without deadlock", all(held[k] == -14 for k in
          ("reentrant_stop", "reentrant_start", "reentrant_begin")), held)
    check("racing Start waits then receives busy", held["start_attempted"] and held["start_waits_for_stop"]
          and held["restart_result"] == -8, held)
    check("inspection Begin acquires a real ownership token", held["stop_result"] == 0 and bool(held["token"]), held)
    token = held["token"]
    check("Begin returns only after node closure", node.call("status") is None)
    check("Start excluded for whole inspection", node.start(path) == -8)
    check("competing Begin excluded", node.call("maintenance_begin", datadir=str(path),
          plan="inspect-unchanged-v1") == {"code": -8, "token": None})
    check("plain Stop preserves lease", node.call("stop") == 0 and node.start(path) == -8)
    check("invalid token cannot release ownership", node.call("maintenance_finish", token="wrong", outcome=0) == -10)
    check("invalid token preserves Start exclusion", node.start(path) == -8)
    check("invalid outcome preserves ownership", node.call("maintenance_finish", token=token, outcome=9) == -5
          and node.start(path) == -8)
    finished = node.call("maintenance_finish", token=token, outcome=0)
    check("unchanged inspection completes", finished == 0, finished)
    check("completed token cannot be reused", node.call("maintenance_finish", token=token, outcome=0) == -10)
    check("start allowed after verified completion", node.start(path) == 0)
    check("maintenance preserves chain and roots", state(node) == original, state(node))

    held = node.call("maintenance_begin", datadir=str(path), plan="inspect-unchanged-v1")
    check("next generation can acquire ownership", held["code"] == 0 and held["token"] != token)
    check("previous token cannot finish new generation", node.call("maintenance_finish", token=token, outcome=0) == -10)
    token = held["token"]
    info = node.call("maintenance_status", datadir=str(path))
    operation = info["operation_id"]
    # Simulate an out-of-contract writer; no actual chain/wallet row is altered.
    sentinel.write_bytes(b"changed")
    check("completion refuses changed contents", node.call("maintenance_finish", token=token, outcome=0) == -13)
    check("failed verification keeps ownership", node.start(path) == -8)
    check("uncertain finish retains durable barrier", node.call("maintenance_finish", token=token, outcome=1) == -9)
    check("ordinary restart refuses uncertainty", node.start(path) == -9)
    node.crash()

    recovered = Embedded()
    try:
        check("fresh process respects persistent barrier", recovered.start(path) == -9)
        alias = WORK / "maintenance-alias"
        alias.symlink_to(path, target_is_directory=True)
        check("path alias cannot bypass barrier", recovered.start(alias) == -9)
        check("new process rejects old token", recovered.call("maintenance_finish", token=token, outcome=0) == -10)
        wrong = recovered.call("maintenance_resume", datadir=str(path), operation_id="0" * 32)
        check("wrong operation cannot resume", wrong == {"code": -13, "token": None}, wrong)
        check("wrong resume leaves startup fenced", recovered.start(path) == -9)
        resume = recovered.call("maintenance_resume", datadir=str(path), operation_id=operation)
        check("explicit resume returns a new token", resume["code"] == 0 and resume["token"] != token, resume)
        check("resume alone does not certify changed data", recovered.call("maintenance_finish", token=resume["token"], outcome=0) == -13)
        # Restore only the deliberately modified fixture file under the held
        # test lease, then require exact original bytes before completion.
        sentinel.write_bytes(b"unchanged")
        check("restored fixture can complete verification", recovered.call("maintenance_finish", token=resume["token"], outcome=0) == 0)
        check("recovered start succeeds", recovered.start(path) == 0)
        check("recovered roots unchanged", state(recovered) == original, state(recovered))
        check("recovered node closes", recovered.call("stop") == 0)
        # Kill with the owner still held, without Finish or a normal teardown.
        held = recovered.call("maintenance_begin", datadir=str(path), plan="inspect-unchanged-v1")
        check("acquire before abrupt process death", held["code"] == 0 and bool(held["token"]), held)
        operation = recovered.call("maintenance_status", datadir=str(path))["operation_id"]
        control = path.parent / ".nodecore-maintenance-v1"
        control.chmod(0o500)
        try:
            failed = recovered.call("maintenance_finish", token=held["token"], outcome=0)
            check("journal write failure retains ownership", failed == -12 and recovered.start(path) == -8, failed)
        finally:
            control.chmod(0o700)
        recovered.crash()
    finally:
        recovered.close()

    after_crash = Embedded()
    try:
        check("process death cannot release the persistent barrier", after_crash.start(path) == -9)
        check("dead process token is invalid", after_crash.call("maintenance_finish", token=held["token"], outcome=0) == -10)
        resumed = after_crash.call("maintenance_resume", datadir=str(path), operation_id=operation)
        check("held operation resumes after death", resumed["code"] == 0 and resumed["token"] != held["token"], resumed)
        check("unchanged crash recovery completes", after_crash.call("maintenance_finish", token=resumed["token"], outcome=0) == 0)
        check("normal startup follows explicit crash recovery", after_crash.start(path) == 0)
        check("crash recovery preserves roots", state(after_crash) == original, state(after_crash))
        check("crash recovery closes", after_crash.call("stop") == 0)
    finally:
        after_crash.close()


def maintenance_gate(node):
    for name in ("empty", "malformed", "symlink", "staging", "unknown-version"):
        parent = WORK / name
        parent.mkdir()
        missing = parent / "not-created"
        control = parent / ".nodecore-maintenance-v1"
        control.mkdir(mode=0o700)
        intent = control / "intent.json"
        if name == "malformed":
            intent.write_text("{truncated")
        elif name == "symlink":
            elsewhere = parent / "elsewhere.json"
            elsewhere.write_text("{}")
            intent.symlink_to(elsewhere)
        elif name == "staging":
            (control / "intent.next").write_text("partial")
        elif name == "unknown-version":
            intent.write_text(json.dumps({"format": 999, "phase": "completed"}))
        check(f"{name} intent blocks startup", node.start(missing) == -9)
        check(f"{name} startup creates no datadir", not missing.exists())
        info = node.call("maintenance_status", datadir=str(missing))
        check(f"{name} diagnostics fail closed", info["gate"] == -9 and info["phase"] == "unresolved", info)
        if not info["inspection_qualification"]:
            unsupported = node.call("maintenance_begin", datadir=str(missing), plan="inspect-unchanged-v1")
            check(f"{name} normal build exposes no inspection permission", unsupported == {"code": -11, "token": None})


node = Embedded()
success = False
try:
    {"lifecycle": lifecycle, "snapshot": snapshot, "maintenance": maintenance,
     "maintenance_gate": maintenance_gate}[os.environ["NODECORE_SCENARIO"]](node)
    success = True
finally:
    node.close()
    print(f"Evidence: {WORK}", flush=True)
    if success:
        check("driver exits as expected", node.process.returncode == ( -9 if os.environ["NODECORE_SCENARIO"] == "maintenance" else 0), node.process.returncode)
