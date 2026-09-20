#!/usr/bin/env python3
"""A legacy CSN must recover authenticated replay metadata automatically.

The offline fixture removes modern replay metadata and stored body proofs after
an actual disconnect. The default peer mode also removes undo metadata; a peer
returning later must unblock replay. CSN_REPLAY_RECOVERY_SOURCE=undo preserves
undo and requires recovery before the peer returns. Neither mode permits a
second reconsiderblock, reindex, or validation bypass.
"""
import base64
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
BINARY = Path(os.environ.get("DINEROD", ROOT / "build/dinerod"))
FIXTURE = Path(os.environ.get("CSN_REPLAY_FIXTURE", ROOT / "build/tests/integration/csn_replay_metadata_fixture"))
RECOVERY_SOURCE = os.environ.get("CSN_REPLAY_RECOVERY_SOURCE", "peer")
sys.path.insert(0, str(ROOT / "tests/mining"))
from dinero_cpu_miner import BlockTemplate, DineroCoinMiner

WORK = Path(tempfile.mkdtemp(prefix=f"dinero-csn-replay-metadata-{RECOVERY_SOURCE}-"))
NODES = []
COIN = 100_000_000
RECEIPT = {"binary": str(BINARY), "recovery_source": RECOVERY_SOURCE, "checks": []}


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def wait(predicate, description, timeout=120):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.2)
    raise AssertionError(f"timeout: {description}")


class Node:
    def __init__(self, name, csn=False):
        self.name, self.csn = name, csn
        self.path = WORK / name
        self.path.mkdir()
        sockets = [socket.socket() for _ in range(3)]
        for sock in sockets:
            sock.bind(("127.0.0.1", 0))
        self.rpc, self.p2p, self.wallet = [sock.getsockname()[1] for sock in sockets]
        for sock in sockets:
            sock.close()
        self.peer, self.process = None, None
        NODES.append(self)

    def call(self, method, params=None):
        cookie = (self.path / ".cookie").read_bytes().strip()
        request = urllib.request.Request(
            f"http://127.0.0.1:{self.rpc}/",
            json.dumps({"jsonrpc": "2.0", "id": 1, "method": method,
                        "params": params or []}).encode(),
            {"Content-Type": "application/json", "Authorization":
             "Basic " + base64.b64encode(cookie).decode()},
        )
        # Retry only the explicit rate-limit response, never an RPC rejection,
        # a timeout, or an arbitrary failed submitblock response.
        for attempt in range(8):
            try:
                with urllib.request.urlopen(request, timeout=60) as response:
                    reply = json.load(response)
                break
            except urllib.error.HTTPError as error:
                if error.code == 429 and attempt < 7:
                    error.close()
                    time.sleep(.5 * (attempt + 1))
                    continue
                raise AssertionError(
                    f"{self.name} {method}: HTTP {error.code}: "
                    f"{error.read().decode(errors='replace')}") from error
        require(not reply.get("error"), f"{self.name} {method}: {reply}")
        result = reply["result"]
        if isinstance(result, dict):
            require(not result.get("error"), f"{self.name} {method}: {result}")
        return result

    def start(self):
        args = [str(BINARY), "--regtest", f"--datadir={self.path}",
                f"--rpcport={self.rpc}", f"--port={self.p2p}",
                f"--wallet-socket-port={self.wallet}", "--listen=1", "--utreexo=1",
                "--utreexo-stateless=1" if self.csn else "--utreexo-bridge=1",
                "--regtest-enforce-pow", "--consensus-sixty-second-height=4",
                f"--connect=127.0.0.1:{self.peer.p2p}"]
        with (WORK / f"{self.name}.log").open("ab") as log:
            self.process = subprocess.Popen(args, stdout=log, stderr=log)

        def ready():
            require(self.process.poll() is None, f"{self.name} exited during startup")
            try:
                return self.call("getblockcount") >= 0
            except (FileNotFoundError, ConnectionError, urllib.error.URLError):
                return False
        wait(ready, f"{self.name} RPC ready")

    def stop(self):
        if self.process is None:
            return
        process, self.process = self.process, None
        process.terminate()
        try:
            process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            raise AssertionError(f"{self.name} required SIGKILL")
        require(process.returncode == 0, f"{self.name} exit {process.returncode}")


def bits_target(bits):
    exponent, mantissa = bits >> 24, bits & 0x7fffff
    require(not bits & 0x800000, "negative PoW target")
    return mantissa << (8 * (exponent - 3)) if exponent >= 3 else mantissa >> (8 * (3 - exponent))


def template(node, address):
    data = node.call("getblocktemplate", [{"rules": ["segwit"], "address": address}])
    coinbase = data["coinbasetxn"]
    return BlockTemplate(
        version=data["version"], height=data["height"],
        previous_block_hash=data["previousblockhash"], bits=data["bits"],
        curtime=data["curtime"], mintime=data.get("mintime", data["curtime"]),
        maxtime=data.get("maxtime", data["curtime"] + 7200),
        coinbase_value=data["coinbasevalue"], transactions=data["transactions"],
        coinbase_tx_hex=coinbase["data"], coinbase_txid=coinbase["txid"],
        utreexo_commitment=data.get("utreexo", {}).get("commitment", data.get("utreexocommitment", "")),
        target=data.get("target", ""), time_mutable="time" in data.get("mutable", []))


def mine(node, address, fees=0, txids=()):
    job = template(node, address)
    require(job.coinbase_value == 100 * COIN + fees,
            f"height {job.height}: exact subsidy+fees expected {100 * COIN + fees}, "
            f"got {job.coinbase_value}")
    require([tx["txid"] for tx in job.transactions] == list(txids),
            f"height {job.height}: unexpected template transaction order")
    if job.height < 4:
        # Preserve the shipped legacy arithmetic, including its wrapping target
        # multiply: historical timestamps bootstrap it before the timing upgrade.
        job.curtime = node.call("getconsensusinfo")["genesis_time"] + job.height * 120
        job.time_mutable = False
        job.bits = "207fffff" if job.height == 1 else "1f00fc9c"
        job.target = f"{bits_target(int(job.bits, 16)):064x}"
    miner = DineroCoinMiner(f"http://127.0.0.1:{node.rpc}", mining_address=address)
    with contextlib.redirect_stdout(io.StringIO()):
        solved = miner.mine_block(job, max_nonce=4_000_000)
    require(solved is not None, f"height {job.height}: PoW nonce search exhausted")
    block = solved[0]
    digest = hashlib.sha256(hashlib.sha256(block[:128]).digest()).digest()
    require(int.from_bytes(digest, "big") <= bits_target(int(job.bits, 16)), "invalid solved PoW")
    check = {"height": job.height, "hash": digest.hex(), "bits": job.bits,
             "fees_una": fees, "coinbase_una": job.coinbase_value,
             "txids": list(txids), "accepted": False}
    if txids:
        RECEIPT["checks"].append(check)
    submitted = node.call("submitblock", [block.hex()])
    require(submitted in (None, {}), f"height {job.height}: template rejected: {submitted}")
    require(node.call("getbestblockhash") == digest.hex(), "submitted block did not become active tip")
    require(node.call("getblock", [digest.hex(), 1])["tx"][1:] == list(txids), "mined txids differ")
    check["accepted"] = True
    if txids:
        print(f"PASS accepted exact-fee template height={job.height} fees={fees} txs={len(txids)}", flush=True)
    return digest.hex()


def spend(node, txid, amount, address, prevout=None):
    raw = node.call("wallet.createrawtransaction", [[{"txid": txid, "vout": 0}], {address: amount}])
    signed = node.call("wallet.signrawtransaction", [raw["hex"], *([prevout] if prevout else [])])
    require(signed.get("complete") is True, "transaction signature incomplete")
    decoded = node.call("wallet.decoderawtransaction", [signed["hex"]])
    sent = node.call("wallet.sendrawtransaction", [signed["hex"]])
    while isinstance(sent, dict):
        sent = sent.get("txid", sent.get("result"))
    require(sent == decoded["txid"], f"broadcast txid mismatch: {sent}")
    output = decoded["vout"][0]
    return sent, [{"txid": sent, "vout": 0, "amount": amount,
                   "scriptPubKey": output["scriptPubKey"]["hex"]}]


def root(node):
    return node.call("blockchain.getutreexocommitment")["commitment"]


def proof(full, csn, txid):
    result = full.call("blockchain.getutxoproofs_batch", [[{"txid": txid, "vout": 0}]])
    require(result["successful"] == 1 and result["failed"] == 0, f"output not provable: {result}")
    for node in (full, csn):
        verified = node.call("blockchain.verifyutxoproofs_batch", [result["proofs"]])
        require(verified["valid"] == 1 and verified["invalid"] == 0,
                f"{node.name} rejected live canonical proof: {verified}")
    return result["proofs"]


def spent(node, proofs):
    result = node.call("blockchain.verifyutxoproofs_batch", [proofs])
    require(result["valid"] == 0 and result["invalid"] == 1 and
            result["results"][0]["error_code"] == "utxo-not-found",
            f"{node.name} spent output rejection: {result}")


def fixture(node, blockhash, downgrade=False):
    require(node.process is None, "fixture operation requires a stopped node")
    args = [str(FIXTURE), "--regtest-fixture", "--datadir", str(node.path),
            "--hash", blockhash, "--downgrade" if downgrade else "--inspect"]
    if downgrade:
        args.append("--keep-local-undo" if RECOVERY_SOURCE == "undo" else "--strip-local-undo-spent")
    result = subprocess.run(args, capture_output=True, text=True, timeout=30)
    label = ("downgrade-" if downgrade else "inspect-") + blockhash
    (WORK / (label + ".log")).write_text(result.stdout + result.stderr)
    require(result.returncode == 0, f"fixture {label} failed: {result.stdout}\n{result.stderr}")
    records = [json.loads(line.removeprefix("FIXTURE_RESULT "))
               for line in result.stdout.splitlines() if line.startswith("FIXTURE_RESULT ")]
    require(len(records) == 1, "fixture did not return exactly one receipt")
    RECEIPT.setdefault("fixtures", []).append(records[0])
    return records[0]


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def require_no_recovery_fuse():
    log = (WORK / "csn.log").read_text(errors="replace")
    require("utreexo proof coverage degraded" not in log and
            "Automatic chainstate recovery is DISABLED" not in log,
            "unrelated proof-coverage recovery fuse prevents a clean metadata-recovery test")


def mine_mature_prefix(node, address):
    for _ in range(105):
        mine(node, address)
        # Four or more RPCs per block can exhaust the 50 request/second
        # token bucket. Linux may expose the early HTTP 429 close as a TCP
        # reset. Pace only this setup; keep transport failures fatal.
        time.sleep(.10)


def main():
    require(RECOVERY_SOURCE in ("peer", "undo"), "CSN_REPLAY_RECOVERY_SOURCE must be peer or undo")
    for binary in (BINARY, FIXTURE):
        require(binary.is_file(), f"missing executable: {binary}")
    RECEIPT.update({"binary_sha256": sha256(BINARY), "fixture": str(FIXTURE),
                    "fixture_sha256": sha256(FIXTURE), "test_sha256": sha256(Path(__file__))})
    full, csn = Node("full"), Node("csn", True)
    full.peer, csn.peer = csn, full
    full.start()
    address = full.call("wallet.getnewaddress")["address"]
    mine_mature_prefix(full, address)
    csn.start()
    parent = full.call("getbestblockhash")
    wait(lambda: csn.call("getbestblockhash") == parent, "CSN mature prefix")
    parent_root = root(full)
    require(parent_root == root(csn), "initial full/CSN roots differ")
    # Created after the daemon has bound its fresh PoW-regtest profile. The
    # mutator additionally checks the real ChainDB genesis and owns its lock.
    (csn.path / "csn-replay-metadata-test-only").write_text(
        f"csn-replay-metadata-fixture-v1\n{csn.path.resolve()}\n")
    first_coin = full.call("getblock", [full.call("getblockhash", [1]), 1])["tx"][0]
    second_coin = full.call("getblock", [full.call("getblockhash", [2]), 1])["tx"][0]
    before = proof(full, csn, first_coin)
    # First teach both nodes the shorter competing branch. Building it only
    # after the higher-work branch is known would make its acquisition depend
    # on unrelated lower-work header selection behavior.
    alternate_address = full.call("wallet.getnewaddress")["address"]
    alternative = [mine(full, alternate_address) for _ in range(3)]
    waiting_tip = alternative[-1]
    wait(lambda: csn.call("getbestblockhash") == waiting_tip, "CSN competing branch at height108")
    waiting_root = root(csn)
    require(root(full) == waiting_root, "competing branch roots differ")
    for node in (csn, full):
        node.call("blockchain.invalidateblock", [alternative[0]])
        require(node.call("getbestblockhash") == parent and root(node) == parent_root,
                f"{node.name} competing-branch disconnect")
    txid, _ = spend(full, first_coin, 99.9, address)
    first_block = mine(full, address, 10_000_000, [txid])
    second_tx, _ = spend(full, second_coin, 9.8, address)
    second_block = mine(full, address, 9_020_000_000, [second_tx])
    for _ in range(2):
        tip = mine(full, address)
    wait(lambda: csn.call("getbestblockhash") == tip, "CSN fee branch at height109")
    final_root = root(full)
    require(final_root == root(csn), "initial fee full/CSN roots differ")
    final_proof = proof(full, csn, second_tx)
    RECEIPT.update({"parent_tip": parent, "parent_root": parent_root,
                    "expected_tip": tip, "expected_root": final_root})
    print("PASS real-PoW fee chain and canonical Utreexo proofs", flush=True)

    # Keep a competing active branch above both historical fee blocks. A
    # straight extension is insufficient: normal height-forward downloads can
    # already replace its missing proof metadata, masking this regression.
    csn.call("blockchain.invalidateblock", [first_block])
    require(csn.call("getbestblockhash") == parent and root(csn) == parent_root,
            "CSN disconnect did not restore original ancestor")
    RECEIPT.update({"waiting_tip": waiting_tip, "waiting_root": waiting_root})
    # No source peer is available during downgrade, restart and reconsider.
    full.stop()
    csn.stop()
    originals = {}
    for blockhash in (first_block, second_block):
        record = fixture(csn, blockhash, downgrade=True)
        require(record["active_tip"] == parent, "offline fixture altered active tip")
        previous, downgraded = record["before"], record["after"]
        require(previous["format"] == "CSN2" and previous["spent_outputs"] > 0,
                "test did not start with genuine metadata-bearing replay records")
        require(downgraded["format"] == "legacy" and not downgraded["body_has_utreexo"] and
                not downgraded["legacy_body_has_utreexo"], "modern replay/body metadata still exists")
        if RECOVERY_SOURCE == "peer":
            require(record["undo_policy"] == "strip" and downgraded["undo_spent"] == 0 and
                    downgraded["legacy_undo_spent"] in (None, 0), "local undo fallback still exists")
        else:
            require(record["undo_policy"] == "keep" and downgraded["undo_spent"] > 0 and
                    downgraded["undo_spent"] == previous["undo_spent"] and
                    downgraded["legacy_undo_spent"] == previous["legacy_undo_spent"],
                    "local undo metadata was not preserved")
        require(previous["targets"] == downgraded["targets"] and
                previous["txids"] == downgraded["txids"], "fixture changed authenticated identities")
        originals[blockhash] = previous
    undo_policy = "preserved undo" if RECOVERY_SOURCE == "undo" else "stripped undo"
    print(f"PASS historical hash-only sidecars, stripped body proofs, {undo_policy}; active ancestor untouched", flush=True)

    csn.start()
    require(csn.call("getbestblockhash") == parent and root(csn) == parent_root,
            "legacy restart changed canonical state")
    csn.call("blockchain.reconsiderblock", [alternative[0]])
    wait(lambda: csn.call("getbestblockhash") == waiting_tip, "CSN restores known competing branch")
    require(root(csn) == waiting_root, "restored competing branch root differs")
    reconsidered = csn.call("blockchain.reconsiderblock", [first_block])
    RECEIPT["reconsider_result"] = reconsidered
    if RECOVERY_SOURCE == "peer":
        # Observe beyond one repair-pump tick. A missing operational dependency
        # cannot change canonical state or mark this known-valid branch invalid.
        for _ in range(16):
            require(csn.call("getbestblockhash") == waiting_tip and root(csn) == waiting_root,
                    "unavailable peer caused canonical tip/forest mutation")
            time.sleep(.5)
        branches = csn.call("blockchain.getchaintips")
        RECEIPT["waiting_tips"] = branches
        if isinstance(branches, dict):
            branches = branches.get("tips", branches.get("result"))
        require(isinstance(branches, list), f"unexpected getchaintips: {branches}")
        require(not any(branch.get("hash") in (first_block, second_block, tip) and branch.get("status") == "invalid"
                        for branch in branches), "missing metadata was classified as invalid history")
        require_no_recovery_fuse()
        print("PASS reconsider waits with peer unavailable; canonical ancestor/root unchanged", flush=True)
    else:
        require(full.process is None, "source peer must remain stopped during local undo recovery")
        wait(lambda: csn.call("getbestblockhash") == tip,
             "automatic legacy replay metadata recovery from local undo with source peer offline")
        require(full.process is None and root(csn) == final_root,
                "local undo recovery did not finish at the expected root before source peer returned")
        RECEIPT["recovered_while_peer_offline"] = True
        require_no_recovery_fuse()
        print("PASS automatic local undo recovery while source peer remains offline", flush=True)

    full.start()
    require(full.call("getbestblockhash") == tip and root(full) == final_root, "source restart mismatch")
    # No second reconsiderblock, block submission or reindex occurs here.
    # Peer mode must finish on reconnect; undo mode already finished offline.
    wait(lambda: csn.call("getbestblockhash") == tip,
         "automatic legacy replay metadata recovery after source peer returns", timeout=120)
    require(root(csn) == final_root, "recovered forest differs from original")
    proof(full, csn, second_tx)
    for node in (full, csn):
        spent(node, before)
        verified = node.call("blockchain.verifyutxoproofs_batch", [final_proof])
        require(verified["valid"] == 1 and verified["invalid"] == 0, "original output proof changed")
    recovery_log = (WORK / "csn.log").read_text(errors="replace")
    log_source = "local history" if RECOVERY_SOURCE == "undo" else "peer"
    for height in (106, 107):
        require(f"[CSN-ReplayRepair] Recovered metadata at height {height} from {log_source}" in recovery_log,
                f"height {height} did not exercise repair from {log_source}")
    csn.stop()
    for blockhash in (first_block, second_block):
        result = fixture(csn, blockhash)
        repaired = result["before"]
        require(result["active_tip"] == tip and not repaired["failed"], "recovery left invalidity or wrong tip")
        require(repaired["format"] == "CSN2" and repaired["spent_outputs"] == originals[blockhash]["spent_outputs"],
                "repaired metadata was not durable")
        require(repaired["targets"] == originals[blockhash]["targets"] and
                repaired["txids"] == originals[blockhash]["txids"], "repair changed targets or transaction identities")
    csn.start()
    require(csn.call("getbestblockhash") == tip and root(csn) == final_root, "repaired restart state mismatch")
    require_no_recovery_fuse()
    proof(full, csn, second_tx)
    print(f"PASS automatic {RECOVERY_SOURCE} repair, CSN2 durability, exact roots and proofs after restart", flush=True)
    # Exercise parent/child metadata after repair. Disconnecting a historical
    # CPFP block currently exposes a separate UTXO-cache restoration issue;
    # it is deliberately not allowed to confound the legacy-repair fixture.
    third_coin = full.call("getblock", [full.call("getblockhash", [3]), 1])["tx"][0]
    package_parent, prevout = spend(full, third_coin, 99.9, address)
    package_child, _ = spend(full, package_parent, 9.8, address, prevout)
    package_tip = mine(full, address, 9_020_000_000, [package_parent, package_child])
    wait(lambda: csn.call("getbestblockhash") == package_tip, "post-repair CPFP relay")
    require(root(full) == root(csn), "post-repair CPFP root mismatch")
    package_root = root(full)
    proof(full, csn, package_child)
    csn.stop()
    csn.start()
    require(csn.call("getbestblockhash") == package_tip and root(csn) == package_root,
            "post-repair CPFP restart mismatch")
    require_no_recovery_fuse()
    proof(full, csn, package_child)
    print("PASS parent/child input metadata and proofs after recovered-node restart", flush=True)



success = False
try:
    main()
    success = True
finally:
    errors = []
    for node in NODES:
        try:
            node.stop()
        except Exception as error:
            errors.append(str(error))
    RECEIPT["success"] = success and not errors
    RECEIPT["cleanup_errors"] = errors
    (WORK / "receipt.json").write_text(json.dumps(RECEIPT, indent=2) + "\n")
    if success and not errors and os.environ.get("DINERO_TEST_KEEP_DATA") != "1":
        shutil.rmtree(WORK)
    else:
        print("Retained evidence:", WORK, flush=True)
    if errors:
        raise AssertionError(errors)
