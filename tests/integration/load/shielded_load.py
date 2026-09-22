#!/usr/bin/env python3
"""Integrated-node load harness (plan docs/superpowers/plans/2026-09-22-shielded-v2-integrated-load-test.md).

Single node under test (NUT) on regtest with the Auth profile forced at low heights. The same
script measures the OLD design (v1 Auth proofs, wallet.shield / wallet.transfer) today and the NEW
design (version-7 bundles) once the consensus wiring exists; only the transaction builder differs.

Scenarios in this file (single node): steady+service (W1+W4) and hostile (W5). Catch-up (W2) and
fork (W3) need the two-node variant. Outputs raw JSON + a Markdown summary per scenario.

Usage: shielded_load.py --dinerod BIN --out DIR [--steady-seconds 480] [--hostile-seconds 240]
"""
import argparse, base64, json, os, re, shutil, socket, statistics, subprocess, sys, tempfile, threading, time, urllib.request, urllib.error

def free_ports(n):
    socks, ports = [], []
    for _ in range(n):
        s = socket.socket(); s.bind(("127.0.0.1", 0)); socks.append(s); ports.append(s.getsockname()[1])
    for s in socks: s.close()
    return ports

class Node:
    def __init__(self, dinerod, workdir, extra_flags):
        self.dinerod, self.dir = dinerod, workdir
        self.rpcport, self.p2pport, self.walletport = free_ports(3)
        self.log_path = os.path.join(workdir, "dinerod.log")
        self.proc = None; self.cookie = None; self.extra = extra_flags
    def start(self):
        os.makedirs(self.dir, exist_ok=True)
        self.log = open(self.log_path, "ab")
        args = [self.dinerod, "--regtest", f"--datadir={self.dir}", f"--rpcport={self.rpcport}", f"--port={self.p2pport}",
                f"--wallet-socket-port={self.walletport}", "--listen=0", "--utreexo=1", "--p2p.offline=1"] + self.extra
        self.proc = subprocess.Popen(args, stdout=self.log, stderr=subprocess.STDOUT)
        t0 = time.time()
        while time.time() - t0 < 120:
            if self.proc.poll() is not None: raise RuntimeError("dinerod exited early; see " + self.log_path)
            for c in (os.path.join(self.dir, ".cookie"), os.path.join(self.dir, "regtest", ".cookie")):
                if os.path.exists(c):
                    self.cookie = open(c, "rb").read().strip()
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
        """Returns (result) or raises. HTTP 503 'RPC server busy' raises BusyError."""
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params}).encode()
        req = urllib.request.Request(f"http://127.0.0.1:{self.rpcport}/", body,
                                     {"Content-Type": "application/json", "Authorization": "Basic " + base64.b64encode(self.cookie).decode()})
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r: reply = json.load(r)
        except urllib.error.HTTPError as e:
            if e.code == 503: raise BusyError()
            raise
        if reply.get("error"): raise RuntimeError(f"{method}: {reply['error']}")
        res = reply.get("result")
        if isinstance(res, dict) and res.get("error"): raise RuntimeError(f"{method}: {res['error']} {res.get('error_message','')}")
        return res
    def cpu_rss(self):
        out = subprocess.run(["ps", "-o", "%cpu=,rss=", "-p", str(self.proc.pid)], capture_output=True, text=True).stdout.split()
        return (float(out[0]), int(out[1]) // 1024) if len(out) == 2 else (0.0, 0)

class BusyError(Exception): pass

def pct(xs, p):
    if not xs: return None
    xs = sorted(xs); k = max(0, min(len(xs) - 1, round(p / 100 * (len(xs) - 1)))); return xs[k]

def summarize(xs):
    return {"n": len(xs), "p50": pct(xs, 50), "p95": pct(xs, 95), "p99": pct(xs, 99), "max": max(xs) if xs else None, "mean": statistics.fmean(xs) if xs else None}

class Probe(threading.Thread):
    """Times cheap and expensive RPCs every second, samples CPU/RSS, counts busy (503) responses and slow (>1 s) replies."""
    def __init__(self, node, template_every=2.0, template_params=None):
        super().__init__(daemon=True); self.node = node; self.stop_flag = False; self.template_params = template_params or [[]]; self.errors = {}
        self.lat = {"getblockcount": [], "getrawmempool": [], "wallet.shieldedbalance": [], "getblocktemplate": []}
        self.busy = 0; self.slow = 0; self.cpu = []; self.rss = []; self.template_tips = []; self.template_every = template_every
    def timed(self, method, params, timeout):
        t0 = time.perf_counter()
        try: res = self.node.rpc(method, params, timeout=timeout)
        except BusyError: self.busy += 1; return None
        except Exception as e: self.errors[method] = str(e)[:200]; return None
        dt = (time.perf_counter() - t0) * 1000; self.lat[method].append(dt)
        if dt > 1000: self.slow += 1
        return res
    def run(self):
        last_tpl = 0
        while not self.stop_flag:
            t = time.time()
            self.timed("getblockcount", [], 30); self.timed("getrawmempool", [], 30); self.timed("wallet.shieldedbalance", [], 30)
            if t - last_tpl >= self.template_every:
                res = None
                for tp in self.template_params:
                    res = self.timed("getblocktemplate", tp, 60)
                    if res is not None: break
                last_tpl = t
                if isinstance(res, dict):
                    try: best = self.node.rpc("getbestblockhash", [], 10)
                    except Exception: best = None
                    self.template_tips.append((t, res.get("previousblockhash"), best))
            c, r = self.node.cpu_rss(); self.cpu.append(c); self.rss.append(r)
            time.sleep(max(0, 1.0 - (time.time() - t)))

def validation_ms_from_log(path, since_height):
    out = {}
    for line in open(path, "rb").read().decode("utf-8", "replace").splitlines():
        if '"validation_ms"' in line:
            m = re.search(r'"block_height"\s*:\s*(\d+).*?"tx_count"\s*:\s*(\d+).*?"validation_ms"\s*:\s*([\d.]+)', line)
            if m and int(m.group(1)) > since_height: out[int(m.group(1))] = (int(m.group(2)), float(m.group(3)))
    return out

def failure_counters(path):
    txt = open(path, "rb").read().decode("utf-8", "replace")
    return {k: len(re.findall(k, txt)) for k in ("SAFE MODE", "INVARIANT VIOLATION", "corrupt", "REORG ABORT", "RPC server busy")}

def tx_hex(node, txid):
    for m, p in (("getrawtransaction", [txid]), ("getrawtransaction", [txid, 0]), ("getrawtransaction", [txid, 1]), ("blockchain.getrawtransaction", [txid, 1]),
                 ("wallet.gettransaction", [txid]), ("gettransaction", [txid]), ("wallet.getrawtransaction", [txid])):
        try:
            r = node.rpc(m, p, timeout=30)
            if isinstance(r, str) and len(r) > 200: return r
            if isinstance(r, dict):
                for k in ("hex", "raw", "rawtx", "raw_hex"):
                    if isinstance(r.get(k), str) and len(r[k]) > 200: return r[k]
        except Exception: continue
    return None

def mutate(hexstr, i):
    # flip one nibble inside the trailing proof region; distinct position per i so every proof is a fresh cache miss
    pos = len(hexstr) - 41 - (i * 7) % 3000
    c = hexstr[pos]; new = format((int(c, 16) ^ (1 + i % 15)) & 0xF, "x")
    return hexstr[:pos] + new + hexstr[pos + 1:]

def scenario_steady(node, seconds, out):
    """W1+W4: wallet produces shielded txs as fast as it can (old design: 1 Auth proof set per tx);
    a block is mined whenever the mempool has txs and >= 15 s passed (<= 8 shielded proofs per block);
    probes measure RPC/template latency and CPU throughout."""
    miner = node.rpc("wallet.getnewaddress", ["taproot", "loadgen"], 30)
    miner = miner["address"] if isinstance(miner, dict) else miner
    node.rpc("generatetoaddress", [140, miner], 600)   # 40 mature coinbases to shield from
    start_height = node.rpc("getblockcount", [])
    recipient = node.rpc("wallet.getshieldedaddress", {"account": 1, "j": 0}, 60)["address"]
    probe = Probe(node, template_params=[[{"address": miner}], [], [{}]]); probe.start()
    builds, mined, blocks, submitted = [], [], [], 0
    confirmed_shields = 0
    t_end = time.time() + seconds; last_block = time.time(); first_txid = None; txids = []; seed_hex = [None]
    while time.time() < t_end:
        # transfers need confirmed notes: shield until two are confirmed, then alternate 1 shield : 2 transfers
        kind = "shield" if (confirmed_shields < 2 or len(builds) % 3 == 0) else "transfer"
        t0 = time.perf_counter()
        try:
            if kind == "shield": r = node.rpc("wallet.shield", {"amount_una": 100_000_000}, 600)   # fee auto-sized by the wallet
            else: r = node.rpc("wallet.transfer", {"amount_una": 40_000_000, "address": recipient}, 900)
            dt = (time.perf_counter() - t0) * 1000; builds.append({"kind": kind, "ms": dt, "vsize": r.get("vsize")}); submitted += 1
            if r.get("txid"):
                txids.append(r["txid"])
                if first_txid is None:
                    first_txid = r["txid"]; seed_hex[0] = tx_hex(node, first_txid)   # only retrievable while still in the mempool
        except BusyError: probe.busy += 1
        except Exception as e:
            builds.append({"kind": kind, "ms": (time.perf_counter() - t0) * 1000, "error": str(e)[:160]})
            time.sleep(1)
        mem = node.rpc("getrawmempool", [], 30)
        if mem and time.time() - last_block >= 15:
            t0 = time.perf_counter(); h = node.rpc("generatetoaddress", [1, miner], 600); dt = (time.perf_counter() - t0) * 1000
            mined.append(dt); blocks.append({"height": node.rpc("getblockcount", []), "txs_in_mempool_before": len(mem), "generate_ms": dt}); last_block = time.time()
            confirmed_shields += sum(1 for b in builds[-len(mem):] if b["kind"] == "shield" and "error" not in b)
    if node.rpc("getrawmempool", [], 30):
        t0 = time.perf_counter(); node.rpc("generatetoaddress", [1, miner], 600); mined.append((time.perf_counter() - t0) * 1000)
    probe.stop_flag = True; probe.join(timeout=90)
    vm = validation_ms_from_log(node.log_path, start_height)
    stale = sum(1 for t in probe.template_tips if t[1] and t[2] and t[1] != t[2])   # template tip behind the node's own best hash
    res = {"scenario": "steady+service", "seconds": seconds, "start_height": start_height, "end_height": node.rpc("getblockcount", []),
           "tx_builds": {"count": len([b for b in builds if "error" not in b]), "errors": [b for b in builds if "error" in b][:10],
                         "shield_ms": summarize([b["ms"] for b in builds if b["kind"] == "shield" and "error" not in b]),
                         "transfer_ms": summarize([b["ms"] for b in builds if b["kind"] == "transfer" and "error" not in b])},
           "blocks": {"count": len(mined), "generate_ms(incl. validation)": summarize(mined), "per_block": blocks,
                      "validation_ms_from_log": {str(h): {"tx_count": v[0], "validation_ms": v[1]} for h, v in sorted(vm.items())},
                      "validation_ms_summary": summarize([v[1] for v in vm.values()])},
           "probe": {k: summarize(v) for k, v in probe.lat.items()}, "probe_busy_503": probe.busy, "probe_slow_over_1s": probe.slow,
           "template_stale_pairs": stale, "probe_errors": probe.errors, "cpu_pct": summarize(probe.cpu), "rss_mb": summarize(probe.rss), "log_counters": failure_counters(node.log_path),
           "first_txid": first_txid, "seed_hex_bytes": (len(seed_hex[0]) // 2) if seed_hex[0] else 0}
    json.dump(res, open(os.path.join(out, "steady.json"), "w"), indent=2)
    return res, seed_hex[0]

def scenario_hostile(node, seconds, hexstr, out):
    """W5: mutated-proof copies of one real Auth transaction pushed through testmempoolaccept as fast as
    the node answers (each mutation is a fresh verification-cache miss), with the probe thread running."""
    if not hexstr: return {"scenario": "hostile", "skipped": "no raw Auth transaction hex captured during the steady scenario"}
    probe = Probe(node); probe.start()
    decisions, accepted, busy = [], 0, 0
    t_end = time.time() + seconds; i = 0
    while time.time() < t_end:
        h = mutate(hexstr, i); i += 1
        t0 = time.perf_counter()
        try:
            r = node.rpc("testmempoolaccept", [h], 300)
            if isinstance(r, dict) and r.get("allowed") is True: accepted += 1
        except BusyError: busy += 1
        except Exception: pass
        decisions.append((time.perf_counter() - t0) * 1000)
    probe.stop_flag = True; probe.join(timeout=90)
    res = {"scenario": "hostile", "seconds": seconds, "invalid_submitted": i, "invalid_accepted(must be 0)": accepted, "busy_503": busy,
           "decision_ms": summarize(decisions), "probe": {k: summarize(v) for k, v in probe.lat.items()}, "probe_busy_503": probe.busy,
           "probe_slow_over_1s": probe.slow, "cpu_pct": summarize(probe.cpu), "rss_mb": summarize(probe.rss), "log_counters": failure_counters(node.log_path)}
    json.dump(res, open(os.path.join(out, "hostile.json"), "w"), indent=2)
    return res

def md(res_steady, res_hostile, host, design):
    def row(name, s): return f"| {name} | {s['n']} | {s['p50']:.0f} | {s['p95']:.0f} | {s['p99']:.0f} | {s['max']:.0f} |" if s and s["n"] else f"| {name} | 0 | | | | |"
    L = [f"# Integrated-node load baseline: {design}", "", f"Host: {host}", "", "## steady+service (W1+W4)", "",
         f"- duration {res_steady['seconds']} s, blocks {res_steady['blocks']['count']}, tx builds {res_steady['tx_builds']['count']}, errors {len(res_steady['tx_builds']['errors'])}",
         f"- probe busy(503) {res_steady['probe_busy_503']}, probe replies >1 s {res_steady['probe_slow_over_1s']}, stale template pairs {res_steady['template_stale_pairs']}",
         f"- daemon CPU % mean {res_steady['cpu_pct']['mean']:.0f} p95 {res_steady['cpu_pct']['p95']:.0f}; RSS MB max {res_steady['rss_mb']['max']}", f"- log counters {res_steady['log_counters']}", "",
         "| metric (ms) | n | p50 | p95 | p99 | max |", "|---|---|---|---|---|---|",
         row("wallet.shield build+submit", res_steady["tx_builds"]["shield_ms"]), row("wallet.transfer build+submit", res_steady["tx_builds"]["transfer_ms"]),
         row("block generate (incl. validation)", res_steady["blocks"]["generate_ms(incl. validation)"]), row("block validation_ms (log)", res_steady["blocks"]["validation_ms_summary"])]
    for k, v in res_steady["probe"].items(): L.append(row(f"probe {k}", v))
    L += ["", "## hostile (W5)", ""]
    if res_hostile.get("skipped"): L.append(f"skipped: {res_hostile['skipped']}")
    else:
        L += [f"- {res_hostile['invalid_submitted']} mutated proofs in {res_hostile['seconds']} s; accepted {res_hostile['invalid_accepted(must be 0)']}; busy(503) {res_hostile['busy_503']}",
              f"- probe busy(503) {res_hostile['probe_busy_503']}, probe replies >1 s {res_hostile['probe_slow_over_1s']}; CPU % mean {res_hostile['cpu_pct']['mean']:.0f}; log counters {res_hostile['log_counters']}", "",
              "| metric (ms) | n | p50 | p95 | p99 | max |", "|---|---|---|---|---|---|", row("invalid-proof decision (testmempoolaccept)", res_hostile["decision_ms"])]
        for k, v in res_hostile["probe"].items(): L.append(row(f"probe {k}", v))
    return "\n".join(L) + "\n"

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--dinerod", required=True); ap.add_argument("--out", required=True)
    ap.add_argument("--steady-seconds", type=int, default=480); ap.add_argument("--hostile-seconds", type=int, default=240)
    ap.add_argument("--design", default="old (v1 Auth proofs)"); ap.add_argument("--host", default=os.uname().machine)
    a = ap.parse_args(); os.makedirs(a.out, exist_ok=True)
    work = tempfile.mkdtemp(prefix="dinero_load_")
    node = Node(a.dinerod, os.path.join(work, "nut"), ["--consensus-shielded-epoch-reset-height=1", "--consensus-shielded-spend-auth-height=2", "--consensus-state-commitment-height=3"])
    try:
        node.start()
        rs, seed = scenario_steady(node, a.steady_seconds, a.out)
        rh = scenario_hostile(node, a.hostile_seconds, seed, a.out)
    finally:
        node.stop(); shutil.copy(node.log_path, os.path.join(a.out, "dinerod.log")); shutil.rmtree(work, ignore_errors=True)
    open(os.path.join(a.out, "summary.md"), "w").write(md(rs, rh, a.host, a.design)); print(open(os.path.join(a.out, "summary.md")).read())

if __name__ == "__main__": main()
