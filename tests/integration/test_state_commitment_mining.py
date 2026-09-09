#!/usr/bin/env python3
"""Mine DNRS-enforced blocks through both public external mining interfaces."""
import base64
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import tempfile
import time
import urllib.request
import urllib.error

ROOT = Path(__file__).resolve().parents[2]
BINARY = Path(os.environ.get("DINEROD", ROOT / "build/dinerod"))
WORK = Path(tempfile.mkdtemp(prefix="dinero_mining_dnrs_"))
NODES = []
RESULTS = []
SUCCESS = False

def require(ok, message):
    if not ok:
        raise AssertionError(message)

def ports():
    sockets = [socket.socket() for _ in range(3)]
    try:
        for s in sockets:
            s.bind(("127.0.0.1", 0))
        return [s.getsockname()[1] for s in sockets]
    finally:
        for s in sockets:
            s.close()

class Node:
    def __init__(self, name, headers=None, dormant=False):
        self.path = WORK / name
        self.path.mkdir()
        if headers:
            shutil.copytree(headers, self.path / "headers")
        self.rpc_port, self.p2p, self.wallet = ports()
        self.extra = ["--consensus-state-commitment-height=4294967295"] if dormant else []
        self.process = None
        NODES.append(self)
        self.start()

    def start(self):
        self.log = open(self.path / "daemon.log", "ab")
        self.process = subprocess.Popen([str(BINARY), "--regtest", f"--datadir={self.path}",
            f"--rpcport={self.rpc_port}", f"--port={self.p2p}", f"--wallet-socket-port={self.wallet}",
            "--p2p.offline=1", "--listen=0", "--utreexo=1", *self.extra], stdout=self.log, stderr=self.log)
        for _ in range(120):
            if self.process.poll() is not None:
                raise AssertionError(f"daemon exited: {self.path}")
            try:
                self.call("getblockcount")
                return
            except (OSError, ValueError, AssertionError, StopIteration):
                time.sleep(0.25)
        raise AssertionError(f"RPC not ready: {self.path}")

    def raw(self, method, params=None):
        cookie = next(p for p in [self.path / ".cookie", self.path / "regtest/.cookie"] if p.exists()).read_bytes().strip()
        request = urllib.request.Request(f"http://127.0.0.1:{self.rpc_port}/",
            data=json.dumps({"jsonrpc":"2.0", "id":1, "method":method, "params":params or []}).encode(),
            headers={"Authorization":"Basic " + base64.b64encode(cookie).decode(), "Content-Type":"application/json"})
        try:
            response = urllib.request.urlopen(request, timeout=60)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            return json.load(response)

    def call(self, method, params=None):
        reply = self.raw(method, params)
        require(not reply.get("error"), f"{method}: {reply}")
        return reply["result"]

    def stop(self):
        if self.process is None:
            return
        self.process.terminate()
        try:
            self.process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=10)
            raise AssertionError(f"daemon required SIGKILL: {self.path}")
        finally:
            self.log.close()
            self.process = None


import sys
sys.path.insert(0, str(ROOT / "tests/mining"))
from dinero_cpu_miner import DineroCoinMiner

try:
    node = Node("active")
    denied = node.raw("blockchain.debugclearundoflag", ["00"*32])
    require(denied.get("error", {}).get("code") == -32099, "development RPC must be disabled by default")
    node.call("generate", [3])
    # Generate establishes a wallet without assuming asynchronous startup timing.
    address = node.call("wallet.getnewaddress")["address"]
    miner = DineroCoinMiner(f"http://127.0.0.1:{node.rpc_port}", str(node.path / ".cookie"), address)
    require(miner.load_cookie_auth(), "cookie")
    for _ in range(2):
        template = miner.get_block_template()
        require(template and template.coinbase_tx_hex, "canonical coinbase absent")
        metadata = node.call("getblocktemplate", [{"address":address}])
        require("statecommitment" in metadata["rules"], "missing active rule")
        require(metadata["mutable"] == [], "unsafe mutation advertised")
        binding = metadata["statecommitment"]
        decoded = node.call("decoderawtransaction", [metadata["coinbasetxn"]["data"]])
        outputs = decoded["vout"]
        scripts = [o["scriptPubKey"]["hex"] for o in outputs]
        tagged = [s for s in scripts if s.startswith("6a25444e525301")]
        require(len(tagged) == 1 and tagged[0] == binding["script"], "DNRS encoding/index")
        require(scripts[binding["output_index"]] == binding["script"], "DNRS output index")
        require(bytes.fromhex(tagged[0][14:])[::-1].hex() == binding["root"], "wire/display order")
        # Use one template's exact bytes; another request can have a new time.
        own = node.call("decoderawtransaction", [template.coinbase_tx_hex])
        own_script = [o["scriptPubKey"]["hex"] for o in own["vout"] if o["scriptPubKey"]["hex"].startswith("6a25444e525301")]
        require(len(own_script) == 1, "reference miner template DNRS")
        solved = miner.mine_block(template, 1000000)
        require(solved is not None, "regtest nonce search exhausted")
        reply = node.call("submitblock", [solved[0].hex()])
        require(reply in (None, {}), f"external block rejected: {reply}")
        root = node.call("daemon.shieldedroot")["shielded_root"]
        require(root == bytes.fromhex(own_script[0][14:])[::-1].hex(), "external mined post-root mismatch")
        require(node.call("getblockcount") == template.height, "external block not connected")
        print("PASS getblocktemplate", template.height, root, flush=True)

    job = node.call("mining.getjob", [{"address":address}])
    require(not job.get("error"), f"coordinator job: {job}")
    header = bytearray.fromhex(job["header_hex"])
    require(len(header) == 128, "header size")
    for nonce in range(1000000):
        struct.pack_into("<I", header, job["nonce_offset"], nonce)
        digest = hashlib.sha256(hashlib.sha256(header).digest()).digest()
        if int.from_bytes(digest, "big") <= int(job["target"],16):
            break
    else:
        raise AssertionError("coordinator nonce search exhausted")
    require(node.call("mining.submit", [{"job_id":job["job_id"], "nonce":nonce}]) in (None, {}), "coordinator rejected")
    require(node.call("getbestblockhash") == digest.hex(), "coordinator submitted a different block")
    require(node.call("getblockcount") == job["height"], "coordinator did not connect")
    committed = node.call("daemon.shieldedroot")["shielded_root"]
    node.stop(); node.start()
    require(node.call("daemon.shieldedroot")["shielded_root"] == committed, "coordinator root not persisted")
    print("PASS mining.getjob/mining.submit and restart", job["height"], committed, flush=True)
    dormant = Node("dormant", dormant=True)
    dormant.call("generate", [3])
    dormant_address = dormant.call("wallet.getnewaddress")["address"]
    metadata = dormant.call("getblocktemplate", [{"address":dormant_address}])
    require("statecommitment" not in metadata["rules"] and "statecommitment" not in metadata, "dormant metadata")
    print("PASS dormant control", flush=True)
    SUCCESS = True
finally:
    for node in NODES:
        node.stop()
    if SUCCESS and not os.environ.get("KEEP_DATADIR"):
        shutil.rmtree(WORK)
    else:
        print("Mining DNRS evidence:", WORK, flush=True)
