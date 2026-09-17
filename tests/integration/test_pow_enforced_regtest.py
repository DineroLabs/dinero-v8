#!/usr/bin/env python3
"""Real solved work, admission negatives, ASERT activation and Utreexo recovery.

This is an enforcement test, not a wall-clock cadence benchmark.
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
import sys
import tempfile
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
BINARY = Path(os.environ.get("DINEROD", ROOT / "build/dinerod"))
sys.path.insert(0, str(ROOT / "tests/mining"))
from dinero_cpu_miner import DineroCoinMiner

WORK = Path(tempfile.mkdtemp(prefix="dinero-pow-enforced-"))
NODES = []


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def wait(predicate):
    deadline = time.monotonic() + 120
    while time.monotonic() < deadline:
        try:
            if predicate():
                return
        except (OSError, ValueError):
            pass
        time.sleep(.2)
    raise AssertionError("timeout waiting for chain convergence")


class Node:
    def __init__(self, name, csn=False):
        self.path = WORK / name
        self.path.mkdir()
        sockets = [socket.socket() for _ in range(3)]
        for sock in sockets:
            sock.bind(("127.0.0.1", 0))
        self.rpc, self.p2p, self.wallet = [s.getsockname()[1] for s in sockets]
        for sock in sockets:
            sock.close()
        self.csn, self.peer, self.process = csn, None, None
        NODES.append(self)

    def args(self, mode=True, height=4):
        return [str(BINARY), "--regtest", f"--datadir={self.path}",
                f"--rpcport={self.rpc}", f"--port={self.p2p}",
                f"--wallet-socket-port={self.wallet}", "--utreexo=1", "--listen=1",
                "--utreexo-stateless=1" if self.csn else "--utreexo-bridge=1",
                f"--consensus-sixty-second-height={height}",
                *(["--regtest-enforce-pow"] if mode else []),
                *([f"--connect=127.0.0.1:{self.peer.p2p}"] if self.peer else [])]

    def start(self, extra=()):
        self.log = (WORK / (self.path.name + ".log")).open("ab")
        self.process = subprocess.Popen(self.args() + list(extra), stdout=self.log, stderr=self.log)
        def ready():
            require(self.process.poll() is None, f"daemon exited: {self.path}")
            return self.call("getblockcount") >= 0
        wait(ready)

    def raw(self, method, params=None):
        cookie = (self.path / ".cookie").read_bytes().strip()
        request = urllib.request.Request(f"http://127.0.0.1:{self.rpc}/",
            json.dumps({"jsonrpc":"2.0", "id":1, "method":method, "params":params or []}).encode(),
            {"Content-Type":"application/json", "Authorization":"Basic " + base64.b64encode(cookie).decode()})
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
        process, self.process = self.process, None
        process.terminate()
        try:
            process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            raise AssertionError("daemon required SIGKILL")
        finally:
            self.log.close()
        require(process.returncode == 0, f"daemon exit {process.returncode}")


def digest(block):
    return hashlib.sha256(hashlib.sha256(block[:128]).digest()).digest()


def target(bits):
    exponent, mantissa = bits >> 24, bits & 0x7fffff
    require(not bits & 0x800000, "negative target")
    return mantissa << (8 * (exponent - 3)) if exponent >= 3 else mantissa >> (8 * (3 - exponent))


def nonce_for(block, valid):
    block = bytearray(block)
    limit = target(struct.unpack_from("<I", block, 108)[0])
    for nonce in range(4_000_000):
        struct.pack_into("<I", block, 112, nonce)
        if (int.from_bytes(digest(block), "big") <= limit) == valid:
            return bytes(block)
    raise AssertionError("nonce search exhausted")


def reject(node, block, reason):
    before = node.call("getbestblockhash")
    reply = node.raw("submitblock", [block.hex()])
    require(reason in json.dumps(reply), f"expected {reason}, got {reply}")
    require(node.call("getbestblockhash") == before, "rejected work changed active tip")


def startup_reject(args, reason):
    result = subprocess.run(args, capture_output=True, text=True, timeout=20)
    require(result.returncode != 0 and reason in result.stdout + result.stderr,
            f"startup rejection {reason}: {result}")


success = False
try:
    full, csn = Node("full"), Node("csn", True)
    full.peer, csn.peer = csn, full
    full.start()
    csn.start()
    address = full.call("wallet.getnewaddress")["address"]
    miner = DineroCoinMiner(f"http://127.0.0.1:{full.rpc}", str(full.path / ".cookie"), address)
    require(miner.load_cookie_auth(), "miner cookie")
    hashes, roots = {}, {}
    for height in range(1, 7):
        template = miner.get_block_template()
        require(template and template.height == height, "template height")
        if height < 4:
            # Historical arithmetic is intentionally preserved before activation.
            # With regtest's huge target, starting months after genesis wraps the
            # old multiply. Bootstrap a genuinely mined historical-time prefix;
            # use ordinary wall-clock templates once the new arithmetic activates.
            # Literal bits also checked by the header-path boundary fixture.
            template.curtime = full.call("getconsensusinfo")["genesis_time"] + height * 120
            template.time_mutable = False
            template.bits = "207fffff" if height == 1 else "1f00fc9c"
            template.target = f"{target(int(template.bits, 16)):064x}"
        solved = miner.mine_block(template, 4_000_000)
        require(solved is not None, "real work not found")
        block = solved[0]
        require(int.from_bytes(digest(block), "big") <= target(int(template.bits, 16)), "independent PoW")
        reject(full, nonce_for(block, False), "bad-pow")
        wrong = bytearray(block)
        # Solve the wrong target as well: rejection must be for ASERT, not hash.
        bits = 0x207fffff if int(template.bits, 16) != 0x207fffff else 0x203fffff
        struct.pack_into("<I", wrong, 108, bits)
        reject(full, nonce_for(wrong, True), "bad-diffbits")
        require(full.call("submitblock", [block.hex()]) in (None, {}), "valid block rejected")
        hashes[height] = digest(block).hex()
        wait(lambda: csn.call("getbestblockhash") == hashes[height])
        a = full.call("blockchain.getutreexocommitment")
        b = csn.call("blockchain.getutreexocommitment")
        require(a["commitment"] == b["commitment"] and b["verified_height"] == height, "CSN root mismatch")
        roots[height] = a["commitment"]
        info = full.call("getconsensusinfo")
        require(info["regtest_pow_enforced"] is True, "mode not reported")
        require(info["target_spacing_seconds"] == (120 if height < 3 else 60), "timing boundary")
        print(f"PASS solved work and admission negatives at height {height}, bits {template.bits}", flush=True)

    for node in (csn, full):
        node.call("blockchain.invalidateblock", [hashes[3]])
        require(node.call("getbestblockhash") == hashes[2], "rollback tip")
    for node in (full, csn):
        require(node.call("blockchain.getutreexocommitment")["commitment"] == roots[2], "rollback root")
        node.stop()
        startup_reject(node.args(mode=False), "PoW profile")
        startup_reject(node.args(height=5), "PoW profile")
        node.start()
        require(node.call("getbestblockhash") == hashes[2], "restart rollback")
        node.call("blockchain.reconsiderblock", [hashes[3]])
        wait(lambda: node.call("getbestblockhash") == hashes[6])
        require(node.call("blockchain.getutreexocommitment")["commitment"] == roots[6], "restored root")
    full.stop()
    full.start(["--reindex"])
    wait(lambda: full.call("getbestblockhash") == hashes[6])
    require(full.call("blockchain.getutreexocommitment")["commitment"] == roots[6], "reindex root")
    for network in ("--mainnet", "--testnet"):
        startup_reject([str(BINARY), network, "--regtest-enforce-pow", f"--datadir={WORK/'denied'}"], "REGTEST-only")
    existing = WORK / "existing"
    existing.mkdir()
    (existing / "unknown-state").write_text("do not migrate")
    startup_reject([str(BINARY), "--regtest", "--regtest-enforce-pow", f"--datadir={existing}"], "fresh datadir")
    require((existing / "unknown-state").read_text() == "do not migrate", "existing state modified")
    success = True
    print("PASS PoW-enforced full/CSN activation, negatives, profile isolation and recovery", flush=True)
finally:
    errors = []
    for node in NODES:
        try:
            node.stop()
        except Exception as error:
            errors.append(str(error))
    if success and not errors:
        shutil.rmtree(WORK)
    else:
        print("Retained evidence:", WORK, flush=True)
    if errors:
        raise AssertionError(errors)
