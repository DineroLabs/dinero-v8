#!/usr/bin/env python3
"""Stateful snapshot restart must retain live base coins without reviving spends.

Two real pre-base spends straddle a cold restart. The second block used to
stall because startup reloaded only the post-base ChainDB coin family.
All wallets, blocks and files are synthetic, offline and owned by this test.
"""
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
BINARY = Path(os.environ.get("DINEROD", ROOT / "build/dinerod")).resolve()
WORK = Path(tempfile.mkdtemp(prefix="dinero_stateful_snapshot_restart_"))
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
    def __init__(self, name, headers=None, dormant=False, extra=()):
        self.path = WORK / name
        self.path.mkdir()
        if headers:
            shutil.copytree(headers, self.path / "headers")
        self.rpc_port, self.p2p, self.wallet = ports()
        self.extra = ["--consensus-state-commitment-height=4294967295"] if dormant else []
        self.extra.extend(extra)
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
            require(self.process.returncode == 0, f"daemon shutdown failed: {self.path}")
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=10)
            raise AssertionError(f"daemon required SIGKILL: {self.path}")
        finally:
            self.log.close()
            self.process = None

    def state(self):
        # Stable public state, including both consensus containers and lifecycle.
        state = {m:self.call(m) for m in ["getblockcount", "getbestblockhash",
            "blockchain.getutreexocommitment", "daemon.shieldedroot"]}
        status = self.call("getsnapshotbootstrapstatus")["snapshot_bootstrap"]
        state["lifecycle"] = {k:status.get(k) for k in ["assumeutxo_active",
            "snapshot_base_height", "snapshot_base_block", "history_validation_state", "fatal"]}
        require("assumeutxo_active" in status and "history_validation_state" in status, "missing lifecycle observability")
        return state

    def logtext(self):
        return (self.path / "daemon.log").read_text(errors="replace")

def state(node):
    chain = node.call("getblockchaininfo")
    root = node.call("blockchain.getutreexocommitment")
    return {"height": chain["blocks"], "hash": chain["bestblockhash"],
            "supply": chain["moneysupply"],
            "root": {key: root[key] for key in
                     ("verified_height", "commitment", "num_leaves", "num_roots")}}

def outpoint(vin):
    return {"txid": vin["txid"], "vout": vin["vout"]}

def send(node, address, amount):
    sent = node.call("wallet.sendtoaddress", [address, amount])
    txid = sent["txid"] if isinstance(sent, dict) else sent
    decoded = node.call("wallet.getrawtransaction", [txid, True])
    inputs = []
    for vin in decoded["vin"]:
        coin = node.call("gettxout", [vin["txid"], vin["vout"], False])
        if coin and coin["height"] <= 130:
            inputs.append(outpoint(vin))
    require(inputs, "fixture must spend an unspent snapshot-base coin")
    node.call("generate", [1])
    blockhash = node.call("getbestblockhash")
    block = node.call("getblock", [blockhash, 1])
    require(txid in block["tx"], "fixture spend must be mined")
    return inputs, node.call("getblock", [blockhash, 0]), state(node)

try:
    source = Node("source", dormant=True)
    source.call("generate", [130])
    snapshot = WORK / "base.dat"
    source.call("dumptxoutset", [str(snapshot)])
    address = source.call("wallet.getnewaddress")
    if isinstance(address, dict):
        address = address["address"]
    spent, first, before = send(source, address, 50.0)
    remaining, second, expected = send(source, address, 250.0)
    require(not set((c["txid"], c["vout"]) for c in spent) &
            set((c["txid"], c["vout"]) for c in remaining), "fixture inputs overlap")
    source.stop()
    consumer = Node("full", source.path / "headers", dormant=True,
        extra=["--sync-profile=mac_fullblock", "--utreexo-stateless=0",
               "--assumeutxo_forward_connect=1", "--utreexo.checkpoint_interval=1000"])
    require(consumer.call("loadtxoutset", [str(snapshot)])["base_height"] == 130,
            "snapshot was not admitted at the expected base")
    consumer.call("submitblock", [first])
    require(state(consumer) == before, "first post-base spend did not match source")
    consumer.stop()
    consumer.extra.append("--assumeutxo_snapshot=" + str(snapshot))
    consumer.start()
    require(state(consumer) == before, "restart changed tip, supply or accumulator")
    for coin in spent:
        require(consumer.call("gettxout", [coin["txid"], coin["vout"], False]) is None,
                "restart resurrected a spent snapshot coin")
    consumer.call("submitblock", [second])
    deadline = time.monotonic() + 10
    while consumer.call("getblockcount") < 132 and time.monotonic() < deadline:
        time.sleep(0.1)
    require(consumer.call("getblockcount") == 132,
            "second pre-base spend stalled at height 131 after stateful snapshot restart")
    require(state(consumer) == expected,
            "second pre-base spend failed after stateful snapshot restart")
    require(consumer.call("safemode.status")["active"] is False, "restart entered safe mode")
    consumer.stop()
    consumer.start()
    require(state(consumer) == expected, "second restart changed exact canonical state")
    for coin in spent + remaining:
        require(consumer.call("gettxout", [coin["txid"], coin["vout"], False]) is None,
                "second restart resurrected a spent snapshot coin")
    SUCCESS = True
    print("PASS: full snapshot restart preserves live base spends and excludes spent coins", flush=True)
finally:
    for node in NODES:
        node.stop()
    if SUCCESS and not os.environ.get("STATEFUL_SNAPSHOT_KEEP"):
        shutil.rmtree(WORK)
    else:
        print("Evidence retained at", WORK, flush=True)
