#!/usr/bin/env python3
"""A future checkpoint must not prevent initial-sync branch replacement."""
import base64
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/mining"))
from dinero_cpu_miner import BlockTemplate, DineroCoinMiner

BINARY = str(Path(sys.argv[1]).resolve())
WORK = Path(tempfile.mkdtemp(prefix="dinero-checkpoint-fork-"))
NODES = []
RECEIPT = {"binary": BINARY, "checks": []}


def require(condition, message):
    if not condition:
        raise AssertionError(message)


class Node:
    def __init__(self, name, checkpoint=None):
        self.name = name
        self.path = WORK / name
        self.path.mkdir()
        config = self.path / "dinero.conf"
        config.write_text("p2p.offline=true\n")
        sockets = [socket.socket() for _ in range(3)]
        for sock in sockets:
            sock.bind(("127.0.0.1", 0))
        self.rpc, p2p, wallet = [sock.getsockname()[1] for sock in sockets]
        for sock in sockets:
            sock.close()
        env = dict(os.environ)
        env.pop("DINERO_TEST_CHECKPOINT_HASH", None)
        if checkpoint:
            env["DINERO_TEST_CHECKPOINT_HASH"] = checkpoint
        args = [BINARY, "--regtest", f"--datadir={self.path}", f"--conf={config}",
                f"--rpcport={self.rpc}", f"--port={p2p}", "--listen=0",
                f"--wallet-socket-port={wallet}", "--utreexo=1", "--utreexo-bridge=1"]
        with (WORK / f"{name}.log").open("w") as log:
            self.process = subprocess.Popen(args, env=env, stdout=log, stderr=log)
        NODES.append(self)
        deadline = time.monotonic() + 60
        while True:
            require(self.process.poll() is None, f"{name} exited during startup")
            try:
                if self.call("getblockcount") >= 0:
                    break
            except (FileNotFoundError, ConnectionError, urllib.error.URLError):
                pass
            require(time.monotonic() < deadline, f"{name} startup timed out")
            time.sleep(.2)

    def call(self, method, params=None):
        cookie = (self.path / ".cookie").read_bytes().strip()
        request = urllib.request.Request(
            f"http://127.0.0.1:{self.rpc}/",
            json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params or []}).encode(),
            {"Content-Type": "application/json", "Authorization": "Basic " + base64.b64encode(cookie).decode()})
        with urllib.request.urlopen(request, timeout=30) as response:
            reply = json.load(response)
        require(not reply.get("error"), f"{self.name} {method}: {reply}")
        return reply["result"]

    def submit(self, block):
        result = self.call("submitblock", [block])
        require(result in (None, {}), f"{self.name}: block rejected: {result}")
        time.sleep(.1)  # Bound fixture RPC load without retrying transport failures.

    def stop(self):
        self.process.terminate()
        try:
            self.process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
            raise AssertionError(f"{self.name} required SIGKILL")
        require(self.process.returncode == 0, f"{self.name} exit {self.process.returncode}")


def mine(node, address):
    data = node.call("getblocktemplate", [{"rules": ["segwit"], "address": address}])
    job = BlockTemplate(
        version=data["version"], height=data["height"], previous_block_hash=data["previousblockhash"],
        bits=data["bits"], curtime=data["curtime"], mintime=data.get("mintime", data["curtime"]),
        maxtime=data.get("maxtime", data["curtime"] + 7200), coinbase_value=data["coinbasevalue"],
        transactions=data["transactions"], coinbase_tx_hex=data["coinbasetxn"]["data"],
        coinbase_txid=data["coinbasetxn"]["txid"],
        utreexo_commitment=data.get("utreexo", {}).get("commitment", data.get("utreexocommitment", "")),
        target=data.get("target", ""), time_mutable="time" in data.get("mutable", []))
    if job.height < 4:
        job.curtime = node.call("getconsensusinfo")["genesis_time"] + job.height * 120
        job.time_mutable = False
        job.bits = "207fffff" if job.height == 1 else "1f00fc9c"
    bits = int(job.bits, 16)
    target = (bits & 0x7fffff) << (8 * ((bits >> 24) - 3))
    job.target = f"{target:064x}"
    miner = DineroCoinMiner(f"http://127.0.0.1:{node.rpc}", mining_address=address)
    with contextlib.redirect_stdout(io.StringIO()):
        solved = miner.mine_block(job, max_nonce=4_000_000)
    require(solved is not None, "PoW nonce search exhausted")
    raw = solved[0]
    digest = hashlib.sha256(hashlib.sha256(raw[:128]).digest()).digest()
    require(int.from_bytes(digest, "big") <= target, "invalid solved PoW")
    node.submit(raw.hex())
    require(node.call("getbestblockhash") == digest.hex(), "mined block did not become active")
    return raw.hex(), digest.hex()


success = False
try:
    branch_b = Node("checkpoint-branch")
    address_b = branch_b.call("wallet.getnewaddress")
    if isinstance(address_b, dict):
        address_b = address_b["address"]
    blocks_b = [mine(branch_b, address_b) for _ in range(8)]
    branch_a = Node("initial-branch")
    for raw, _ in blocks_b[:4]:
        branch_a.submit(raw)
    address_a = branch_a.call("wallet.getnewaddress")
    if isinstance(address_a, dict):
        address_a = address_a["address"]
    blocks_a = [mine(branch_a, address_a) for _ in range(2)]
    require(blocks_a[0][1] != blocks_b[4][1], "fixture branches must differ")
    consumer = Node("consumer", checkpoint=blocks_b[7][1])
    for raw, _ in blocks_b[:4] + blocks_a:
        consumer.submit(raw)
    require(consumer.call("getbestblockhash") == blocks_a[-1][1], "consumer initial tip differs")
    RECEIPT.update(initial_tip=blocks_a[-1][1], replacement=blocks_b[4][1],
                   checkpoint=blocks_b[-1][1])
    # The first replacement is below both the current tip and future checkpoint.
    # Old #804 rejects it solely because it is not in current active ancestry.
    for raw, _ in blocks_b[4:]:
        consumer.submit(raw)
    require(consumer.call("getbestblockhash") == blocks_b[-1][1], "consumer did not reach checkpoint branch")
    RECEIPT["checks"].append("initial-sync replacement reaches exact checkpoint hash")
    consumer.submit(blocks_b[4][0])  # Canonical duplicate remains accepted.
    RECEIPT["checks"].append("canonical duplicate accepted")
    try:
        result = consumer.call("submitblock", [blocks_a[0][0]])
    except AssertionError as error:
        result = str(error)
    require("bad-fork-prior-to-checkpoint" in str(result),
            f"old fork not rejected after checkpoint establishment: {result}")
    require(consumer.call("getbestblockhash") == blocks_b[-1][1], "rejection changed active tip")
    RECEIPT["checks"].append("old fork rejected after checkpoint establishment with tip unchanged")
    success = True
    print("PASS real PoW initial-sync branch replacement, checkpoint establishment, duplicate and old-fork rejection", flush=True)
finally:
    errors = []
    for node in reversed(NODES):
        try:
            node.stop()
        except Exception as error:
            errors.append(str(error))
    RECEIPT.update(success=success and not errors, cleanup_errors=errors)
    (WORK / "receipt.json").write_text(json.dumps(RECEIPT, indent=2) + "\n")
    print("Checkpoint acceptance evidence:", WORK, flush=True)
    if errors:
        raise AssertionError(errors)
