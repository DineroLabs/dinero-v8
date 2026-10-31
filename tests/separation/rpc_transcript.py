#!/usr/bin/env python3
"""W5 regtest RPC transcript (spec sections 4, 8 W5 row, 9).

usage: rpc_transcript.py record --dinerod PATH --config off|on --out transcripts/<cfg>.json
       rpc_transcript.py check  --dinerod PATH --config off|on --out transcripts/<cfg>.json
                                [--actual PATH]

Starts stock `dinerod --regtest` in fresh temp datadirs on free loopback ports,
mines, and runs the W5 list in order. Every recorded call stores the HTTP
status and the whole JSON-RPC body (`jsonrpc`, `id`, `error`, `result`), so
handler-level errors (`result.error`) and JSON-RPC errors (`error.code`,
`error.message`) are both frozen. `record` writes the normalized transcript;
`check` re-runs it and compares field for field (`json.dumps(sort_keys=True)`
equality, so names, types -- 0 vs 0.0, int vs bool -- values, error codes and
messages must all match). Exit 0 PASS, 1 transcript differs, 2 run/input error.

Transcript steps (in order; every entry carries its step name):
  node A (wallet restored from a fixed mnemonic so keys, scripts and txids are
  deterministic; 1 coinbase to the wallet, maturity blocks to a foreign address):
    getnewaddress, getbalance, listunspent [0,9999999] (before the send),
    getnewaddress (recipient), sendtoaddress, listunspent [0,9999999] and
    listunspent [] (after the send, before mining: the coin spent by the
    mempool tx must be gone -- the mempool-spent filter), wallet.getsyncstatus,
    fee bump/RBF: rbf.config, rbf.status, rbf.estimate, and
    wallet.gettxbumprecommendation / wallet.gettxinclusion on the mempool tx
    (the RbfCpfpDetector RPCs; dinerod does not register them at baseline --
    RegisterFeeBumpRpcHandlers has no caller -- so their "Method not found"
    is what is frozen; dinerod has no replacement-building RPC),
    errors: sendtoaddress to an invalid address, sendtoaddress while the only
    coin is spent in the mempool, gettransaction of an unknown txid;
    then one block: getbalance, listunspent, error sendtoaddress over the
    balance (insufficient funds), wallet.rescanblockchain [0], wallet.rescan,
    getbalance, gettransaction of the sent tx.
  snapshot-recovery rescan (phase_snapshot): dumptxoutset on source S,
    loadtxoutset on consumer C, getbalance, wallet.recovertransparentfromsnapshot
    refused while history is missing, then (S serving bodies over loopback P2P)
    the recovery receipt, getbalance and listunspent.
  P2MR (phase_p2mr, node P, a created + encrypted wallet): wallet.signp2mr on
    the locked wallet (error), wallet.getnewp2mraddress (address encode),
    wallet.importp2mrseed of a fixed seed (deterministic pubkey/address),
    wallet.listp2mraddresses, wallet.signp2mr (v7rpc::SignP2MR), signp2mr with
    a malformed sighash (error), sendtoaddress to the P2MR address (address
    decode to its 0x53 0x20 scriptPubKey), wallet.createrawtransaction and
    wallet.signrawtransaction spending that P2MR output (WalletKeyProvider ->
    SignP2MR), sendrawtransaction.
  ON builds only: the marker ORCHARD_ITEM (owner decision: stock regtest
    cannot activate Orchard and the Orchard harness ctests are on the
    never-run floor, so the Orchard mempool entry seen by RBF detection is
    covered by the named Task 5.3 unit tests, not by this transcript).

Synchronisation (not recorded): before every node-A call the node is polled
until getblockcount == blocks.synced == wallet_scan.height ==
wallet_scan.chain_height (wallet.getsyncstatus). Snapshot-phase calls are made
after their explicit waits (snapshot activation, recovery success); the
recovery polls themselves are not recorded (their count depends on P2P timing).

Normalization rules (applied to params and responses; nothing else is masked):
  N1 a string that is exactly 64 hex chars          -> "<H#n>"
  N2 a regtest/mainnet/testnet/bitcoin-regtest bech32 address token
     (hrp din|rdin|tdin|bcrt, separator 1, bech32 charset), whole value or
     token inside a string                          -> "<A#n>"
  N3 64-hex tokens inside a longer string (bounded by non-hex) -> "<H#n>"
  N4 value of a time field (key in TIME_KEYS)       -> "<T:int>" / "<T:float>" /
     "<T:str>" (the JSON type is kept in the tag, so a type change still fails)
  N5 numeric value of a duration field (DURATION_KEYS) -> "<D:int>" / "<D:float>"
  N6 the run's temp work directory inside any string -> "<WORK>"
  N7 an integer equal to one of this run's ports under a key in PORT_KEYS, or
     ":<port>" inside a string                      -> "<PORT:int>" / ":<PORT>"
  N8 an integer equal to one of this run's dinerod PIDs under a key in
     PID_KEYS                                       -> "<PID:int>"
  (N7/N8 are keyed so an unrelated integer -- a byte count, an amount -- that
  happens to equal a random port or PID is never masked.)
  N9 a string that is entirely hex of even length > 128 chars (ML-DSA-65
     pubkeys, signatures, raw transactions) -> "<X#n:len=L>", numbered by first
     appearance like <H#n>, so equal blobs stay equal and the length is kept.
     It exists because ML-DSA signing is hedged (signature_hex differs every
     run for the same key and message) and createhd keys are random.
  N10 an object KEY that is itself a whole address or 64-hex id (e.g. the
     recipient map of createrawtransaction) -> "<A#n>"/"<H#n>"; every other key
     is a field name and is never rewritten. Field-name keys are walked sorted,
     value keys after them in input order.
  At baseline N1-N6, N9 and N10 fire; N7/N8 are defensive.
  <H#n>/<A#n>/<X#n> are numbered by first appearance across the whole transcript in
  a depth-first walk with object keys sorted, so a txid returned by
  sendtoaddress must reappear as the same <H#n> later.
"""
import argparse
import base64
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

MNEMONIC = ("abandon abandon abandon abandon abandon abandon abandon abandon "
            "abandon abandon abandon about")
# Second fixed mnemonic: owns the foreign address the maturity blocks pay to.
FOREIGN_MNEMONIC = ("legal winner thank year wave sausage worth useful legal "
                    "winner thank yellow")
UNKNOWN_TXID = "00" * 31 + "01"
AVOID_PORTS = {20998, 20999, 34100, 34997, 34998, 50051}

TIME_KEYS = {"time", "blocktime", "timereceived", "timestamp", "mediantime",
             "first_seen", "entry_time", "time_in_mempool", "received_time",
             "last_progress_time", "created_at"}
PORT_KEYS = {"port", "rpcport", "p2pport", "rpc_port", "p2p_port", "wallet_socket_port"}
PID_KEYS = {"pid", "process_id"}
DURATION_KEYS = {"duration", "duration_ms", "elapsed", "elapsed_ms", "eta"}

ORCHARD_ITEM = ("COVERED_BY_UNIT_TEST orchard_rbf_entry "
                "Task5.3:RbfDetectorOrchardEntryThrowsSameError,"
                "WalletMempoolEntryOrchardVinThrowsSameError "
                "(stock regtest cannot activate Orchard; harness ctests are on "
                "the never-run floor)")

HEX64 = re.compile(r"[0-9a-fA-F]{64}")
HEX64_TOKEN = re.compile(r"(?<![0-9a-fA-F])[0-9a-fA-F]{64}(?![0-9a-fA-F])")
HEX_BLOB = re.compile(r"(?:[0-9a-fA-F]{2}){65,}")
ADDR = re.compile(r"(?<![0-9a-z])(?:rdin|tdin|din|bcrt)1[02-9ac-hj-np-z]{6,}(?![0-9a-z])")


LOG_TAIL = 60


class RunError(Exception):
    pass


def free_port(taken):
    for _ in range(200):
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
        s.close()
        if port not in taken and port not in AVOID_PORTS and port + 1 not in taken:
            taken.add(port)
            return port
    raise RunError("no free port")


class Node:
    def __init__(self, dinerod, work, name, taken, log_dir, pids):
        self.dinerod = dinerod
        self.datadir = os.path.join(work, name)
        self.name = name
        self.rpcport = free_port(taken)
        self.p2pport = free_port(taken)
        self.wsport = free_port(taken)
        self.log_dir = log_dir
        self.proc = None
        self.pids = pids
        self.starts = 0
        os.makedirs(self.datadir, exist_ok=True)

    def start(self, *extra, offline=True):
        net = ["--p2p.offline=1", "--listen=0"] if offline else [
            "--p2p.offline=0", "--listen=1", "--bind=127.0.0.1", "--dnsseed=0",
            "--fixedseeds=0", "--discover=0"]
        cmd = [self.dinerod, "--regtest", "--datadir=" + self.datadir,
               "--rpcport=%d" % self.rpcport, "--port=%d" % self.p2pport,
               "--wallet-socket-port=%d" % self.wsport, "--listenonion=0",
               *net, *extra]
        self.starts += 1
        self.out_log = os.path.join(self.log_dir, "%s.%d.log" % (self.name, self.starts))
        log = open(self.out_log, "wb")
        self.proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL)
        log.close()
        self.pids.append(self.proc.pid)
        deadline = time.time() + 120
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise RunError("%s exited rc=%s during startup\n%s" % (
                    self.name, self.proc.returncode, self.startup_logs()))
            try:
                st, body = self.rpc("getblockcount", [])
                if st == 200 and isinstance(body.get("result"), int):
                    return
            except (OSError, RunError):
                pass
            time.sleep(0.5)
        raise RunError("%s did not become ready" % self.name)

    def startup_logs(self, n=LOG_TAIL):
        """The last n lines of dinerod's log for this start, with its path, so
        a CI log shows why the node exited (the work dir does not outlive the
        runner). dinerod logs to stdout/stderr (debug.log_file is empty by
        default), captured in out_log; a dinero.log under the datadir is
        included too."""
        logs = [("log (stdout/stderr)", self.out_log)]
        for root, _dirs, files in sorted(os.walk(self.datadir)):
            if "dinero.log" in files:
                logs.append(("dinero.log", os.path.join(root, "dinero.log")))
        parts = []
        for what, path in logs:
            try:
                with open(path, "rb") as f:
                    lines = f.read().decode(errors="replace").splitlines()
            except OSError as e:
                parts.append("%s %s: unreadable (%s)" % (self.name, what, e))
                continue
            parts.append("%s %s %s (last %d of %d lines):" % (
                self.name, what, path, min(n, len(lines)), len(lines)))
            parts.extend("  | " + l for l in lines[-n:])
        return "\n".join(parts)

    def cookie(self):
        for p in (os.path.join(self.datadir, ".cookie"),
                  os.path.join(self.datadir, "regtest", ".cookie")):
            if os.path.exists(p):
                with open(p) as f:
                    return f.read().strip()
        raise RunError("no cookie for %s" % self.name)

    def rpc(self, method, params):
        data = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method,
                           "params": params}).encode()
        req = urllib.request.Request("http://127.0.0.1:%d/" % self.rpcport, data=data)
        req.add_header("Content-Type", "application/json")
        req.add_header("Authorization", "Basic " + base64.b64encode(self.cookie().encode()).decode())
        try:
            with urllib.request.urlopen(req, timeout=300) as r:
                status, raw = r.status, r.read()
        except urllib.error.HTTPError as e:
            status, raw = e.code, e.read()
        try:
            body = json.loads(raw.decode())
        except ValueError:
            raise RunError("%s %s: non-JSON HTTP %d body %r" % (self.name, method, status, raw[:200]))
        return status, body

    def ok(self, method, params):
        st, body = self.rpc(method, params)
        res = body.get("result")
        if st != 200 or body.get("error") or (isinstance(res, dict) and res.get("error")):
            raise RunError("%s %s %s failed: HTTP %d %s" % (self.name, method,
                           json.dumps(params), st, json.dumps(body)[:600]))
        return res

    def wait_synced(self, timeout=120):
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            height = self.ok("getblockcount", [])
            s = self.ok("wallet.getsyncstatus", [])
            scan = s.get("wallet_scan", {})
            last = (height, scan)
            if (scan.get("height") == height and scan.get("chain_height") == height
                    and s.get("blocks", {}).get("synced") == height):
                return
            time.sleep(0.25)
        raise RunError("%s wallet never caught up: %r" % (self.name, last))

    def stop(self):
        if self.proc is None:
            return
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=60)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=30)
        self.proc = None


def is_value_key(k):
    return bool(HEX64.fullmatch(k) or ADDR.fullmatch(k))


def _tag(v):
    return "int" if isinstance(v, int) else "float" if isinstance(v, float) else "str"


class Normalizer:
    def __init__(self, work, ports, pids):
        self.h, self.a, self.x = {}, {}, {}
        self.work = os.path.realpath(work)
        self.work_raw = work
        self.ports = set(ports)
        self.pids = set(pids)

    def _h(self, v):
        k = v.lower()
        if k not in self.h:
            self.h[k] = "<H#%d>" % (len(self.h) + 1)
        return self.h[k]

    def _a(self, v):
        if v not in self.a:
            self.a[v] = "<A#%d>" % (len(self.a) + 1)
        return self.a[v]

    def _x(self, v):
        k = v.lower()
        if k not in self.x:
            self.x[k] = "<X#%d:len=%d>" % (len(self.x) + 1, len(v))
        return self.x[k]

    def string(self, s):
        if HEX64.fullmatch(s):
            return self._h(s)
        if HEX_BLOB.fullmatch(s):
            return self._x(s)
        for w in sorted({self.work, self.work_raw}, key=len, reverse=True):
            s = s.replace(w, "<WORK>")
        s = ADDR.sub(lambda m: self._a(m.group(0)), s)
        s = HEX64_TOKEN.sub(lambda m: self._h(m.group(0)), s)
        for p in sorted(self.ports, reverse=True):
            s = re.sub(r":%d(?!\d)" % p, ":<PORT>", s)
        return s

    def key(self, k):
        """Object keys are field names and are never rewritten, except a key
        that is itself a whole value: an address or a 64-hex id (N10)."""
        if HEX64.fullmatch(k):
            return self._h(k)
        if ADDR.fullmatch(k):
            return self._a(k)
        return k

    def value(self, v, key=None):
        if isinstance(v, dict):
            names = sorted(k for k in v if not is_value_key(k))
            values = [k for k in v if is_value_key(k)]   # value-keys: input order
            out = {}
            for k in names + values:
                out[self.key(k)] = self.value(v[k], k)
            return out
        if isinstance(v, list):
            return [self.value(x, key) for x in v]
        if isinstance(v, bool) or v is None:
            return v
        if isinstance(v, (str, int, float)):
            if key in TIME_KEYS:
                return "<T:%s>" % _tag(v)
            if key in DURATION_KEYS and not isinstance(v, str):
                return "<D:%s>" % _tag(v)
        if isinstance(v, str):
            return self.string(v)
        if isinstance(v, int):
            if key in PORT_KEYS and v in self.ports:
                return "<PORT:int>"
            if key in PID_KEYS and v in self.pids:
                return "<PID:int>"
        return v


class Transcript:
    def __init__(self):
        self.entries = []

    def call(self, node, step, method, params, sync=True):
        if sync:
            node.wait_synced()
        status, body = node.rpc(method, params)
        self.entries.append({"step": step, "node": node.name, "method": method,
                             "params": params, "http_status": status, "body": body})
        return body

    def marker(self, text):
        self.entries.append({"marker": text})


def result_of(body):
    return body.get("result")


def phase_wallet(t, a, foreign):
    """Node A: the ordinary wallet RPC list."""
    a.ok("wallet.restore", {"name": "w5", "mnemonic": MNEMONIC, "rescan": False})
    a1 = result_of(t.call(a, "getnewaddress", "getnewaddress", []))["address"]
    a.ok("generatetoaddress", [1, a1])
    a.ok("generatetoaddress", [100, foreign])
    t.call(a, "getbalance.funded", "getbalance", [])
    t.call(a, "listunspent.before_send", "listunspent", [0, 9999999])
    a2 = result_of(t.call(a, "getnewaddress.recipient", "getnewaddress", []))["address"]
    sent = result_of(t.call(a, "sendtoaddress", "sendtoaddress", [a2, 1.0]))
    txid = sent.get("txid") if isinstance(sent, dict) else None
    if not isinstance(txid, str) or not HEX64.fullmatch(txid):
        raise RunError("sendtoaddress returned no txid: %r" % sent)
    t.call(a, "listunspent.mempool_spent_filter", "listunspent", [0, 9999999])
    t.call(a, "listunspent.default_after_send", "listunspent", [])
    t.call(a, "wallet.getsyncstatus", "wallet.getsyncstatus", [])
    # Fee bump / RBF surface of dinerod: rbf.* (live mempool policy) and the
    # W.4.4 detector RPCs, which dinerod does not register at baseline
    # (RegisterFeeBumpRpcHandlers has no caller) -- their absence is frozen too.
    t.call(a, "rbf.config", "rbf.config", [])
    t.call(a, "rbf.status", "rbf.status", [])
    t.call(a, "rbf.estimate", "rbf.estimate", [sent.get("fee_paid_una"), 154])
    t.call(a, "rbf.bump_recommendation", "wallet.gettxbumprecommendation", [txid])
    t.call(a, "rbf.inclusion", "wallet.gettxinclusion", [txid])
    t.call(a, "error.invalid_address", "sendtoaddress", ["rdin1notanaddress", 1.0])
    t.call(a, "error.all_coins_spent_in_mempool", "sendtoaddress", [a2, 1.0])
    t.call(a, "error.unknown_txid", "gettransaction", [UNKNOWN_TXID])
    a.ok("generatetoaddress", [1, foreign])
    t.call(a, "getbalance.after_confirm", "getbalance", [])
    t.call(a, "listunspent.after_confirm", "listunspent", [])
    t.call(a, "error.insufficient_funds", "sendtoaddress", [a2, 1000000.0])
    t.call(a, "rescan.rescanblockchain", "wallet.rescanblockchain", [0])
    t.call(a, "rescan.rescan", "wallet.rescan", [])
    t.call(a, "getbalance.after_rescan", "getbalance", [])
    t.call(a, "gettransaction.sent", "gettransaction", [txid])


SNAPSHOT_ARGS = ("--consensus-state-commitment-height=4294967295",)
SNAPSHOT_BASE = 120


def poll(fn, what, timeout=180, interval=0.5):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        ok, last = fn()
        if ok:
            return last
        time.sleep(interval)
    raise RunError("timed out waiting for %s: %s" % (what, json.dumps(last)[:600]))


def phase_snapshot(t, work, dinerod, taken, logs, pids, nodes, foreign):
    """Snapshot-recovery rescan with stock regtest dinerod.

    Source S mines a funded chain and exports a snapshot; consumer C loads it,
    gets the same mnemonic with no local inventory, is refused while the
    pre-snapshot creation bodies are missing, then recovers once S serves them
    over loopback P2P (ChainstateService::RecoverTransparentSnapshotWallet ->
    WalletManager::RescanBlockchainImpl with a prepared history).
    State commitments are held dormant (SNAPSHOT_ARGS) as for every regtest
    export-at-tip-and-reload snapshot flow.
    """
    snap = os.path.join(work, "snapshot.dat")
    src = Node(dinerod, work, "s", taken, logs, pids)
    con = Node(dinerod, work, "c", taken, logs, pids)
    nodes += [src, con]
    src.start(*SNAPSHOT_ARGS)
    src.ok("wallet.restore", {"name": "snapshot-source", "mnemonic": MNEMONIC, "rescan": False})
    src.ok("wallet.importmnemonic", {"mnemonic": MNEMONIC, "rescan": False, "initial_address_count": 4})
    owner = src.ok("wallet.listaddresses", [])[0]["address"]
    src.ok("generatetoaddress", [2, owner])
    src.ok("generatetoaddress", [SNAPSHOT_BASE - 2, foreign])
    t.call(src, "snapshot.dumptxoutset", "dumptxoutset", [snap], sync=False)
    src.stop()
    shutil.copytree(os.path.join(src.datadir, "headers"), os.path.join(con.datadir, "headers"))
    snap_args = SNAPSHOT_ARGS + ("--assumeutxo_snapshot=" + snap, "--assumeutxo_forward_connect=1",
                                 "--assumeutxo_bg_stall_timeout=3600")
    con.start(*snap_args)
    t.call(con, "snapshot.loadtxoutset", "loadtxoutset", [snap], sync=False)

    def active():
        st = con.ok("getsnapshotbootstrapstatus", [])
        b = (st or {}).get("snapshot_bootstrap", {})
        return b.get("assumeutxo_active") is True and b.get("snapshot_base_height") == SNAPSHOT_BASE, st
    poll(active, "snapshot activation")
    con.ok("wallet.restore", {"name": "snapshot-recovered", "mnemonic": MNEMONIC, "rescan": False})
    con.ok("wallet.importmnemonic", {"mnemonic": MNEMONIC, "rescan": False, "initial_address_count": 4})
    con.stop()
    con.start(*snap_args)
    params = {"walletname": "snapshot-recovered", "expected_first_address": owner}
    t.call(con, "getbalance.snapshot_before_recovery", "getbalance", [], sync=False)
    t.call(con, "snapshot_recovery.refused_without_history",
           "wallet.recovertransparentfromsnapshot", params, sync=False)
    con.stop()
    src.start(*SNAPSHOT_ARGS, offline=False)
    con.start(*snap_args, "--connect=127.0.0.1:%d" % src.p2pport, offline=False)

    def recovered():
        st, body = con.rpc("wallet.recovertransparentfromsnapshot", params)
        res = body.get("result")
        return isinstance(res, dict) and res.get("success") is True, body
    poll(recovered, "snapshot recovery")
    # Recovery succeeded (the polls are not recorded: their count depends on
    # P2P timing). The recorded call is the idempotent retry and its results.
    t.call(con, "snapshot_recovery.recovered", "wallet.recovertransparentfromsnapshot",
           params, sync=False)
    t.call(con, "getbalance.snapshot_recovered", "getbalance", [], sync=False)
    t.call(con, "listunspent.snapshot_recovered", "listunspent", [1, 9999999], sync=False)
    con.stop()
    src.stop()


P2MR_PASSWORD = "w5-regtest-password"
P2MR_SIGHASH = "11" * 32
P2MR_SEED = "22" * 32
P2MR_IMPORT_PATH = "m/44'/1448'/0'/0/7"


def phase_p2mr(t, work, dinerod, taken, logs, pids, nodes, foreign):
    """v7 P2MR RPCs (rpc/v7_pq_rpc_json.cpp, rpc/v7_pq_handlers.cpp) on node P.

    Address encode (wallet.getnewp2mraddress), store listing, direct signing
    (wallet.signp2mr -> v7rpc::SignP2MR), address decode (sendtoaddress to the
    P2MR address builds its 0x53 0x20 scriptPubKey), and the wallet signing
    path for a P2MR input (wallet.signrawtransaction -> WalletKeyProvider ->
    SignP2MR), plus locked-wallet and malformed-sighash errors.
    """
    p = Node(dinerod, work, "p", taken, logs, pids)
    nodes.append(p)
    p.start()
    # wallet.createhd, not wallet.restore: only a created wallet carries the
    # initial PQ master owner that unlock loads (wallet_manager.cpp:5053-5090,
    # :5300-5316); a restored wallet answers v7_master_key_not_loaded.
    p.ok("wallet.createhd", ["w5p"])
    p.ok("wallet.encrypt", [P2MR_PASSWORD])
    t.call(p, "p2mr.error.locked", "wallet.signp2mr",
           {"address": "rdin1r0", "sighash_hex": P2MR_SIGHASH})
    p.ok("wallet.unlock", [P2MR_PASSWORD, 3600])
    r = result_of(t.call(p, "p2mr.getnewp2mraddress", "wallet.getnewp2mraddress",
                         {"account": 0, "change": 0, "address_index": 0,
                          "leaf_index": 0, "label": "w5"}))
    r = result_of(t.call(p, "p2mr.importp2mrseed", "wallet.importp2mrseed",
                         {"seed_hex": P2MR_SEED, "derivation_path": P2MR_IMPORT_PATH,
                          "leaf_index": 0, "label": "w5-import"}))
    addr, root = r["address"], r["merkle_root_hex"]
    t.call(p, "p2mr.listp2mraddresses", "wallet.listp2mraddresses", {})
    t.call(p, "p2mr.signp2mr", "wallet.signp2mr", {"address": addr, "sighash_hex": P2MR_SIGHASH})
    t.call(p, "p2mr.error.bad_sighash", "wallet.signp2mr", {"address": addr, "sighash_hex": "abcd"})
    owner = p.ok("getnewaddress", [])["address"]
    p.ok("generatetoaddress", [1, owner])
    p.ok("generatetoaddress", [100, foreign])
    sent = result_of(t.call(p, "p2mr.sendtoaddress", "sendtoaddress", [addr, 30.0]))
    fund = sent.get("txid") if isinstance(sent, dict) else None
    if not isinstance(fund, str):
        raise RunError("P2MR funding failed: %r" % sent)
    p.ok("generatetoaddress", [1, foreign])
    spk = "5320" + root
    tx = p.ok("gettransaction", [fund])
    vout = [o["vout"] for o in tx["outputs"] if o.get("scriptPubKey") == spk]
    if len(vout) != 1:
        raise RunError("P2MR output not found in %s" % json.dumps(tx)[:600])
    recipient = p.ok("getnewaddress", [])["address"]
    raw = result_of(t.call(p, "p2mr.createrawtransaction", "wallet.createrawtransaction",
                           [[{"txid": fund, "vout": vout[0]}], [{recipient: 29.9999}]]))
    prev = [{"txid": fund, "vout": vout[0], "scriptPubKey": spk, "amount": 30.0}]
    signed = result_of(t.call(p, "p2mr.signrawtransaction", "wallet.signrawtransaction",
                              [raw["hex"], prev]))
    t.call(p, "p2mr.sendrawtransaction", "sendrawtransaction", [signed["hex"]])
    p.stop()


def run(dinerod, config, keep=False, phases=("wallet", "snapshot", "p2mr")):
    work = tempfile.mkdtemp(prefix="sep_w5_")
    logs = os.path.join(work, "logs")
    os.makedirs(logs)
    taken = set()
    pids = []
    nodes = []
    t = Transcript()
    try:
        a = Node(dinerod, work, "a", taken, logs, pids)
        nodes.append(a)
        a.start()
        # Foreign payee: a second fixed wallet's first address, created on A
        # and discarded (A then switches to the recorded wallet "w5").
        a.ok("wallet.restore", {"name": "foreign", "mnemonic": FOREIGN_MNEMONIC, "rescan": False})
        foreign = a.ok("getnewaddress", [])["address"]
        if "wallet" in phases:
            phase_wallet(t, a, foreign)
        a.stop()
        if "snapshot" in phases:
            phase_snapshot(t, work, dinerod, taken, logs, pids, nodes, foreign)
        if "p2mr" in phases:
            phase_p2mr(t, work, dinerod, taken, logs, pids, nodes, foreign)
        if config == "on":
            t.marker(ORCHARD_ITEM)
        norm = Normalizer(work, taken, pids)
        out = []
        for e in t.entries:
            out.append(norm.value(e))
        return out
    except Exception:
        keep = True
        with open(os.path.join(work, "raw_entries.json"), "w") as f:
            f.write(dump(t.entries))
        raise
    finally:
        for n in nodes:
            try:
                n.stop()
            except Exception:
                pass
        if keep:
            sys.stderr.write("rpc_transcript: work dir kept at %s\n" % work)
        else:
            shutil.rmtree(work, ignore_errors=True)


def dump(obj):
    return json.dumps(obj, indent=1, sort_keys=True, ensure_ascii=True) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["record", "check"])
    ap.add_argument("--dinerod", required=True)
    ap.add_argument("--config", choices=["off", "on"], required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--actual", help="check: also write the new transcript here")
    ap.add_argument("--keep", action="store_true", help="keep the temp work dir")
    args = ap.parse_args(argv)
    if not os.access(args.dinerod, os.X_OK):
        print("rpc_transcript: dinerod not executable: %s" % args.dinerod, file=sys.stderr)
        return 2
    if args.mode == "check" and not os.path.exists(args.out):
        print("rpc_transcript: no baseline %s" % args.out, file=sys.stderr)
        return 2
    try:
        doc = {"config": args.config, "transcript": run(args.dinerod, args.config, args.keep)}
    except RunError as e:
        print("rpc_transcript: RUN ERROR: %s" % e, file=sys.stderr)
        return 2
    text = dump(doc)
    if args.mode == "record":
        with open(args.out, "w") as f:
            f.write(text)
        print("rpc_transcript: recorded %d entries -> %s" % (len(doc["transcript"]), args.out))
        return 0
    if args.actual:
        with open(args.actual, "w") as f:
            f.write(text)
    with open(args.out) as f:
        base = f.read()
    if base == text:
        print("rpc_transcript: PASS (%d entries match %s)" % (len(doc["transcript"]), args.out))
        return 0
    import difflib
    sys.stdout.writelines(difflib.unified_diff(
        base.splitlines(True), text.splitlines(True), "baseline", "actual", n=4))
    print("rpc_transcript: FAIL transcript differs from %s" % args.out)
    return 1


if __name__ == "__main__":
    sys.exit(main())
