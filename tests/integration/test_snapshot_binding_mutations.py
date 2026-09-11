#!/usr/bin/env python3
"""Gate D: checksum-valid forgeries reach the real loadtxoutset boundary.

Each case uses an isolated empty consumer with independently persisted PoW
headers. Rejections assert their precise class and unchanged state after restart.
No production activation override is changed; dormancy controls are regtest-only.
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
import time
import urllib.request
import urllib.error

ROOT = Path(__file__).resolve().parents[2]
BINARY = Path(os.environ.get("DINEROD", ROOT / "build/dinerod"))
WORK = Path(tempfile.mkdtemp(prefix="dinero_gate_d_"))
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

def fixture(data, name):
    data = bytes(data)
    p = WORK / (name + ".dat")
    p.write_bytes(data + hashlib.sha256(data).digest())
    return p

def offsets(data):
    require(struct.unpack_from("<II", data) == (0x4f545855, 5), "export must be v5")
    count = struct.unpack_from("<Q", data, 44)[0]
    pos = 68
    for _ in range(count):
        size = struct.unpack_from("<I", data, pos + 44)[0]
        pos += 53 + size
    require(data[pos:pos+4] == b"UTRX", "UTRX framing")
    forest_size = struct.unpack_from("<Q", data, pos+8)[0]
    shld = pos + 64 + forest_size
    require(data[shld:shld+4] == b"SHLD", "SHLD framing")
    frontier, anchors, nullifiers = struct.unpack_from("<QQQ", data, shld+8)
    bind = shld + 72 + frontier + anchors + nullifiers
    require(data[bind:bind+4] == b"BIND", "BIND framing")
    cbsize, branches = struct.unpack_from("<QI", data, bind+8)
    require(bind+20+cbsize+32*branches == len(data), "exact binding length")
    return shld, shld+72+frontier, bind, cbsize, branches

def attempt(name, path, headers, expected=None, dormant=False, verified=False, extra=()):
    blob = path.read_bytes()
    require(hashlib.sha256(blob[:-32]).digest() == blob[-32:], f"{name}: fixture checksum invalid")
    node = Node(name, headers, dormant, extra)
    before = node.state()
    require(before["getblockcount"] == 0 and before["blockchain.getutreexocommitment"]["num_leaves"] == 0, f"{name}: consumer must start empty")
    response = node.raw("loadtxoutset", [str(path)])
    message = str(response.get("error", ""))
    if expected:
        require(response.get("error") and expected in message, f"{name}: wrong rejection: {response}")
        require("Checksum verified successfully" in node.logtext() or name in ("v4_enforced", "base_hash"), f"{name}: did not reach checksum-valid path")
        after = node.state()
        require(after == before, f"{name}: rejected import mutated state: before={before}, after={after}")
        node.stop(); node.start()
        require(node.state() == before, f"{name}: rejected import persisted state across restart")
        require("AssumeUTXO mode ACTIVE - UTXO set loaded" not in node.logtext(), f"{name}: published failed snapshot")
    else:
        require(not response.get("error") and response.get("result", {}).get("coins_loaded", 0)>0, f"{name}: valid control failed: {response}")
        if verified:
            require("v5 binding VERIFIED:" in node.logtext() and "(enforced)" in node.logtext(), f"{name}: enforcement did not verify")
        if name == "payload_dormant":
            require("commitment-mismatch" in node.logtext() and "advisory: state commitment dormant" in node.logtext(), "missing advisory control")
    node.stop()
    RESULTS.append({"case":name, "expected":expected or "accepted", "result":"passed", "snapshot_sha256":hashlib.sha256(blob).hexdigest(),
        "observed_error":response.get("error"), "header_context":headers.name,
        "state_preservation_checked":bool(expected),
        "pre_state_sha256":hashlib.sha256(json.dumps(before,sort_keys=True).encode()).hexdigest() if expected else None})
    print("PASS", name, expected or "accepted", flush=True)

try:
    source = Node("source")
    source.call("generate", [12])
    original = WORK / "original.dat"
    source.call("dumptxoutset", [str(original)])
    # Same chain, independently validated by a second node; no shared database.
    peer = Node("independent_exporter")
    for height in range(1, 13):
        time.sleep(0.1)  # keep block-copy RPC traffic below the admission limit
        block_hash = source.call("getblockhash", [height])
        peer.call("submitblock", [source.call("getblock", [block_hash, 0])])
    require(peer.call("getbestblockhash") == source.call("getbestblockhash"), "independent exporter tip")
    time.sleep(2)  # cross the wall-clock second that previously changed file hashes
    repeated, independent = WORK / "repeated.dat", WORK / "independent.dat"
    source.call("dumptxoutset", [str(repeated)])
    peer.call("dumptxoutset", [str(independent)])
    exported = original.read_bytes()
    base = source.call("getblock", [source.call("getbestblockhash")])
    require(struct.unpack_from("<I", exported, 4)[0] == 5, "determinism fixture must be v5")
    require(struct.unpack_from("<Q", exported, 52)[0] == base["time"], "v5 must use base timestamp")
    require(exported == repeated.read_bytes(), "v5 repeated export changed bytes")
    require(exported == independent.read_bytes(), "independent v5 exporter changed bytes")
    peer.stop()
    legacy = Node("legacy_exporter", dormant=True)
    legacy.call("generate", [2])
    legacy_file = WORK / "legacy_export.dat"
    before = int(time.time())
    legacy.call("dumptxoutset", [str(legacy_file)])
    after = int(time.time())
    legacy_data = legacy_file.read_bytes()
    require(struct.unpack_from("<I", legacy_data, 4)[0] == 4, "legacy fixture must be v4")
    require(before <= struct.unpack_from("<Q", legacy_data, 52)[0] <= after, "v4 export time changed")
    legacy.stop()
    print("PASS v5 independent/repeated byte identity and v4 timestamp compatibility", flush=True)
    source.stop()
    shutil.copytree(source.path / "headers", WORK / "unburied_headers")
    source.start(); source.call("generate", [8]); source.stop()
    shutil.copytree(source.path / "headers", WORK / "buried_headers")
    data = original.read_bytes()
    require(hashlib.sha256(data[:-32]).digest() == data[-32:], "writer checksum")
    body = data[:-32]
    shld, anchor, bind, cbsize, branches = offsets(body)
    # Anchor payload changes full SHR1 while keeping tree/UTXO data unchanged.
    payload = bytearray(body); payload[anchor+12] ^= 1
    payload_file = fixture(payload, "payload")
    attempt("payload", payload_file, WORK / "buried_headers", "commitment-mismatch")
    claimed = bytearray(body); claimed[shld+40] ^= 1
    attempt("claimed_root", fixture(claimed, "claimed_root"), WORK / "buried_headers", "does not match snapshot commitment_root")
    coinbase = bytearray(body)
    tag = coinbase.find(b"\x6a\x25DNRS\x01", bind+20, bind+20+cbsize)
    require(tag >= 0, "coinbase must contain canonical DNRS")
    coinbase[tag+7] ^= 1
    attempt("coinbase_commitment", fixture(coinbase,"coinbase"), WORK / "buried_headers", "invalid-merkle-proof")
    branch = bytearray(body)
    if branches:
        branch[bind+20+cbsize] ^= 1
    else:
        struct.pack_into("<I", branch, bind+16, 1); branch.extend(bytes([42])*32)
    attempt("merkle_branch", fixture(branch,"branch"), WORK / "buried_headers", "invalid-merkle-proof")
    base = bytearray(body); base[8] ^= 1
    # Unknown-base validation legitimately precedes checksum parsing.
    unknown = fixture(base,"base")
    attempt("base_hash", unknown, WORK / "buried_headers", "not found in chain")
    source.start()
    fork = source.call("getblockhash", [12]); source.call("invalidateblock", [fork]); source.call("generate", [20]); source.stop()
    shutil.copytree(source.path / "headers", WORK / "fork_headers")
    attempt("ancestry", original, WORK / "fork_headers", "insufficient-burial-or-non-ancestry")
    attempt("unburied", original, WORK / "unburied_headers", "insufficient-burial-or-non-ancestry")
    v4 = bytearray(body[:bind]); struct.pack_into("<I", v4,4,4)
    v4file = fixture(v4,"v4")
    attempt("v4_enforced",v4file,WORK / "buried_headers","Snapshot container v4 is not usable")
    attempt("valid_v5", original, WORK / "buried_headers", verified=True)
    old_timestamp = bytearray(body)
    struct.pack_into("<Q", old_timestamp, 52, int(time.time()))
    attempt("legacy_v5_export_time", fixture(old_timestamp, "legacy_v5_export_time"),
            WORK / "buried_headers", verified=True)
    attempt("v4_dormant",v4file,WORK / "buried_headers",dormant=True)
    attempt("payload_dormant",payload_file,WORK / "buried_headers",dormant=True)
    # A short window after reset is valid when it is the window committed by
    # that base. A count-only publisher gate would incorrectly refuse it.
    reset_args = ["--consensus-shielded-epoch-reset-height=10"]
    reset = Node("reset_exporter", extra=reset_args)
    reset.call("generate", [12])
    reset_file = WORK / "reset.dat"
    reset.call("dumptxoutset", [str(reset_file)])
    reset_body = reset_file.read_bytes()[:-32]
    reset_shld, reset_anchor, _, _, _ = offsets(reset_body)
    anchor_count = struct.unpack_from("<H", reset_body, reset_anchor+6)[0]
    require(anchor_count == 3, f"reset+2 must retain exactly three roots: {anchor_count}")
    reset.call("generate", [8]); reset.stop()
    shutil.copytree(reset.path / "headers", WORK / "reset_headers")
    attempt("valid_short_reset_window", reset_file, WORK / "reset_headers",
            verified=True, extra=reset_args)
    shortened = bytearray(reset_body)
    anchor_bytes = struct.unpack_from("<Q", shortened, reset_shld+16)[0]
    struct.pack_into("<Q", shortened, reset_shld+16, anchor_bytes-36)
    struct.pack_into("<H", shortened, reset_anchor+6, anchor_count-1)
    del shortened[reset_anchor+8:reset_anchor+44]
    attempt("modified_short_reset_window", fixture(shortened, "shortened_reset"),
            WORK / "reset_headers", "commitment-mismatch", extra=reset_args)
    # Exercise canonical bytes with real commitments/nullifiers and a peer
    # that reaches the same state after disconnect/reconnect and restart.
    funded = Node("funded_exporter")
    funded.call("generate", [130])
    for amount in (100, 150):
        funded.call("wallet.shield", [amount])
        funded.call("generate", [2])
    nullifiers = []
    for amount in (50.0, 60.0):
        spend = funded.call("wallet.unshield", {"amount":amount})
        nullifiers.append(spend["nullifier_hex"])
        funded.call("generate", [2])
    require(len(set(nullifiers)) == 2, "must exercise two real nullifiers")
    recovered = Node("reorg_exporter")
    tip = funded.call("getblockcount")
    for height in range(1, tip+1):
        time.sleep(0.1)  # two source RPCs per block; do not flood the limiter
        block_hash = funded.call("getblockhash", [height])
        recovered.call("submitblock", [funded.call("getblock", [block_hash, 0])])
    before_reorg = WORK / "funded.dat"
    funded.call("dumptxoutset", [str(before_reorg)])
    disconnected = funded.call("getblockhash", [131])
    recovered.call("invalidateblock", [disconnected])
    require(recovered.call("getblockcount") == 130, "reorg must disconnect shielded activity")
    recovered.call("reconsiderblock", [disconnected])
    require(recovered.call("getbestblockhash") == funded.call("getbestblockhash"), "reorg restored tip")
    recovered.stop(); recovered.start()
    after_reorg = WORK / "recovered.dat"
    recovered.call("dumptxoutset", [str(after_reorg)])
    require(before_reorg.read_bytes() == after_reorg.read_bytes(),
            "nonempty v5 bytes differ after independent sync/reorg/restart")
    funded.stop(); recovered.stop()
    print("PASS nonempty v5 independent sync/reorg/restart byte identity (two nullifiers)", flush=True)
    result = {"cases":RESULTS, "daemon_sha256":hashlib.sha256(BINARY.read_bytes()).hexdigest()}
    if os.environ.get("GATE_D_REPORT"):
        Path(os.environ["GATE_D_REPORT"]).write_text(json.dumps(result,indent=2)+"\n")
    SUCCESS = True
    print("SUCCESS: Gate D snapshot binding mutations and controls", flush=True)
except Exception:
    print("FAIL: retained Gate D evidence at", WORK, flush=True)
    raise
finally:
    for node in NODES:
        node.stop()
    # Keep test artifacts on failure; successful runs can opt in to retention.
    if SUCCESS and not os.environ.get("GATE_D_KEEP"):
        shutil.rmtree(WORK)
