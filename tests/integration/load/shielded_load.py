#!/usr/bin/env python3
"""Integrated-node load harness (docs/superpowers/plans/2026-09-22-shielded-v2-integrated-load-test.md).

Single node under test (NUT) on regtest with the Auth profile forced at low heights. Measures the OLD
design (v1 Auth proofs via wallet.shield / wallet.transfer) today; the NEW design (version-7 bundles)
plugs in as a different transaction builder once the consensus wiring exists.

Scenarios here (single node): steady+service (W1+W4) and hostile (W5). Catch-up (W2) and fork (W3)
need the two-node variant.

Exit codes: 0 = ran and no qualification failure; 2 = qualification failure (an invalid proof was
accepted, or a positive seed was rejected); 1 = harness/transport failure. Evidence is written in
every case.
"""
import argparse, base64, json, os, re, shutil, socket, statistics, subprocess, sys, tempfile, threading, time, urllib.request, urllib.error

class BusyError(Exception): pass          # HTTP 503 "RPC server busy"
class TransportError(Exception): pass     # connection/timeouts/HTTP errors other than 503
class ProtocolError(Exception): pass      # JSON-RPC error envelopes or malformed replies
class QualificationFailure(Exception): pass

def free_ports(n):
    socks, ports = [], []
    for _ in range(n):
        s = socket.socket(); s.bind(("127.0.0.1", 0)); socks.append(s); ports.append(s.getsockname()[1])
    for s in socks: s.close()
    return ports

class Node:
    def __init__(self, dinerod, workdir, extra_flags, listen=False, connect=None):
        self.dinerod, self.dir = dinerod, workdir
        self.rpcport, self.p2pport, self.walletport = free_ports(3)
        self.log_path = os.path.join(workdir, "dinerod.log"); self.proc = None; self.cookie = None; self.extra = extra_flags
        self.listen, self.connect = listen, connect
    def start(self, connect=None):
        os.makedirs(self.dir, exist_ok=True); self.log = open(self.log_path, "ab"); self.cookie = None
        if connect is not None: self.connect = connect
        # Mirror the repository's two-node relay test: every networked node runs --listen=1 and names its
        # peer with --connect; a node with no peer runs offline (--p2p.offline=1, no listeners).
        net = ["--listen=1", f"--connect=127.0.0.1:{self.connect}"] if self.connect else ["--listen=0", "--p2p.offline=1"]
        args = [self.dinerod, "--regtest", f"--datadir={self.dir}", f"--rpcport={self.rpcport}", f"--port={self.p2pport}",
                f"--wallet-socket-port={self.walletport}", "--utreexo=1"] + net + self.extra
        self.proc = subprocess.Popen(args, stdout=self.log, stderr=subprocess.STDOUT)
        t0 = time.time()
        while time.time() - t0 < 120:
            if self.proc.poll() is not None: raise RuntimeError("dinerod exited early; see " + self.log_path)
            for c in (os.path.join(self.dir, ".cookie"), os.path.join(self.dir, "regtest", ".cookie")):
                if os.path.exists(c): self.cookie = open(c, "rb").read().strip()
            if self.cookie:
                try:
                    if self.rpc("getblockcount", [], timeout=5) is not None: return
                except Exception: pass
            time.sleep(0.25)
        raise RuntimeError("rpc never became ready")
    def stop(self):
        if self.proc and self.proc.poll() is None:
            try: self.rpc("stop", [], timeout=10)
            except Exception: pass
            try: self.proc.wait(timeout=120)
            except subprocess.TimeoutExpired: self.proc.terminate(); self.proc.wait(timeout=60)
    def rpc(self, method, params, timeout=60):
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params}).encode()
        req = urllib.request.Request(f"http://127.0.0.1:{self.rpcport}/", body,
                                     {"Content-Type": "application/json", "Authorization": "Basic " + base64.b64encode(self.cookie).decode()})
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r: raw = r.read()
        except urllib.error.HTTPError as e:
            if e.code == 503: raise BusyError()
            raise TransportError(f"http {e.code}")
        except (urllib.error.URLError, socket.timeout, ConnectionError, OSError) as e:
            raise TransportError(str(e)[:120])
        try: reply = json.loads(raw)
        except Exception: raise ProtocolError("malformed json")
        if not isinstance(reply, dict): raise ProtocolError("reply not an object")
        if reply.get("error"): raise ProtocolError(f"{method}: {reply['error']}")
        res = reply.get("result")
        if isinstance(res, dict) and res.get("error"): raise ProtocolError(f"{method}: {res['error']} {res.get('error_message', '')}")
        return res
    def cpu_seconds(self):
        # cumulative CPU time of the daemon (user+sys), from ps; robust on macOS and Linux
        out = subprocess.run(["ps", "-o", "time=", "-p", str(self.proc.pid)], capture_output=True, text=True).stdout.strip()
        parts = out.replace("-", ":").split(":")
        try:
            secs = 0.0
            for p in parts: secs = secs * 60 + float(p)
            return secs
        except ValueError: return None
    def rss_mb(self):
        out = subprocess.run(["ps", "-o", "rss=", "-p", str(self.proc.pid)], capture_output=True, text=True).stdout.strip()
        return int(out) // 1024 if out.isdigit() else None

def pct(xs, p):
    if not xs: return None
    xs = sorted(xs); k = max(0, min(len(xs) - 1, round(p / 100 * (len(xs) - 1)))); return xs[k]
def summarize(xs):
    xs = [x for x in xs if x is not None]
    return {"n": len(xs), "p50": pct(xs, 50), "p95": pct(xs, 95), "p99": pct(xs, 99), "max": max(xs) if xs else None, "mean": statistics.fmean(xs) if xs else None}

class EndpointProbe(threading.Thread):
    """One endpoint, one bounded in-flight call, fixed schedule. Every tick is recorded: a sample
    (scheduled/start/end/ms/ok/err) or a missed deadline when the previous call overran."""
    def __init__(self, node, name, method, params, interval, timeout, bracket_tip=False):
        super().__init__(daemon=True); self.node, self.name, self.method, self.params = node, name, method, params
        self.interval, self.timeout, self.bracket_tip = interval, timeout, bracket_tip
        self.samples, self.missed, self.stop_flag = [], 0, False
    def run(self):
        t_sched = time.monotonic()
        while not self.stop_flag:
            rec = {"sched": t_sched, "start": time.monotonic()}
            try:
                if self.bracket_tip: before = self.node.rpc("getbestblockhash", [], 10)
                res = self.node.rpc(self.method, self.params, self.timeout); rec["ok"] = True
                if self.bracket_tip:
                    after = self.node.rpc("getbestblockhash", [], 10)
                    rec["tip_before"], rec["tip_after"] = before, after
                    rec["template_prev"] = res.get("previousblockhash") if isinstance(res, dict) else None
            except BusyError: rec["ok"] = False; rec["err"] = "busy503"
            except TransportError as e: rec["ok"] = False; rec["err"] = "transport:" + str(e)
            except ProtocolError as e: rec["ok"] = False; rec["err"] = "protocol:" + str(e)[:80]
            rec["end"] = time.monotonic(); rec["ms"] = (rec["end"] - rec["start"]) * 1000
            self.samples.append(rec)
            t_sched += self.interval
            overrun = int(max(0, (time.monotonic() - t_sched) // self.interval))
            if overrun: self.missed += overrun; t_sched += overrun * self.interval
            time.sleep(max(0, t_sched - time.monotonic()))
    def report(self):
        ok = [s["ms"] for s in self.samples if s.get("ok")]
        errs = {}
        for s in self.samples:
            if not s.get("ok"): errs[s["err"].split(":")[0]] = errs.get(s["err"].split(":")[0], 0) + 1
        rep = {"method": self.method, "interval_s": self.interval, "samples": len(self.samples), "ok": len(ok), "errors": errs,
               "missed_deadlines": self.missed, "latency_ms": summarize(ok), "slow_over_1s": sum(1 for m in ok if m > 1000)}
        if self.bracket_tip:
            stale = sum(1 for s in self.samples if s.get("ok") and s.get("tip_before") and s["tip_before"] == s["tip_after"] and s["template_prev"] != s["tip_before"])
            rep["stale_template_with_stable_tip"] = stale
        return rep

class ResourceSampler(threading.Thread):
    def __init__(self, node, interval=1.0):
        super().__init__(daemon=True); self.node, self.interval, self.stop_flag, self.samples = node, interval, False, []
    def run(self):
        last_cpu, last_t = self.node.cpu_seconds(), time.monotonic()
        while not self.stop_flag:
            time.sleep(self.interval)
            cpu, t = self.node.cpu_seconds(), time.monotonic()
            util = ((cpu - last_cpu) / (t - last_t) * 100) if (cpu is not None and last_cpu is not None and t > last_t) else None
            self.samples.append({"t": t, "cpu_pct_of_one_core": util, "rss_mb": self.node.rss_mb()}); last_cpu, last_t = cpu, t
    def report(self):
        return {"samples": len(self.samples), "cpu_pct_of_one_core": summarize([s["cpu_pct_of_one_core"] for s in self.samples]), "rss_mb": summarize([s["rss_mb"] for s in self.samples])}

def start_probes(node, miner):
    probes = [EndpointProbe(node, "getblockcount", "getblockcount", [], 1.0, 30), EndpointProbe(node, "getrawmempool", "getrawmempool", [], 1.0, 30),
              EndpointProbe(node, "wallet.shieldedbalance", "wallet.shieldedbalance", [], 1.0, 60),
              EndpointProbe(node, "getblocktemplate", "getblocktemplate", [{"address": miner}], 2.0, 60, bracket_tip=True)]
    res = ResourceSampler(node)
    for p in probes: p.start()
    res.start(); return probes, res

def stop_probes(probes, res):
    for p in probes: p.stop_flag = True
    res.stop_flag = True
    for p in probes: p.join(timeout=90)
    res.join(timeout=10)
    return {p.name: p.report() for p in probes}, res.report(), {p.name: p.samples for p in probes}, res.samples

def failure_counters(path):
    txt = open(path, "rb").read().decode("utf-8", "replace")
    return {k: len(re.findall(k, txt)) for k in ("SAFE MODE", "INVARIANT VIOLATION", "corrupt", "REORG ABORT", "RPC server busy")}

def tx_hex(node, txid):
    r = node.rpc("getrawtransaction", [txid], 30)   # {hex} while the tx is in the mempool
    return r["hex"] if isinstance(r, dict) and isinstance(r.get("hex"), str) and len(r["hex"]) > 200 else None

def parse_accept(res):
    """Normalise a testmempoolaccept reply. Returns (verdict, reason): verdict in {accepted, rejected,
    malformed}. Accepts the documented list-of-objects form and a bare object; anything else is malformed."""
    items = res if isinstance(res, list) else [res] if isinstance(res, dict) else None
    if not items or len(items) != 1 or not isinstance(items[0], dict) or "allowed" not in items[0]: return "malformed", None
    it = items[0]
    if it["allowed"] is True: return "accepted", None
    if it["allowed"] is False:
        reason = it.get("reject-reason") or it.get("reject_reason") or it.get("reason") or it.get("error") or "unspecified"
        return "rejected", str(reason)[:120]
    return "malformed", None

def read_varint(b, pos):
    f = b[pos]
    if f < 253: return f, pos + 1
    if f == 253: return int.from_bytes(b[pos+1:pos+3], "little"), pos + 3
    if f == 254: return int.from_bytes(b[pos+1:pos+5], "little"), pos + 5
    return int.from_bytes(b[pos+1:pos+9], "little"), pos + 9

def bundle_fields(hexstr):
    """Byte windows of every field of the v1 shielded bundle inside a transaction, found by locating the
    bundle bytes (the last varint-prefixed blob before locktime). Returns [(name, start, end)] in tx byte
    offsets, or None if the layout cannot be parsed."""
    tx = bytes.fromhex(hexstr)
    # The bundle is serialised as varint(len) || bundle immediately before the 4-byte locktime.
    # Search backwards for a varint whose length lands exactly on len(tx) - 4.
    end = len(tx) - 4
    start = None
    # The bundle is the varint-prefixed blob that ends exactly at the locktime; scan FORWARD over the
    # short transparent header (version, marker/flag, vin/vout counts, fee flag + fee) so a coincidental
    # byte pattern near the tail can never be mistaken for the prefix.
    for cand in range(4, min(len(tx) - 1, 256)):
        try: n, p = read_varint(tx, cand)
        except IndexError: continue
        if p + n == end and n > 100: start = p; break
    if start is None: return None
    f = []; pos = start
    try:
        return _bundle_fields_from(tx, start, end, f)
    except (IndexError, ValueError):
        return None

def _bundle_fields_from(tx, start, end, f):
    pos = start
    f.append(("value_balance", pos, pos + 8)); pos += 8
    n_sp, pos = read_varint(tx, pos)
    for i in range(n_sp):
        f.append((f"spend{i}_nullifier", pos, pos + 32)); pos += 32
        f.append((f"spend{i}_anchor", pos, pos + 32)); pos += 32
        f.append((f"spend{i}_cv", pos, pos + 33)); pos += 33
        n, pos = read_varint(tx, pos); f.append((f"spend{i}_zkproof", pos, pos + n)); pos += n
    n_out, pos = read_varint(tx, pos)
    for j in range(n_out):
        f.append((f"out{j}_commitment", pos, pos + 32)); pos += 32
        f.append((f"out{j}_cv", pos, pos + 33)); pos += 33
        n, pos = read_varint(tx, pos); f.append((f"out{j}_encrypted_note", pos, pos + n)); pos += n
        n, pos = read_varint(tx, pos); f.append((f"out{j}_zkproof", pos, pos + n)); pos += n
    n, pos = read_varint(tx, pos); f.append(("range_proof_container", pos, pos + n)); pos += n
    f.append(("bvk_commitment", pos, pos + 33)); pos += 33
    f.append(("binding_sig", pos, pos + 64)); pos += 64
    if pos != end: return None
    return f

def field_at(fields, byte):
    for name, a, e in fields or []:
        if a <= byte < e: return name
    return "outside_bundle"

def mutate_in(hexstr, windows, i):
    """Flip one nibble at a position chosen (distinct per i) inside the union of the given byte windows."""
    total = sum(e - a for a, e in windows)
    k = (i * 7919) % total
    for a, e in windows:
        if k < e - a: byte = a + k; break
        k -= e - a
    pos = byte * 2 + (i & 1)
    return hexstr[:pos] + format((int(hexstr[pos], 16) ^ (1 + i % 15)) & 0xF, "x") + hexstr[pos + 1:], byte

def mutate(hexstr, i):
    # v1 bundles end with range proof || bvk || binding signature (cheap checks that run BEFORE the Spartan
    # proofs); the proofs occupy most of the transaction. Flip one nibble at a position spread over the
    # middle 80% of the hex, distinct per i. The node's reject reason, not the elapsed time, names the stage.
    span = len(hexstr) * 8 // 10; pos = len(hexstr) // 10 + (i * 7919) % span
    return hexstr[:pos] + format((int(hexstr[pos], 16) ^ (1 + i % 15)) & 0xF, "x") + hexstr[pos + 1:]

def scenario_steady(node, seconds, out):
    """W1+W4 at the old design's own generation rate: the wallet builds shielded txs back to back
    (offered rate = achieved rate, both recorded); a block is mined when the mempool is non-empty and
    >= 15 s passed (legal budget: <= 8 shielded proofs per block); block contents are read back."""
    miner = node.rpc("wallet.getnewaddress", ["taproot", "loadgen"], 30); miner = miner["address"] if isinstance(miner, dict) else miner
    node.rpc("generatetoaddress", [140, miner], 600)
    start_height = node.rpc("getblockcount", [])
    recipient = node.rpc("wallet.getshieldedaddress", {"account": 1, "j": 0}, 60)["address"]
    probes, res = start_probes(node, miner)
    builds, blocks, submitted = [], [], {}
    t_start = time.monotonic(); t_end = t_start + seconds; last_block = time.monotonic(); confirmed_shields = 0
    while time.monotonic() < t_end:
        kind = "shield" if (confirmed_shields < 2 or len(builds) % 3 == 0) else "transfer"
        t0 = time.monotonic()
        try:
            r = node.rpc("wallet.shield", {"amount_una": 100_000_000}, 600) if kind == "shield" else node.rpc("wallet.transfer", {"amount_una": 40_000_000, "address": recipient}, 900)
            builds.append({"t": t0 - t_start, "kind": kind, "ms": (time.monotonic() - t0) * 1000, "vsize": r.get("vsize"), "txid": r.get("txid")})
            if r.get("txid"): submitted[r["txid"]] = kind
        except (BusyError, TransportError, ProtocolError) as e:
            builds.append({"t": t0 - t_start, "kind": kind, "ms": (time.monotonic() - t0) * 1000, "error": type(e).__name__ + ":" + str(e)[:120]}); time.sleep(1)
        mem = node.rpc("getrawmempool", [], 30)
        if mem and time.monotonic() - last_block >= 15:
            t0 = time.monotonic(); gen = node.rpc("generatetoaddress", [1, miner], 600); gen_ms = (time.monotonic() - t0) * 1000
            hashes = gen if isinstance(gen, list) else next((v for v in (gen or {}).values() if isinstance(v, list)), []) if isinstance(gen, dict) else []
            if not hashes: hashes = [node.rpc("getbestblockhash", [], 10)]   # exactly one block was mined
            blk = node.rpc("getblock", [hashes[0], 1], 60)
            if not isinstance(blk, dict): blk = {}
            txs = blk.get("tx", []) if isinstance(blk, dict) else []
            included = [t for t in txs if t in submitted]
            blocks.append({"height": blk.get("height"), "hash": hashes[0] if hashes else None, "generate_ms": gen_ms, "mempool_before": len(mem), "tx_count": len(txs),
                           "shielded_included": len(included), "included_kinds": {k: sum(1 for t in included if submitted[t] == k) for k in ("shield", "transfer")}})
            confirmed_shields += sum(1 for t in included if submitted[t] == "shield"); last_block = time.monotonic()
    if node.rpc("getrawmempool", [], 30): node.rpc("generatetoaddress", [1, miner], 600)
    reports, resrep, raw_probe, raw_res = stop_probes(probes, res)
    okb = [b for b in builds if "error" not in b]
    result = {"scenario": "steady+service", "seconds": seconds, "start_height": start_height, "end_height": node.rpc("getblockcount", []),
              "generation": {"offered_equals_achieved": True, "achieved_tx_per_s": len(okb) / seconds, "builds_ok": len(okb), "builds_failed": [b for b in builds if "error" in b][:10],
                             "shield_ms": summarize([b["ms"] for b in okb if b["kind"] == "shield"]), "transfer_ms": summarize([b["ms"] for b in okb if b["kind"] == "transfer"])},
              "blocks": {"count": len(blocks), "generate_ms": summarize([b["generate_ms"] for b in blocks]), "shielded_txs_confirmed": sum(b["shielded_included"] for b in blocks), "per_block": blocks},
              "probes": reports, "resources": resrep, "log_counters": failure_counters(node.log_path), "miner": miner}
    json.dump(result, open(os.path.join(out, "steady.json"), "w"), indent=2)
    json.dump({"probes": raw_probe, "resources": raw_res, "builds": builds}, open(os.path.join(out, "steady.raw.json"), "w"))
    return result

def build_seed(node, kind, miner, recipient):
    """A fresh, unmined seed transaction whose hex is captured while in the mempool, then removed from the
    mempool so mutated copies pass the cheap checks (inputs unspent, nullifier fresh) and only the proof can fail."""
    r = node.rpc("wallet.shield", {"amount_una": 100_000_000}, 600) if kind == "shield" else node.rpc("wallet.transfer", {"amount_una": 40_000_000, "address": recipient}, 900)
    h = tx_hex(node, r["txid"])
    if not h: raise QualificationFailure(f"could not capture {kind} seed hex")
    node.rpc("mempool.clear", [], 60)
    if node.rpc("getrawmempool", [], 30): raise QualificationFailure("mempool.clear left transactions")
    verdict, reason = parse_accept(node.rpc("testmempoolaccept", [h], 300))
    if verdict != "accepted": raise QualificationFailure(f"positive {kind} seed not re-accepted: {verdict} {reason}")
    return h

def scenario_hostile(node, seconds, out, miner=None, recipient=None):
    """W5: mutated-proof copies of unmined seeds (an output-only shield: 1 proof; a 1-in-2-out transfer:
    3 proofs) alternately through testmempoolaccept, one client, as fast as the node answers. Every reply is
    schema-checked; any acceptance is a qualification failure; transport/protocol errors are counted, never
    timed as decisions; the node's reject reason is the stage signal."""
    miner = miner or (lambda a: a["address"] if isinstance(a, dict) else a)(node.rpc("wallet.getnewaddress", ["taproot", "hostile"], 30))
    recipient = recipient or node.rpc("wallet.getshieldedaddress", {"account": 1, "j": 0}, 60)["address"]
    # Fund first (a confirmed shielded note for the transfer seed), then build BOTH seeds with nothing
    # mined afterwards, so no seed's transparent input or note can be spent by later funding activity.
    node.rpc("wallet.shield", {"amount_una": 100_000_000}, 600); node.rpc("generatetoaddress", [1, miner], 600)
    seeds = {}
    try: seeds["transfer_3proofs"] = build_seed(node, "transfer", miner, recipient)
    except QualificationFailure: raise
    except Exception as e: seeds["transfer_3proofs_error"] = str(e)[:160]
    seeds["shield_1proof"] = build_seed(node, "shield", miner, recipient)
    json.dump({"seeds_hex": {k: v for k, v in seeds.items() if not k.endswith("_error")}}, open(os.path.join(out, "hostile.seeds.json"), "w"))
    layouts = {k: bundle_fields(v) for k, v in seeds.items() if not k.endswith("_error")}
    for k, lay in layouts.items():
        if lay is None: raise QualificationFailure(f"could not decode the bundle layout of the {k} seed")
    json.dump({"seeds_hex": {k: v for k, v in seeds.items() if not k.endswith("_error")}, "layouts": layouts}, open(os.path.join(out, "hostile.seeds.json"), "w"))
    # Lanes: "proof" mutates only inside zk-proof windows (the workload under test); "ciphertext" mutates
    # only inside encrypted-note windows (a control: no consensus check covers them, acceptance is expected).
    lanes = {}
    for k, lay in layouts.items():
        lanes[k + "/proof"] = (seeds[k], [(a, e) for n, a, e in lay if n.endswith("_zkproof")])
        lanes[k + "/ciphertext_control"] = (seeds[k], [(a, e) for n, a, e in lay if n.endswith("_encrypted_note")])
    probes, res = start_probes(node, miner)
    counts = {k: {"decisions": 0, "accepted": 0, "rejected": 0, "malformed": 0, "busy503": 0, "transport": 0, "protocol": 0} for k in lanes}
    reasons = {k: {} for k in counts}; decisions = {k: [] for k in counts}; accepted_examples = []
    # proof lanes get 4 of every 5 submissions; the ciphertext control lane 1 of 5
    schedule = [k for k in lanes if k.endswith("/proof")] * 4 + [k for k in lanes if k.endswith("/ciphertext_control")]
    t_end = time.monotonic() + seconds; i = 0
    while time.monotonic() < t_end:
        kind = schedule[i % len(schedule)]; seed_hex, windows = lanes[kind]
        h, byte = mutate_in(seed_hex, windows, i); fld = field_at(layouts[kind.split("/")[0]], byte); i += 1; c = counts[kind]
        t0 = time.monotonic()
        try: raw = node.rpc("testmempoolaccept", [h], 300)
        except BusyError: c["busy503"] += 1; continue
        except TransportError: c["transport"] += 1; continue
        except ProtocolError as e:
            # a JSON-RPC error envelope carrying a reject reason is a decision, not a harness fault
            c["protocol"] += 1; continue
        dt = (time.monotonic() - t0) * 1000
        verdict, reason = parse_accept(raw)
        if verdict == "malformed": c["malformed"] += 1; continue
        c["decisions"] += 1; decisions[kind].append({"i": i - 1, "byte": byte, "field": fld, "ms": dt, "reason": reason, "verdict": verdict})
        if verdict == "accepted":
            c["accepted"] += 1; accepted_examples.append({"i": i - 1, "kind": kind, "byte": byte, "field": fld, "reply": raw})
        else: c["rejected"] += 1; reasons[kind][reason] = reasons[kind].get(reason, 0) + 1
    reports, resrep, raw_probe, raw_res = stop_probes(probes, res)
    result = {"scenario": "hostile", "seconds": seconds, "seeds": {k: (len(v) // 2 if isinstance(v, str) else v) for k, v in seeds.items()}, "counts": counts, "reject_reasons": reasons,
              "decision_ms_by_reason": {k: {r: summarize([d["ms"] for d in decisions[k] if d["reason"] == r]) for r in reasons[k]} for k in counts},
              "decision_ms_all": {k: summarize([d["ms"] for d in decisions[k]]) for k in counts}, "accepted_examples": accepted_examples[:5],
              "probes": reports, "resources": resrep, "log_counters": failure_counters(node.log_path),
              "accepted_by_field": {k: {f: sum(1 for d in decisions[k] if d["verdict"] == "accepted" and d["field"] == f) for f in {d["field"] for d in decisions[k]}} for k in counts},
              "qualification_failed": any(c["accepted"] > 0 for k, c in counts.items() if not k.endswith("ciphertext_control"))}
    json.dump(result, open(os.path.join(out, "hostile.json"), "w"), indent=2)
    json.dump({"probes": raw_probe, "resources": raw_res, "decisions": decisions}, open(os.path.join(out, "hostile.raw.json"), "w"))
    return result

class HeightTracker(threading.Thread):
    """Samples getblockcount on a node every `interval` s; records every height change with a timestamp."""
    def __init__(self, node, interval=0.25):
        super().__init__(daemon=True); self.node, self.interval, self.stop_flag, self.changes, self.errors = node, interval, False, [], 0
    def run(self):
        last = None
        while not self.stop_flag:
            try:
                h = self.node.rpc("getblockcount", [], 10)
                if h != last: self.changes.append({"t": time.monotonic(), "height": h}); last = h
            except Exception: self.errors += 1
            time.sleep(self.interval)

def produce_block_with_proofs(a, miner, recipient, kinds):
    """On node A: build the given shielded txs (kinds in {shield, transfer}) then mine one block and read it
    back. Legal budget is 8 proofs/block: shield = 1 proof, transfer = 3 (1 spend + 2 outputs)."""
    txids = {}
    for k in kinds:
        r = a.rpc("wallet.shield", {"amount_una": 100_000_000}, 600) if k == "shield" else a.rpc("wallet.transfer", {"amount_una": 40_000_000, "address": recipient}, 900)
        txids[r["txid"]] = k
    gen = a.rpc("generatetoaddress", [1, miner], 600)
    hashes = gen if isinstance(gen, list) else next((v for v in (gen or {}).values() if isinstance(v, list)), []) if isinstance(gen, dict) else []
    if not hashes: hashes = [a.rpc("getbestblockhash", [], 10)]
    blk = a.rpc("getblock", [hashes[0], 1], 60); txs = blk.get("tx", []) if isinstance(blk, dict) else []
    inc = [t for t in txs if t in txids]
    return {"height": blk.get("height"), "hash": hashes[0], "tx_count": len(txs), "shielded_included": len(inc),
            "proofs_included": sum(1 if txids[t] == "shield" else 3 for t in inc), "kinds": [txids[t] for t in inc]}

def wait_height(node, target, timeout):
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        try:
            if node.rpc("getblockcount", [], 10) >= target: return True
        except Exception: pass
        time.sleep(0.25)
    return False

def scenario_two_node(dinerod, out, flags, blocks_per_phase=3):
    """W2 (catch-up with cold proofs) and W3 (reorg onto a chain with cold proofs). Node A produces
    shielded transactions and blocks; node B (the node under test) is offline while they are produced, so
    it has never seen the proofs when it validates the blocks. B's height timeline, RPC probes and CPU are
    the measurements; A's block contents are the receipts."""
    work = tempfile.mkdtemp(prefix="dinero_load2_")
    a = Node(dinerod, os.path.join(work, "a"), flags, listen=True)
    b = Node(dinerod, os.path.join(work, "b"), flags, listen=True)
    res = {"scenario": "two-node", "phases": {}}
    try:
        a.start(connect=b.p2pport); b.start(connect=a.p2pport)
        miner = a.rpc("wallet.getnewaddress", ["taproot", "producer"], 30); miner = miner["address"] if isinstance(miner, dict) else miner
        recipient = a.rpc("wallet.getshieldedaddress", {"account": 1, "j": 0}, 60)["address"]
        a.rpc("generatetoaddress", [140, miner], 600)
        if not wait_height(b, 140, 300): raise RuntimeError("B did not sync the funding chain")
        # confirmed shielded funds on A for transfers
        for _ in range(2): a.rpc("wallet.shield", {"amount_una": 100_000_000}, 600)
        a.rpc("generatetoaddress", [1, miner], 600); wait_height(b, 141, 120)
        h0 = a.rpc("getblockcount", [])
        # ---- W2: B offline while A produces blocks with proofs B has never seen ----
        b.stop()
        produced = []
        for _ in range(blocks_per_phase): produced.append(produce_block_with_proofs(a, miner, recipient, ["transfer", "transfer", "shield", "shield"]))
        target = a.rpc("getblockcount", [])
        b.start(connect=a.p2pport)
        t_start = time.monotonic(); tracker = HeightTracker(b); tracker.start(); probes, rs = start_probes(b, miner)
        synced = wait_height(b, target, 1800); t_sync = time.monotonic() - t_start
        tracker.stop_flag = True; tracker.join(timeout=5); reports, resrep, raw_probe, raw_res = stop_probes(probes, rs)
        changes = [c for c in tracker.changes if c["height"] > h0]
        per_block = [{"height": c["height"], "t_since_restart_s": c["t"] - t_start} for c in changes]
        gaps = [per_block[i]["t_since_restart_s"] - per_block[i-1]["t_since_restart_s"] for i in range(1, len(per_block))]
        res["phases"]["W2_catchup_cold_proofs"] = {"blocks_produced_offline": produced, "proofs_total": sum(p["proofs_included"] for p in produced), "synced": synced,
            "time_to_tip_s": t_sync, "b_height_timeline": per_block, "per_block_gap_s": summarize(gaps), "b_probes": reports, "b_resources": resrep,
            "b_log_counters": failure_counters(b.log_path), "a_log_counters": failure_counters(a.log_path)}
        json.dump({"probes": raw_probe, "resources": raw_res, "heights": tracker.changes}, open(os.path.join(out, "two_node_w2.raw.json"), "w"))
        # ---- W3: B mines its own short chain offline; A extends further with proofs; B reconnects and reorgs ----
        b.stop(); b.connect = None; b.start()   # offline (no peer), mines alone
        bminer = b.rpc("wallet.getnewaddress", ["taproot", "b"], 30); bminer = bminer["address"] if isinstance(bminer, dict) else bminer
        fork_base = b.rpc("getblockcount", [])
        b.rpc("generatetoaddress", [2, bminer], 600); b_tip = b.rpc("getbestblockhash", [], 10)
        produced2 = []
        for _ in range(blocks_per_phase): produced2.append(produce_block_with_proofs(a, miner, recipient, ["transfer", "transfer", "shield", "shield"]))
        a_tip = a.rpc("getbestblockhash", [], 10); target2 = a.rpc("getblockcount", [])
        b.stop(); b.start(connect=a.p2pport)
        t_start = time.monotonic(); tracker = HeightTracker(b); tracker.start(); probes, rs = start_probes(b, miner)
        converged = False
        while time.monotonic() - t_start < 1800:
            try:
                if b.rpc("getbestblockhash", [], 10) == a_tip: converged = True; break
            except Exception: pass
            time.sleep(0.5)
        t_conv = time.monotonic() - t_start
        tracker.stop_flag = True; tracker.join(timeout=5); reports, resrep, raw_probe, raw_res = stop_probes(probes, rs)
        res["phases"]["W3_reorg_onto_cold_proofs"] = {"fork_base_height": fork_base, "b_side_blocks": 2, "a_side_blocks": produced2, "a_side_proofs": sum(p["proofs_included"] for p in produced2),
            "b_tip_before": b_tip, "a_tip": a_tip, "converged_to_a": converged, "time_to_converge_s": t_conv,
            "b_height_timeline": [{"height": c["height"], "t_s": c["t"] - t_start} for c in tracker.changes], "b_probes": reports, "b_resources": resrep,
            "b_log_counters": failure_counters(b.log_path), "a_log_counters": failure_counters(a.log_path)}
        json.dump({"probes": raw_probe, "resources": raw_res, "heights": tracker.changes}, open(os.path.join(out, "two_node_w3.raw.json"), "w"))
        res["qualification_failed"] = (not synced) or (not converged) or any(v for v in res["phases"]["W2_catchup_cold_proofs"]["b_log_counters"].values() if False)
    finally:
        for n in (a, b):
            try: n.stop()
            except Exception: pass
        for n, name in ((a, "a"), (b, "b")):
            if os.path.exists(n.log_path): shutil.copy(n.log_path, os.path.join(out, f"dinerod-{name}.log"))
        shutil.rmtree(work, ignore_errors=True)
    json.dump(res, open(os.path.join(out, "two_node.json"), "w"), indent=2)
    return res

def two_node_md(res, host, design):
    w2 = res["phases"].get("W2_catchup_cold_proofs", {}); w3 = res["phases"].get("W3_reorg_onto_cold_proofs", {})
    L = [f"# Two-node baseline: {design}", "", f"Host: {host}", "", "## W2 catch-up: B validates blocks whose proofs it never saw", ""]
    if w2:
        L += [f"- blocks produced while B was offline: {len(w2['blocks_produced_offline'])}, proofs total {w2['proofs_total']} (per block: {[p['proofs_included'] for p in w2['blocks_produced_offline']]})",
              f"- synced: {w2['synced']}, time to tip after restart {w2['time_to_tip_s']:.1f} s; per-block gap s: {w2['per_block_gap_s']}",
              f"- B CPU % of one core mean {w2['b_resources']['cpu_pct_of_one_core']['mean']:.0f}; B log counters {w2['b_log_counters']}; A {w2['a_log_counters']}", "",
              "| B probe during catch-up | n ok | p50 | p95 | p99 | max | missed | > 1 s |", "|---|---|---|---|---|---|---|---|"]
        for k, p in w2["b_probes"].items(): L.append(f"| {k} | {fmt(p['latency_ms'])} | {p['missed_deadlines']} | {p['slow_over_1s']} |")
    L += ["", "## W3 reorg: B abandons its own 2 blocks for A's longer chain with cold proofs", ""]
    if w3:
        L += [f"- fork base {w3['fork_base_height']}; A side {len(w3['a_side_blocks'])} blocks / {w3['a_side_proofs']} proofs; converged to A: {w3['converged_to_a']} in {w3['time_to_converge_s']:.1f} s",
              f"- B height timeline: {[(c['height'], round(c['t_s'], 1)) for c in w3['b_height_timeline']]}",
              f"- B CPU % mean {w3['b_resources']['cpu_pct_of_one_core']['mean']:.0f}; B log counters {w3['b_log_counters']}", "",
              "| B probe during reorg | n ok | p50 | p95 | p99 | max | missed | > 1 s |", "|---|---|---|---|---|---|---|---|"]
        for k, p in w3["b_probes"].items(): L.append(f"| {k} | {fmt(p['latency_ms'])} | {p['missed_deadlines']} | {p['slow_over_1s']} |")
    L += ["", f"qualification failed: {res.get('qualification_failed')}"]
    return "\n".join(L) + "\n"

def fmt(s):
    return f"{s['n']} | {s['p50']:.0f} | {s['p95']:.0f} | {s['p99']:.0f} | {s['max']:.0f}" if s and s["n"] else "0 | | | |"

def md(rs, rh, host, design):
    L = [f"# Integrated-node load baseline: {design}", "", f"Host: {host}", "", "## steady+service (W1+W4), old design at its own generation rate", "",
         f"- {rs['seconds']} s, achieved {rs['generation']['achieved_tx_per_s']:.3f} shielded tx/s ({rs['generation']['builds_ok']} ok, {len(rs['generation']['builds_failed'])} failed), {rs['blocks']['count']} blocks, {rs['blocks']['shielded_txs_confirmed']} shielded tx confirmed (read back from block contents)",
         f"- daemon CPU % of one core mean {rs['resources']['cpu_pct_of_one_core']['mean']:.0f} p95 {rs['resources']['cpu_pct_of_one_core']['p95']:.0f} ({rs['resources']['samples']} samples); RSS max {rs['resources']['rss_mb']['max']} MB; log counters {rs['log_counters']}", "",
         "| metric (ms) | n | p50 | p95 | p99 | max | missed ticks | errors | >1 s |", "|---|---|---|---|---|---|---|---|---|",
         f"| wallet.shield build+submit | {fmt(rs['generation']['shield_ms'])} | | | |", f"| wallet.transfer build+submit | {fmt(rs['generation']['transfer_ms'])} | | | |",
         f"| block generate incl. validation (own mempool, cached proofs) | {fmt(rs['blocks']['generate_ms'])} | | | |"]
    for k, p in rs["probes"].items():
        extra = f" stale-with-stable-tip {p['stale_template_with_stable_tip']}" if "stale_template_with_stable_tip" in p else ""
        L.append(f"| probe {k} every {p['interval_s']} s | {fmt(p['latency_ms'])} | {p['missed_deadlines']} | {p['errors'] or 0}{extra} | {p['slow_over_1s']} |")
    L += ["", "## hostile (W5): mutated copies of unmined seeds through testmempoolaccept, one client", "", f"- seeds (bytes): {rh['seeds']}", f"- counts: {rh['counts']}", f"- reject reasons: {rh['reject_reasons']}",
          f"- accepted by lane/field: {rh['accepted_by_field']}",
          f"- **qualification failed (acceptance in a proof lane): {rh['qualification_failed']}** (accepted examples: {rh['accepted_examples'][:3]})",
          f"- daemon CPU % of one core mean {rh['resources']['cpu_pct_of_one_core']['mean']:.0f}; log counters {rh['log_counters']}", "",
          "| decision (ms) by seed and reject reason | n | p50 | p95 | p99 | max |", "|---|---|---|---|---|---|"]
    for k, byr in rh["decision_ms_by_reason"].items():
        for r, s in byr.items(): L.append(f"| {k} / {r} | {fmt(s)} |")
    L += ["", "| probe during hostile | n | p50 | p95 | p99 | max | missed | errors | >1 s |", "|---|---|---|---|---|---|---|---|---|"]
    for k, p in rh["probes"].items(): L.append(f"| {k} | {fmt(p['latency_ms'])} | {p['missed_deadlines']} | {p['errors'] or 0} | {p['slow_over_1s']} |")
    return "\n".join(L) + "\n"

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--dinerod", required=True); ap.add_argument("--out", required=True)
    ap.add_argument("--steady-seconds", type=int, default=480); ap.add_argument("--hostile-seconds", type=int, default=240)
    ap.add_argument("--design", default="old (v1 Auth proofs)"); ap.add_argument("--host", default=os.uname().machine)
    ap.add_argument("--hostile-only", action="store_true", help="skip the steady scenario (funds the wallet, then runs W5)")
    ap.add_argument("--two-node", action="store_true", help="run W2 (catch-up with cold proofs) and W3 (reorg) with a producer node A and node under test B")
    ap.add_argument("--blocks-per-phase", type=int, default=3)
    a = ap.parse_args(); os.makedirs(a.out, exist_ok=True)
    flags = ["--consensus-shielded-epoch-reset-height=1", "--consensus-shielded-spend-auth-height=2", "--consensus-state-commitment-height=3"]
    if a.two_node:
        rc = 0
        try:
            res = scenario_two_node(a.dinerod, a.out, flags, a.blocks_per_phase)
            open(os.path.join(a.out, "summary.md"), "w").write(two_node_md(res, a.host, a.design)); print(open(os.path.join(a.out, "summary.md")).read())
            if res.get("qualification_failed"): rc = 2
        except Exception as e:
            open(os.path.join(a.out, "HARNESS_ERROR.txt"), "w").write(repr(e) + "\n"); rc = 1
        print(f"exit={rc}"); sys.exit(rc)
    work = tempfile.mkdtemp(prefix="dinero_load_"); rc = 0
    node = Node(a.dinerod, os.path.join(work, "nut"), ["--consensus-shielded-epoch-reset-height=1", "--consensus-shielded-spend-auth-height=2", "--consensus-state-commitment-height=3"])
    rs = rh = None
    try:
        node.start()
        if a.hostile_only:
            miner = node.rpc("wallet.getnewaddress", ["taproot", "loadgen"], 30); miner = miner["address"] if isinstance(miner, dict) else miner
            node.rpc("generatetoaddress", [140, miner], 600)
            rh = scenario_hostile(node, a.hostile_seconds, a.out, miner)
        else:
            rs = scenario_steady(node, a.steady_seconds, a.out)
            rh = scenario_hostile(node, a.hostile_seconds, a.out, rs["miner"])
        if rh["qualification_failed"]: rc = 2
    except QualificationFailure as e:
        open(os.path.join(a.out, "QUALIFICATION_FAILED.txt"), "w").write(str(e) + "\n"); rc = 2
    except Exception as e:
        open(os.path.join(a.out, "HARNESS_ERROR.txt"), "w").write(repr(e) + "\n"); rc = 1
    finally:
        node.stop()
        if os.path.exists(node.log_path): shutil.copy(node.log_path, os.path.join(a.out, "dinerod.log"))
        shutil.rmtree(work, ignore_errors=True)
    if rs and rh:
        open(os.path.join(a.out, "summary.md"), "w").write(md(rs, rh, a.host, a.design)); print(open(os.path.join(a.out, "summary.md")).read())
    elif rh:
        print(json.dumps({k: rh[k] for k in ("counts", "reject_reasons", "accepted_by_field", "qualification_failed", "decision_ms_by_reason")}, indent=1))
    print(f"exit={rc}"); sys.exit(rc)

if __name__ == "__main__": main()
