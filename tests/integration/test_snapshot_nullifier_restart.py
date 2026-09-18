#!/usr/bin/env python3
"""A nonempty imported nullifier must survive forward progress and restart.

Uses real regtest shield/unshield transactions and enforced v5 snapshot binding.
No wallet secrets or device data are used.
"""
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
import sqlite3
import time
import urllib.request
import urllib.error

ROOT = Path(__file__).resolve().parents[2]
BINARY = Path(os.environ.get("DINEROD", ROOT / "build/dinerod"))
WORK = Path(tempfile.mkdtemp(prefix="dinero_snapshot_nullifier_restart_"))
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


def cache_count(node):
    # Synthetic stopped fixture only. ChainDB remains the authority; this
    # checks the observable consequence of startup cache reconciliation.
    path = node.path / "blockchain/shielded_nullifiers.db"
    with sqlite3.connect(path.as_uri() + "?mode=ro", uri=True) as db:
        return db.execute("SELECT COUNT(*) FROM nullifiers").fetchone()[0]


def stable_state(node):
    state = {method: node.call(method) for method in (
        "getblockcount", "getbestblockhash", "blockchain.getutreexocommitment",
        "daemon.shieldedroot")}
    # Compare consensus state across full/compact roles. The RPC also reports
    # each node's storage role, memory estimate and live peer count.
    commitment = state["blockchain.getutreexocommitment"]
    state["blockchain.getutreexocommitment"] = {
        key: commitment[key] for key in (
            "commitment", "num_leaves", "num_roots", "verified_height")}
    return state


try:
    # A real spend is essential: shield-only/empty-pool snapshots have no
    # imported nullifier to lose and passed before the persistence fix.
    source = Node("source")
    source.call("generate", [130])
    source.call("wallet.shield", [100])
    source.call("generate", [2])
    spend = source.call("wallet.unshield", {"amount": 50.0})
    require(bool(spend.get("nullifier_hex")), "real unshield must create a nullifier")
    source.call("generate", [2])
    base_height = source.call("getblockcount")
    snapshot = WORK / "nonempty.dat"
    source.call("dumptxoutset", [str(snapshot)])
    blob = snapshot.read_bytes()
    require(struct.unpack_from("<II", blob) == (0x4f545855, 5), "must exercise bound v5 snapshot")
    require(hashlib.sha256(blob[:-32]).digest() == blob[-32:], "snapshot checksum")
    # Bury the base and supply verified ancestry for the enforced binding gate.
    source.call("generate", [8])
    expected = stable_state(source)
    source.stop()
    require(cache_count(source) == 1, "source must have exactly one real nullifier")
    shutil.copytree(source.path / "headers", WORK / "buried_headers")
    source.start()

    for profile, flags in (("full", []), ("csn", ["--utreexo-stateless=1"])):
        node = Node(profile, WORK / "buried_headers", extra=[
            "--assumeutxo_forward_connect=1", "--utreexo.checkpoint_interval=1", *flags])
        imported = node.call("loadtxoutset", [str(snapshot)])
        require(imported["base_height"] == base_height, "wrong imported base")
        require("v5 binding VERIFIED:" in node.logtext() and "(enforced)" in node.logtext(),
                "v5 consensus binding must be enforced")
        if profile == "csn":
            require("mode=STATELESS" in node.logtext(), "CSN case must be stateless")
            role = node.call("blockchain.getutreexocommitment")
            require(role["sync_profile"] == "ios_utreexo" and
                    role["validation_role"] == "compact_validator" and
                    role["retains_full_state"] is False,
                    "CSN case must use the phone's compact validation role")
        # The base-only case can hide lost nullifiers by loading the snapshot
        # again. Forward progress is what makes checkpoint restore skip that.
        for height in range(base_height + 1, base_height + 9):
            time.sleep(0.15)
            block_hash = source.call("getblockhash", [height])
            node.call("submitblock", [source.call("getblock", [block_hash, 0])])
        observed = stable_state(node)
        require(observed == expected, f"{profile}: forward state differs: expected={expected}, observed={observed}")
        node.stop()
        require(cache_count(node) == 1, f"{profile}: missing nullifier before restart")
        node.extra.append("--assumeutxo_snapshot=" + str(snapshot))
        # Two restarts catch both the initial loss and cache provenance drift.
        for attempt in range(2):
            node.start()
            require(stable_state(node) == expected, f"{profile}: restart {attempt} changed Utreexo/shielded state")
            node.stop()
            require(cache_count(node) == 1, f"{profile}: restart {attempt} lost imported nullifier")
        require("shielded-tip-marker-state-mismatch" not in node.logtext(),
                f"{profile}: shielded startup mismatch")
        require("stamped as a cache — treating as post-crash residue" not in node.logtext(),
                f"{profile}: committed snapshot nullifier discarded")
        print(f"PASS {profile}: nonempty v5 import, forward progress and two restarts", flush=True)
    SUCCESS = True
    print("SUCCESS: snapshot nullifier durability (full and CSN)", flush=True)
except Exception:
    print("FAIL: retained snapshot nullifier evidence at", WORK, flush=True)
    raise
finally:
    for node in NODES:
        node.stop()
    if SUCCESS and not os.environ.get("SNAPSHOT_NULLIFIER_KEEP"):
        shutil.rmtree(WORK)
