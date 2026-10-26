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
        # The embedded RPC registry still owns a loopback listener even with
        # rpc=0. Let the OS pick its port; never contend with a user's daemon.
        "p2p.offline=1\nlisten=0\nrpc=0\nrpc.port=0\nwallet.socket.enable=0\n"
        "utreexo.checkpoint_interval=1000\nassumeutxo_bg_stall_timeout=3600\n")
    return path


def mainnet_schedule(node):
    # Mainnet schedules Orchard (H150000). The daemon refuses to start while the
    # legacy Lightning oracle clients are enabled, and embedded hosts never run
    # them, so NodeCore must disable them itself. App static peers (addnode)
    # must reach the daemon too. Offline: no peer is contacted.
    path = datadir("mainnet-schedule")
    started = node.start(path, network="mainnet", addnode="127.0.0.1:9,127.0.0.2:9")
    check("embedded mainnet with scheduled Orchard starts", started == 0, started)
    status = node.call("status")
    check("embedded mainnet is running", status["running"] and status["network"] == "mainnet", status)
    # The daemon's own startup line counts configured addnode seeds (none on
    # mainnet otherwise); this is the first P2P start in this driver's log.
    seeds = [line for line in (WORK / "nodecore.log").read_text(errors="replace").splitlines()
             if "[P2PService]   Seed nodes:" in line]
    check("app static peers reach the daemon", bool(seeds) and seeds[0].endswith("Seed nodes: 2"), seeds[:1])
    check("mainnet stop", node.call("stop") == 0)


def lifecycle(node):
    mainnet_schedule(node)
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


def datadir_lock(node):
    # Native kernel-lock regression using the real linked C ABI. No mock
    # owner, forced database unlock or installed datadir is involved.
    import fcntl
    path = datadir("locked-root").resolve()
    alias = WORK / "root-alias"
    alias.symlink_to(path, target_is_directory=True)
    with (path / "dinerod.lock").open("a+b") as lease:
        fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
        refused = node.start(alias)
        check("existing root lock refuses alias before Init", refused == -3, refused)
        check("lock refusal does not create ChainDB", not (path / "blockchain").exists())
        fcntl.flock(lease, fcntl.LOCK_UN)
        check("alias starts after external lock release", node.start(alias) == 0)
        check("locked alias node answers", node.rpc("getblockcount") == 0)
        try:
            fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
            held = False
            fcntl.flock(lease, fcntl.LOCK_UN)
        except BlockingIOError:
            held = True
        check("SDK retains root lock while running", held)
        contender = Embedded()
        try:
            check("second process cannot open resolved root", contender.start(path) == -3)
            check("owner still answers after contention", node.rpc("getblockcount") == 0)
            check("SDK closes before root release", node.call("stop") == 0)
            fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
            check("completed stop releases root lock", not (path / "dinerod.pid").exists())
            fcntl.flock(lease, fcntl.LOCK_UN)
            check("other process reopens resolved root", contender.start(path) == 0)
            check("alias cannot bypass reopened root lock", node.start(alias) == -3)
            check("second process closes cleanly", contender.call("stop") == 0)
        finally:
            contender.close()
        check("alias cold reopen after both owners close", node.start(alias) == 0)
        check("final alias owner closes", node.call("stop") == 0)


def state(node):
    commitment = node.rpc("blockchain.getutreexocommitment")
    return {"height": node.rpc("getblockcount"), "hash": node.rpc("getbestblockhash"),
            "utreexo": {key: commitment[key] for key in
                ("commitment", "num_leaves", "num_roots", "verified_height")},
            "shielded": node.rpc("daemon.shieldedroot")}


def storage_readiness(node):
    def layout(path):
        db = path / "blockchain" / "chaindb"
        options = sorted(db.glob("OPTIONS-*"))
        return bool(options) and "shielded_state_v1" in options[-1].read_text(errors="replace")
    def seeds(path):
        # Inspect only closed disposable wallets, retaining digests not seed
        # material in the test's output. Migration must not change these rows.
        found = {}
        for db in sorted((path / "wallets").glob("wallet_*.db")):
            with sqlite3.connect(db.as_uri()+"?mode=ro", uri=True) as sql:
                rows = sql.execute("SELECT * FROM hd_seeds ORDER BY id").fetchall()
            found[db.name] = hashlib.sha256(repr(rows).encode()).hexdigest()
        assert found, "real bootstrap must create a wallet"
        return found
    path = datadir("fresh-stateless-ready").resolve()
    check("fresh scheduled stateless SDK starts", node.start(path, network="mainnet", sync_profile="ios_utreexo") == 0)
    before = state(node)
    check("fresh scheduled SDK is separated before sync", layout(path))
    check("fresh readiness closes", node.call("stop") == 0)
    original_seeds = seeds(path)
    check("fresh readiness cold reopen", node.start(path, network="mainnet", sync_profile="ios_utreexo") == 0)
    check("fresh readiness roots unchanged", state(node) == before)
    check("fresh readiness second close", node.call("stop") == 0)
    check("fresh readiness keeps wallet seed", seeds(path) == original_seeds)

    legacy = datadir("legacy-stateless-ready").resolve()
    check("real stopped legacy fixture", node.call("legacy_storage_fixture", datadir=str(legacy)) == 0)
    check("fixture is legacy before embedded start", not layout(legacy))
    original_seeds = seeds(legacy)
    sentinel = legacy / "wallets" / "pending-sentinel"
    sentinel.write_bytes(b"fixture-outside-chaindb-must-not-change")
    check("SDK upgrades existing legacy store for stateless profile", node.start(legacy, network="mainnet", sync_profile="ios_utreexo") == 0)
    check("legacy store separated", layout(legacy))
    check("original legacy store retained", (legacy/"blockchain"/"chaindb.legacy-v1").is_dir())
    check("migration kept canonical genesis", state(node) == before)
    check("migration closes", node.call("stop") == 0)
    check("migration keeps seed and unrelated pending file", seeds(legacy) == original_seeds and sentinel.read_bytes() == b"fixture-outside-chaindb-must-not-change")
    check("migrated stateless SDK cold reopens", node.start(legacy, network="mainnet", sync_profile="ios_utreexo") == 0)
    check("migrated cold roots unchanged", state(node) == before)
    check("migrated final close", node.call("stop") == 0)

    linked = datadir("inner-symlink-refusal").resolve()
    check("real legacy fixture for inner-symlink negative", node.call("legacy_storage_fixture", datadir=str(linked)) == 0)
    original_seeds = seeds(linked)
    (linked/"blockchain").rename(linked/"private-blockchain")
    (linked/"blockchain").symlink_to(linked/"private-blockchain", target_is_directory=True)
    check("preactivation refusal can still report its status", node.start(linked, network="mainnet", sync_profile="ios_utreexo") == 0)
    report = node.rpc("orchard.getactivationstatus")
    check("inner-symlink migration guard still refuses", report["storage_upgrade_refused"] is True, report)
    check("refusal does not publish separated layout", not layout(linked) and not (linked/"blockchain"/"chaindb.legacy-v1").exists())
    check("refusal keeps canonical genesis", state(node) == before)
    check("refused store closes", node.call("stop") == 0)
    check("refusal keeps wallet seed", seeds(linked) == original_seeds)


def reopen_fences(node):
    path = datadir("reopen-fence-owner")
    phrase = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about"
    check("real Core for fences starts", node.start(path) == 0)
    restored = node.rpc("wallet.restore", {"name":"reopen-owner", "mnemonic":phrase, "rescan":False})
    check("named fixture wallet is open", restored["success"] is True)
    probe = node.call("reopen_fences")
    check("real wallet and pool reopen fences", probe == {
        "wallet_name":"reopen-owner", "candidate_count":0,
        "foreign_fence_refused":True,
        "wallet_reentrant_refused":True, "pool_reentrant_refused":True,
        "wallet_gate_closed":True, "pool_gate_closed":True,
        "wallet_stop_requires_decision":True, "pool_stop_requires_decision":True}, probe)
    check("aborted fences keep live context", node.rpc("getblockcount") == 0)
    check("named wallet opens without phrase after fence abort", node.rpc("wallet.open", ["reopen-owner"])["success"] is True)
    check("fence fixture stops", node.call("stop") == 0)
    check("fence fixture cold reopens", node.start(path) == 0)
    check("named wallet reopens without phrase after ordinary restart", node.rpc("wallet.open", ["reopen-owner"])["success"] is True)
    check("fence fixture closes", node.call("stop") == 0)


def reopen_rpc_pause(node):
    path = datadir("reopen-rpc-owner")
    phrase = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about"
    check("RPC pause real Core starts", node.start(path) == 0)
    check("RPC pause named wallet opens", node.rpc("wallet.restore", {"name":"rpc-owner", "mnemonic":phrase, "rescan":False})["success"] is True)
    probe = node.call("reopen_rpc_pause")
    expected = {key:True for key in ["nested_refused", "stale_epoch_refused", "resumed_epoch_allowed",
        "overlap_refusal_suppressed", "wallet.orchard.queueshield", "wallet.orchard.queuespend",
        "wallet.orchard.queuesend", "getblockcount"]}
    check("paused queues return no reply, including complete overlap", probe == expected, probe)
    check("RPC resumes on same context", node.rpc("getblockcount") == 0)
    check("named wallet remains openable without phrase", node.rpc("wallet.open", ["rpc-owner"])["success"] is True)
    check("RPC pause fixture closes", node.call("stop") == 0)


def app_reopen(node):
    # Real linked DaemonApp instances, not a source mocked owner. No nodecore
    # worker/automatic boundary is claimed by this sequential host fixture.
    path = datadir("actual-app-reopen")
    probe = node.call("actual_app_reopen", datadir=str(path))
    check("actual App sealed close/fresh App/named open", probe == {
        "candidates":0, "resolved":0, "retained":0, "wallet_name":"reopen-owner",
        "paused_queues_retryable":True, "wallet_open_without_phrase":True,
        "stale_epoch_refused":True, "repeat_resume_refused":True, "resumed_epoch_allowed":True,
        "completed_intent_archived":True}, probe)


def app_pending_reopen(node):
    expected = {"candidates":0, "resolved":0, "retained":0, "wallet_name":"reopen-owner",
        "paused_queues_retryable":True, "wallet_open_without_phrase":True,
        "stale_epoch_refused":True, "repeat_resume_refused":True, "resumed_epoch_allowed":True,
        "completed_intent_archived":True}
    same = node.call("actual_app_reopen", datadir=str(datadir("abandoned-app-reopen")), mode="abandoned")
    check("live pause is not stolen; abandoned receipt recovers closed", same == {
        **expected, "live_token_refused":True, "abandoned_gate_stays_closed":True}, same)
    path = datadir("cold-pending-app-reopen")
    prepared = node.call("actual_app_reopen", datadir=str(path), mode="prepare")
    check("pending intent survives complete original host exit", prepared == {
        "pending_wallet":"reopen-owner", "pending_count":0, "rpc_closed":True}, prepared)
    node.close()
    check("original driver exits cleanly before fresh host", node.process.returncode == 0, node.process.returncode)
    fresh = Embedded()
    try:
        recovered = fresh.call("actual_app_reopen", datadir=str(path), mode="cold")
        check("fresh process reopens named wallet without phrase and retry-gates queues", recovered == expected, recovered)
    finally:
        fresh.close()
    check("cold recovery driver exits cleanly", fresh.process.returncode == 0, fresh.process.returncode)


def app_prepared_readmission(node):
    # Actual linked Apps at GENESIS/EMPTY pool. No automatic SDK publication,
    # activation crossing or nonempty private-payment survival is claimed.
    prepared = {"paused_queues_retryable":True, "prepared_candidates":0,
        "prepared_resolved":0, "prepared_retained":0, "prepared_gate_closed":True,
        "prepared_exact_intent_retained":True, "prepared_named_wallet":"reopen-owner",
        "prepared_queues_retryable":True}
    resumed = {"candidates":0, "resolved":0, "retained":0, "wallet_name":"reopen-owner",
        "paused_queues_retryable":True, "wallet_open_without_phrase":True,
        "stale_epoch_refused":True, "repeat_resume_refused":True, "resumed_epoch_allowed":True,
        "completed_intent_archived":True}
    probe = node.call("actual_app_reopen", datadir=str(datadir("prepared-resume")), mode="prepared-resume")
    check("readmission keeps RPC and durable intent closed until explicit completion", probe == {**prepared, **resumed}, probe)
    path = datadir("prepared-exit")
    probe = node.call("actual_app_reopen", datadir=str(path), mode="prepared-exit")
    check("prepared replacement stops without archiving intent or reopening queues", probe == prepared, probe)
    node.close()
    check("prepared host exits cleanly with its intent still selected", node.process.returncode == 0, node.process.returncode)
    fresh = Embedded()
    try:
        recovered = fresh.call("actual_app_reopen", datadir=str(path), mode="cold")
        check("fresh process recovers prepared named wallet without phrase", recovered == resumed, recovered)
    finally:
        fresh.close()
    check("prepared recovery host exits cleanly", fresh.process.returncode == 0, fresh.process.returncode)


def sdk_pending_reopen(node):
    # Real Apps and the actual linked SDK worker, GENESIS/EMPTY corpus only.
    # Publication/pending-intent controls, not activation/queued acceptance.
    path = datadir("short-publication")
    probe = node.call("actual_app_reopen", datadir=str(path), mode="published")
    for field in ["unprepared_publish_refused", "foreign_publish_refused",
                  "throw_keeps_gate_closed", "gate_closed_inside_publication",
                  "gate_open_after_publication", "published_intent_retained",
                  "prepared_gate_closed", "prepared_exact_intent_retained",
                  "prepared_queues_retryable", "paused_queues_retryable",
                  "completed_intent_archived", "wallet_open_without_phrase"]:
        check("publication contract: " + field, probe[field] is True, probe)
    check("short publication reopens intended named wallet", probe["wallet_name"] == "reopen-owner", probe)
    path = datadir("published-before-archive-exit")
    prepared = node.call("actual_app_reopen", datadir=str(path), mode="published-exit")
    check("published host exits before archive without dropping intent",
          prepared["published_intent_retained"] and prepared["gate_open_after_publication"], prepared)
    pending = path / ".core-reopen-pending-v1"
    intent_bytes = pending.read_bytes()
    journals = {p.name: {str(f.relative_to(p)): hashlib.sha256(f.read_bytes()).hexdigest()
                        for f in p.rglob("*") if f.is_file()}
                for p in path.glob(".core-reopen-candidates-v1-*") if p.is_dir()}
    check("selected pending corpus exists before fresh SDK start", bool(journals))
    node.close()
    check("publication fixture process exits cleanly", node.process.returncode == 0)
    fresh = Embedded()
    try:
        check("actual SDK automatically recovers pending named intent", fresh.start(path) == 0)
        check("SDK recovered App is running", fresh.call("status")["running"] is True)
        # Loading the named wallet alone is insufficient: WalletService also
        # remembers the last active wallet. Prove the worker actually consumed
        # the selected intent through normal readmission, then retained its
        # completed control and the original public candidate corpus.
        deadline = time.monotonic() + 10
        while pending.exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        completed = list(path.glob(".core-reopen-pending-v1.completed-*"))
        check("SDK worker completed selected pending readmission",
              not pending.exists() and len(completed) == 1)
        check("SDK retains completed intent bytes", completed[0].read_bytes() == intent_bytes)
        check("SDK preserves original public candidate corpus", journals == {
            p.name: {str(f.relative_to(p)): hashlib.sha256(f.read_bytes()).hexdigest()
                     for f in p.rglob("*") if f.is_file()}
            for p in path.glob(".core-reopen-candidates-v1-*") if p.is_dir()})
        check("SDK worker loaded the active named wallet before caller open",
              fresh.rpc("wallet.getwalletinfo")["walletname"] == "reopen-owner")
        check("SDK named wallet opens without phrase", fresh.rpc("wallet.open", ["reopen-owner"])["success"] is True)
        check("SDK canonical RPC uses replacement App", fresh.rpc("getblockcount") == 0)
        check("SDK recovered owner stops normally", fresh.call("stop") == 0)
        check("SDK ordinary restart after publication", fresh.start(path) == 0)
        check("SDK named wallet still opens without phrase", fresh.rpc("wallet.open", ["reopen-owner"])["success"] is True)
        check("SDK closes recovered restart", fresh.call("stop") == 0)
    finally:
        fresh.close()
    check("SDK recovery process exits cleanly", fresh.process.returncode == 0)


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
    # Startup owns a generated default wallet. Recovery uses a separate name;
    # the subsequent import exercises the existing same-seed bind handler.
    restored = node.rpc("wallet.restore", {
        "name": "snapshot-source", "mnemonic": mnemonic, "rescan": False})
    check("source recovery uses a new wallet", restored.get("success") is True, restored)
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
    check("legacy compact profile reports its actual full backing",
          node.rpc("blockchain.getutreexocommitment")["retains_full_state"] is True)
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
    check("legacy assumed-base read does not invent a root-only owner",
          node.rpc("blockchain.getutreexocommitment")["retains_full_state"] is True)
    check("unrelated consumer wallet owns no fixture coins", len(node.rpc("wallet.listunspent", [0])) == 0)

    # The consumer has already returned from nodecore_start. Publish another
    # generation without replacing A. The real late-import handler must reopen
    # A: it has no pre-base bodies from which to recover this wallet.
    staging_b.rename(generation_b)
    newer = generation_b / selected.name
    check("another complete generation is published", newer.is_file()
          and Path(str(newer) + ".manifest.json").is_file())
    check("later generation has different bytes", hashlib.sha256(newer.read_bytes()).hexdigest() != digest)
    # Establish the recovered seed without replacing the consumer's default
    # wallet or scanning. The unchanged import below must still read generation A.
    restored = node.rpc("wallet.restore", {
        "name": "snapshot-recovered", "mnemonic": mnemonic, "rescan": False})
    check("late recovery uses a new wallet", restored.get("success") is True, restored)
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
    try:
        {"lifecycle": lifecycle, "datadir_lock": datadir_lock, "storage_readiness": storage_readiness,
         "reopen_fences": reopen_fences,
         "reopen_rpc_pause": reopen_rpc_pause,
         "app_reopen": app_reopen,
         "app_pending_reopen": app_pending_reopen,
         "app_prepared_readmission": app_prepared_readmission,
         "sdk_pending_reopen": sdk_pending_reopen,
         "snapshot": snapshot, "maintenance": maintenance,
         "maintenance_gate": maintenance_gate}[os.environ["NODECORE_SCENARIO"]](node)
        success = True
    finally:
        node.close()
        if success:
            check("driver exits as expected", node.process.returncode == ( -9 if os.environ["NODECORE_SCENARIO"] == "maintenance" else 0), node.process.returncode)
except BaseException:
    (WORK / "controller-failure.txt").write_text(traceback.format_exc())
    raise
finally:
    print(f"Evidence: {WORK}", flush=True)
