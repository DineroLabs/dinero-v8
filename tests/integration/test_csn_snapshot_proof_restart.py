#!/usr/bin/env python3
"""Snapshot coins and post-base proof coverage survive sparse-checkpoint restart.

A real post-snapshot spend is essential: replay advances the forest but used to
leave the snapshot coin view stale. All fixtures are synthetic and offline.
"""
import base64
import copy
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
BINARY = Path(os.environ.get("DINEROD", ROOT / "build/dinerod"))
WORK = Path(tempfile.mkdtemp(prefix="dinero_csn_snapshot_proof_restart_"))
NODES = []
CHECKS = []
SUCCESS = False


def check(name, condition, observed=None):
    CHECKS.append({"name": name, "passed": bool(condition), "observed": observed})
    (WORK / "checks.json").write_text(json.dumps(CHECKS, indent=2) + "\n")
    print(("PASS " if condition else "FAIL ") + name, flush=True)


class Node:
    def __init__(self, name, headers=None, extra=()):
        self.path = WORK / name
        self.path.mkdir()
        if headers:
            shutil.copytree(headers, self.path / "headers")
        sockets = [socket.socket() for _ in range(3)]
        for sock in sockets:
            sock.bind(("127.0.0.1", 0))
        self.ports = [sock.getsockname()[1] for sock in sockets]
        for sock in sockets:
            sock.close()
        self.extra = list(extra)
        self.process = None
        NODES.append(self)
        self.start()

    def start(self):
        self.log = open(self.path / "daemon.log", "ab")
        self.process = subprocess.Popen([str(BINARY), "--regtest", f"--datadir={self.path}",
            f"--rpcport={self.ports[0]}", f"--port={self.ports[1]}",
            f"--wallet-socket-port={self.ports[2]}", "--p2p.offline=1", "--listen=0",
            "--utreexo=1", *self.extra], stdout=self.log, stderr=self.log)
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            assert self.process.poll() is None, f"daemon exited: {self.path}"
            try:
                self.call("getblockcount")
                return
            except (OSError, ValueError, RuntimeError):
                time.sleep(0.25)
        raise AssertionError(f"RPC not ready: {self.path}")

    def call(self, method, params=None):
        cookie = (self.path / ".cookie").read_bytes().strip()
        request = urllib.request.Request(f"http://127.0.0.1:{self.ports[0]}/",
            data=json.dumps({"jsonrpc": "2.0", "id": 1, "method": method,
                             "params": [] if params is None else params}).encode(),
            headers={"Authorization": "Basic " + base64.b64encode(cookie).decode(),
                     "Content-Type": "application/json"})
        try:
            response = urllib.request.urlopen(request, timeout=60)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            reply = json.load(response)
        if reply.get("error"):
            raise RuntimeError(f"{method}: {reply['error']}")
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
            raise AssertionError(f"unclean shutdown: {self.path}")
        finally:
            self.log.close()
            self.process = None

    def state(self):
        commitment = self.call("blockchain.getutreexocommitment")
        return {"height": self.call("getblockcount"), "hash": self.call("getbestblockhash"),
                "utreexo": {k: commitment[k] for k in
                    ("commitment", "num_leaves", "num_roots", "verified_height")},
                "shielded": self.call("daemon.shieldedroot")}


def proofs(node, stage, outpoint):
    generated = node.call("blockchain.getutxoproofs_batch", [[outpoint]])
    ok = generated["successful"] == 1 and generated["failed"] == 0
    check(stage + ": generated", ok, generated)
    if ok:
        verified = node.call("blockchain.verifyutxoproofs_batch", [generated["proofs"]])
        check(stage + ": verified", verified["valid"] == 1 and verified["invalid"] == 0, verified)
        altered = copy.deepcopy(generated["proofs"])
        assert altered[0]["proof"]["siblings"], "fixture must have a nontrivial proof"
        altered[0]["proof"]["siblings"][0] = "0" * 64
        rejected = node.call("blockchain.verifyutxoproofs_batch", [altered])
        check(stage + ": altered sibling rejected", rejected["valid"] == 0 and rejected["invalid"] == 1
              and rejected["results"][0]["error_code"] == "proof-invalid", rejected)
    try:
        single = node.call("blockchain.getutxoproof", [outpoint["txid"], outpoint["vout"]])
        check(stage + ": single proof", "leaf_hash" in single and "siblings" in single, single)
    except RuntimeError as error:
        check(stage + ": single proof", False, str(error))


try:
    source = Node("source")
    source.call("generate", [130])
    snapshot = WORK / "base.dat"
    source.call("dumptxoutset", [str(snapshot)])
    address = source.call("wallet.getnewaddress")
    if isinstance(address, dict):
        address = address["address"]
    sent = source.call("wallet.sendtoaddress", [address, 50.0])
    txid = sent["txid"] if isinstance(sent, dict) else sent
    decoded = source.call("wallet.getrawtransaction", [txid, True])
    spent = {"txid": decoded["vin"][0]["txid"], "vout": decoded["vin"][0]["vout"]}
    spent_coin = source.call("gettxout", [spent["txid"], spent["vout"]])
    assert spent_coin["coinbase"] and spent_coin["height"] <= 130
    old_proof = source.call("blockchain.getutxoproofs_batch", [[spent]])["proofs"]
    assert old_proof[0]["success"]
    source.call("generate", [8])
    spend_block = source.call("getblock", [source.call("getblockhash", [131]), 1])
    assert txid in spend_block["tx"], "spend did not confirm in the first forward block"
    candidates = source.call("wallet.listunspent", [0, 9999999])
    prebase = next({"txid": coin["txid"], "vout": coin["vout"]} for coin in candidates
        if (canonical := source.call("gettxout", [coin["txid"], coin["vout"]]))
        and canonical.get("height", 131) <= 130 and canonical.get("value_una", 0) > 0)
    postbase = {"txid": txid, "vout": 0}
    assert source.call("gettxout", [txid, 0])["value_una"] > 0
    expected = source.state()
    blocks = [source.call("getblock", [source.call("getblockhash", [height]), 0])
              for height in range(131, 139)]
    source.stop()
    node = Node("csn", source.path / "headers", extra=["--utreexo-stateless=1",
        "--sync-profile=ios_utreexo", "--assumeutxo_forward_connect=1",
        "--utreexo.checkpoint_interval=1000"])
    assert node.call("loadtxoutset", [str(snapshot)])["base_height"] == 130
    for height, block in enumerate(blocks, 131):
        node.call("submitblock", [block])
        assert node.call("getblockcount") == height
    assert node.state() == expected, "forward roots differ from full source"
    for stage in ("forward", "restart-1", "restart-2"):
        if stage != "forward":
            node.stop()
            if "--assumeutxo_snapshot=" + str(snapshot) not in node.extra:
                node.extra.append("--assumeutxo_snapshot=" + str(snapshot))
            node.start()
        check(stage + ": roots/tip unchanged", node.state() == expected, node.state())
        safe = node.call("safemode.status")
        check(stage + ": no safe mode", safe["active"] is False, safe)
        proofs(node, stage + " prebase", prebase)
        proofs(node, stage + " postbase", postbase)
        rejected = node.call("blockchain.verifyutxoproofs_batch", [old_proof])
        check(stage + ": spent snapshot coin rejected", rejected["valid"] == 0 and rejected["invalid"] == 1, rejected)
        spent_result = node.call("blockchain.getutxoproofs_batch", [[spent]])
        check(stage + ": spent snapshot proof unavailable", spent_result["successful"] == 0
              and spent_result["failed"] == 1
              and spent_result["proofs"][0]["error_code"] == "utxo-not-found", spent_result)
        check(stage + ": spent lookup does not trigger recovery", node.call("safemode.status")["active"] is False)
    failed = [c["name"] for c in CHECKS if not c["passed"]]
    assert not failed, "Failed requirements: " + ", ".join(failed)
    SUCCESS = True
    print("SUCCESS: snapshot proof coverage, sparse restart and spent-coin rejection", flush=True)
finally:
    for node in NODES:
        node.stop()
    if SUCCESS and not os.environ.get("CSN_PROOF_KEEP"):
        shutil.rmtree(WORK)
    else:
        print("Evidence retained at", WORK, flush=True)
