#!/usr/bin/env python3
"""Request-local GBT exclusions must rebuild fees and every block commitment."""
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
WORK = Path(tempfile.mkdtemp(prefix="dinero_template_exclusions_"))
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
    wallet = node.call("wallet.createhd", ["exclusion-regression", "", False])
    address = wallet["first_address"]
    node.call("generatetoaddress", [101, address])
    node.call("wallet.shield", [1.0])
    node.call("generatetoaddress", [1, address])
    unshield = node.call("wallet.unshield", [1.0])["txid"]
    shield = node.call("wallet.shield", [1.0])["txid"]
    original = template(node, address)
    require(txids(original) == {unshield, shield}, "mixed shield/unshield fixture missing")

    # Test exclusion before malformed-parameter checks: old daemons silently
    # ignore this field, so this must fail red against the pre-fix binary.
    filtered = template(node, address, [unshield])
    require(txids(filtered) == {shield}, "exclude_txids was ignored or removed the wrong transaction")
    require(filtered["statecommitment"]["root"] != original["statecommitment"]["root"],
            "removing unshield did not rebuild DNRS")
    remaining_fee = sum(tx["fee"] for tx in filtered["transactions"])
    removed_fee = next(tx["fee"] for tx in original["transactions"] if tx["txid"] == unshield)
    require(filtered["totalfees"] == remaining_fee, "filtered fee total is wrong")
    require(original["coinbasevalue"] - filtered["coinbasevalue"] == removed_fee,
            "coinbase still claims excluded fees")
    require(txids(template(node, address)) == {shield, unshield}, "exclusion mutated the mempool")
    equivalent = template(node, address, [unshield.upper(), unshield, "11" * 32])
    require(txids(equivalent) == {shield}, "duplicates, uppercase or unknown txid changed selection")
    require(txids(template(node, address, [])) == {shield, unshield}, "empty exclusions changed selection")

    for invalid in ["11" * 32, None, [3], ["11"], ["gg" * 32], ["11" * 32] * 10001]:
        reply = node.raw("getblocktemplate", [{"address": address, "exclude_txids": invalid}])
        error = reply.get("error") or (reply.get("result") or {}).get("error")
        require(error, f"invalid exclusions accepted: {str(invalid)[:100]}")
    print("PASS exclusion validation, fee accounting, DNRS prediction and request isolation", flush=True)

    # Mine the unshield first: a later tree update can invalidate its old
    # anchor under the current daemon's height rules. Request-local exclusion
    # preserves mempool membership, not validity at every future height.
    recovery = template(node, address, [shield])
    require(txids(recovery) == {unshield}, "reverse exclusion selected wrong tx")
    mine_template(node, address, recovery)
    require(set(node.call("getrawmempool")) == {shield}, "excluded tx was evicted")
    # All-excluded next block still advances shielded anchor history. Its DNRS
    # cannot be replaced by the current state digest or the parent's script.
    empty = template(node, address, [shield])
    require(not txids(empty), "all-excluded template not empty")
    require(empty["totalfees"] == 0, "empty template claims fees")
    mine_template(node, address, empty)
    require(set(node.call("getrawmempool")) == {shield}, "empty template evicted excluded tx")
    final = template(node, address)
    require(txids(final) == {shield}, "excluded transaction not selectable later")
    mine_template(node, address, final)
    require(not node.call("getrawmempool"), "remaining shield did not confirm")
    print("PASS filtered, all-excluded and later retry blocks accepted with DNRS", flush=True)
    SUCCESS = True
finally:
    for node in NODES:
        node.stop()
    if SUCCESS and not os.environ.get("KEEP_DATADIR"):
        shutil.rmtree(WORK)
    else:
        print("Template exclusion evidence:", WORK, flush=True)
