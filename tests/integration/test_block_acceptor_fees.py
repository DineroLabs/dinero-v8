#!/usr/bin/env python3
"""A daemon must accept its own exact-fee templates through the real PoW path.

The regression is a contextual admission precheck that invented a 0.01 DIN
transparent fee instead of leaving exact fee validation to ConnectBlock.
Exercise ordinary and large fees, an intra-block dependency, and full/CSN
recovery without weakening PoW, Utreexo, or the coinbase reward assertions.
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
sys.path.insert(0, str(ROOT / "tests/mining"))
from dinero_cpu_miner import BlockTemplate, DineroCoinMiner

WORK = Path(tempfile.mkdtemp(prefix="dinero-block-acceptor-fees-"))
NODES = []
COIN = 100_000_000
RECEIPT = {"binary": str(BINARY), "checks": []}


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


def main():
    require(BINARY.is_file(), f"missing daemon: {BINARY}")
    binary_hash = hashlib.sha256()
    with BINARY.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            binary_hash.update(chunk)
    RECEIPT["binary_sha256"] = binary_hash.hexdigest()
    full, csn = Node("full"), Node("csn", True)
    full.peer, csn.peer = csn, full
    full.start()
    address = full.call("wallet.getnewaddress")["address"]
    for _ in range(105):
        mine(full, address)
    csn.start()
    parent = full.call("getbestblockhash")
    wait(lambda: csn.call("getbestblockhash") == parent, "CSN mature prefix")
    parent_root = root(full)
    require(parent_root == root(csn), "initial full/CSN roots differ")
    print("PASS real-PoW mature prefix and full/CSN root agreement", flush=True)
    first_coin = full.call("getblock", [full.call("getblockhash", [1]), 1])["tx"][0]
    second_coin = full.call("getblock", [full.call("getblockhash", [2]), 1])["tx"][0]
    before = proof(full, csn, first_coin)
    txid, _ = spend(full, first_coin, 99.9, address)
    first_block = mine(full, address, 10_000_000, [txid])
    wait(lambda: csn.call("getbestblockhash") == first_block, "CSN 0.1 DIN-fee block")
    require(root(full) == root(csn), "fee block full/CSN roots differ")
    proof(full, csn, txid)
    for node in (full, csn):
        spent(node, before)

    # Two transactions in one block: the child's input does not exist in the
    # pre-block coin view, so exact fee accounting must use the in-block overlay.
    package_parent, prevout = spend(full, second_coin, 99.9, address)
    package_child, _ = spend(full, package_parent, 9.8, address, prevout)
    tip = mine(full, address, 9_020_000_000, [package_parent, package_child])
    wait(lambda: csn.call("getbestblockhash") == tip, "CSN parent/child high-fee block")
    final_root = root(full)
    require(final_root == root(csn), "package full/CSN roots differ")
    proof(full, csn, package_child)

    for node in (csn, full):
        node.call("blockchain.invalidateblock", [first_block])
        require(node.call("getbestblockhash") == parent, f"{node.name} rollback tip")
    require(root(full) == root(csn) == parent_root, "rollback full/CSN roots differ from original")
    proof(full, csn, first_coin)
    for node in (full, csn):
        node.stop()
        node.start()
        require(node.call("getbestblockhash") == parent, f"{node.name} rollback restart tip")
        node.call("blockchain.reconsiderblock", [first_block])
        wait(lambda: node.call("getbestblockhash") == tip, f"{node.name} fee-block replay")
        require(root(node) == final_root, f"{node.name} replay root differs")
        spent(node, before)
    proof(full, csn, package_child)
    RECEIPT.update({"parent_tip": parent, "parent_root": parent_root,
                    "recovered_tip": tip, "recovered_root": final_root})
    print("PASS exact transparent fees, in-block dependency, full/CSN proofs and restart/reorg", flush=True)


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
