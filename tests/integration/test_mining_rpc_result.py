#!/usr/bin/env python3
"""Benign actual mining RPC template/result checks on isolated patched daemons."""
import base64
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
WORK = Path(tempfile.mkdtemp(prefix="dinero_mining_rpc_result_"))
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


try:
    node = Node("active")
    wallet = node.call("wallet.createhd", ["mining-result", "", False])
    address = wallet["first_address"]
    node.call("generatetoaddress", [101, address])
    recipient = node.call("wallet.getnewaddress")["address"]
    sent = node.call("wallet.sendtoaddress", [recipient, 1.0])
    txid = sent["txid"] if isinstance(sent, dict) else sent
    require(isinstance(txid, str) and len(txid) == 64, "missing admitted transaction ID")
    entry = node.call("mempool.getmempoolentry", [txid])
    # This established context RPC expresses base fees in DIN.
    expected_fee = round(entry["fees"]["base"] * 100000000)
    require(expected_fee > 0, "fixture must pay a positive fee")
    original = template(node, address)
    require(txids(original) == {txid}, "admitted transaction absent from template")
    require(original["transactions"][0]["fee"] == expected_fee, "template fee differs from admitted fee")
    require(original["totalfees"] == expected_fee, "template total differs from its sole accepted fee")
    print("PASS actual RPC returns accepted body and exact transaction fees", flush=True)

    filtered = template(node, address, [txid])
    require(not txids(filtered) and filtered["totalfees"] == 0, "excluded result retained fees or body")
    require(original["coinbasevalue"] - filtered["coinbasevalue"] == expected_fee, "excluded coinbase retained fee")
    require(set(node.call("getrawmempool")) == {txid}, "template request mutated pool")
    job = node.call("mining.getjob", [{"address": address}])
    require(not job.get("error") and bool(job["job_id"]), "mining job construction refused")
    require(job["height"] == original["height"] and job["prev_hash"] == node.call("getbestblockhash"), "job parent changed")
    require(len(bytes.fromhex(job["header_hex"])) > 80, "job header missing extended fields")
    require(set(node.call("getrawmempool")) == {txid}, "job request mutated pool")
    print("PASS exclusion and actual mining.getjob preserve the admitted pool", flush=True)

    restored = template(node, address)
    require(txids(restored) == {txid} and restored["totalfees"] == expected_fee, "retry lost captured result")
    mine_template(node, address, restored)
    require(not node.call("getrawmempool"), "accepted template transaction did not leave pool")
    empty = template(node, address)
    require(not txids(empty) and empty["totalfees"] == 0, "later empty result inherited transaction fees")
    print("PASS returned RPC template accepted and later response has no stale fees", flush=True)
    SUCCESS = True
finally:
    for node in NODES:
        node.stop()
    if SUCCESS and not os.environ.get("KEEP_DATADIR"):
        shutil.rmtree(WORK)
    else:
        print("Mining RPC result evidence:", WORK, flush=True)
