#!/usr/bin/env python3
"""Full-node mining jobs accept genuine inputs owned only by another node."""
import base64
import hashlib
import struct
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
import urllib.request
import urllib.error

ROOT = Path(__file__).resolve().parents[2]
BINARY = Path(os.environ.get("DINEROD", ROOT / "build/dinerod"))
WORK = Path(tempfile.mkdtemp(prefix="dinero_mining_chain_provider_"))
NODES = []
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
        # HTTP 429 rejects before request parsing; only that explicit refusal
        # is safe to retry. Other failures, including ambiguous sends, propagate.
        for attempt in range(51):
            reply = self.raw(method, params)
            error = reply.get("error")
            if (isinstance(error, dict) and error.get("code") == -32000 and
                    error.get("message") == "Rate limit exceeded. Try again shortly." and attempt < 50):
                time.sleep(0.1)
                continue
            require(not error, f"{method}: {reply}")
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


def template(node, address, exclusions=None):
    request = {"address": address}
    if exclusions is not None:
        request["exclude_txids"] = exclusions
    result = node.call("getblocktemplate", [request])
    require(not result.get("error"), f"template: {result}")
    return result


def txids(gbt):
    return {tx["txid"] for tx in gbt["transactions"]}


def mine_template(node, address, gbt):
    miner = DineroCoinMiner(f"http://127.0.0.1:{node.rpc_port}", str(node.path / ".cookie"), address)
    require(miner.load_cookie_auth(), "cookie")
    # Feed the reference miner the exact filtered response, including coinbase.
    miner.rpc_call = lambda method, params: gbt
    candidate = miner.get_block_template()
    require(candidate is not None, "reference miner could not map template")
    solved = miner.mine_block(candidate, 1000000)
    require(solved is not None, "regtest nonce search exhausted")
    result = node.call("submitblock", [solved[0].hex()])
    require(result in (None, {}), f"filtered block rejected: {result}")
    require(node.call("getblockcount") == gbt["height"], "filtered block did not connect")
    require(node.call("daemon.shieldedroot")["shielded_root"] == gbt["statecommitment"]["root"],
            "filtered DNRS differs from connected state")


def sync_blocks(source, destination):
    for height in range(destination.call("getblockcount") + 1, source.call("getblockcount") + 1):
        blockhash = source.call("getblockhash", [height])
        raw = source.call("getblock", [blockhash, 0])
        require(destination.call("submitblock", [raw]) in (None, {}), "valid source block refused")
    require(destination.call("getbestblockhash") == source.call("getbestblockhash"), "chain tips differ")


def solve_job(node, address, expected_txid):
    job = node.call("mining.getjob", [{"address":address}])
    require(not job.get("error") and job.get("job_id"), f"job refused: {job}")
    header = bytearray.fromhex(job["header_hex"])
    require(len(header) == 128 and job["nonce_size"] == 4, "unexpected mining header")
    for nonce in range(1000000):
        struct.pack_into("<I", header, job["nonce_offset"], nonce)
        digest = hashlib.sha256(hashlib.sha256(header).digest()).digest()
        if int.from_bytes(digest, "big") <= int(job["target"], 16):
            break
    else:
        raise AssertionError("regtest nonce search exhausted")
    require(node.call("mining.submit", [{"job_id":job["job_id"], "nonce":nonce}]) in (None, {}), "job submission rejected")
    require(node.call("getbestblockhash") == digest.hex(), "submitted job did not become selected tip")
    require(node.call("getblock", [digest.hex(), 1])["tx"][1:] == [expected_txid], "accepted job dropped foreign input")
    require(not node.call("getrawmempool"), "confirmed foreign transaction retained in pool")
    return digest.hex()


try:
    source = Node("source")
    miner = Node("miner")
    source_address = source.call("wallet.createhd", ["source-owner", "", False])["first_address"]
    mining_address = miner.call("wallet.createhd", ["mining-owner", "", False])["first_address"]
    require(source_address != mining_address, "wallet identities must differ")
    source.call("generatetoaddress", [101, source_address])
    sync_blocks(source, miner)
    require(not miner.call("wallet.listunspent", [0, 9999999]), "miner must not own source funding")
    print("PASS independent full node holds source chain without source wallet coins", flush=True)

    for round_index in range(2):
        recipient = source.call("wallet.getnewaddress")["address"]
        sent = source.call("wallet.sendtoaddress", [recipient, 1.0])
        txid = sent["txid"] if isinstance(sent, dict) else sent
        raw = source.call("wallet.getrawtransaction", [txid])["hex"]
        admitted = miner.call("wallet.sendrawtransaction", [raw])
        while isinstance(admitted, dict):
            admitted = admitted.get("txid", admitted.get("result"))
        require(admitted == txid and set(miner.call("getrawmempool")) == {txid}, "foreign transaction not admitted")
        before = template(miner, mining_address)
        require(txids(before) == {txid} and before["totalfees"] > 0, "chain template omitted foreign input")
        blockhash = solve_job(miner, mining_address, txid)
        require(miner.call("getblockcount") == before["height"], "job height differs from chain template")
        sync_blocks(miner, source)
        miner.stop(); miner.start()
        require(miner.call("getbestblockhash") == blockhash and not miner.call("getrawmempool"), "restart lost accepted foreign spend")
        print("PASS foreign-wallet job accepted and reopened round", round_index + 1, flush=True)
    SUCCESS = True
finally:
    for node in NODES:
        node.stop()
    if SUCCESS and not os.environ.get("KEEP_DATADIR"):
        shutil.rmtree(WORK)
    else:
        print("Mining chain provider evidence:", WORK, flush=True)
