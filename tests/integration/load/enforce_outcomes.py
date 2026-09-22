#!/usr/bin/env python3
"""Enforce load-test outcomes on an evidence tree. Exit 0 only if every required result file exists,
parses, and shows a completed, clean, covered run. Used by CI (the same code the unit tests exercise).

Usage: enforce_outcomes.py EVIDENCE_DIR [--require hostile twonode steady] [--out ENFORCEMENT.txt]
"""
import argparse, glob, json, os, re, sys

FATAL_LOG_COUNTERS = ("SAFE MODE", "INVARIANT VIOLATION", "corrupt", "REORG ABORT")
REQUIRED = {"hostile": ["hostile.json", "OUTCOME.json"], "twonode": ["two_node.json", "OUTCOME.json"], "steady": ["steady.json", "hostile.json", "OUTCOME.json"]}

def load(path, bad):
    if not os.path.isfile(path): bad.append(f"missing required file: {path}"); return None
    try:
        d = json.load(open(path))
    except Exception as e:
        bad.append(f"malformed json: {path}: {str(e)[:80]}"); return None
    if not isinstance(d, dict): bad.append(f"malformed result (not an object): {path}"); return None
    return d

def check_hostile(d, path, bad):
    counts = d.get("counts")
    if not isinstance(counts, dict) or not counts: bad.append(f"{path}: no lane counts"); return
    for lane, c in counts.items():
        if not isinstance(c, dict): bad.append(f"{path}: malformed lane {lane}"); continue
        if lane.endswith("/proof") and c.get("accepted", 1) > 0: bad.append(f"{path}: {lane} accepted {c['accepted']} mutated proof(s)")
        if lane.endswith("/proof") and c.get("decisions", 0) == 0: bad.append(f"{path}: {lane} made no decisions")
    for k in FATAL_LOG_COUNTERS:
        if (d.get("log_counters") or {}).get(k, 0): bad.append(f"{path}: daemon log {k} x{d['log_counters'][k]}")
    if d.get("qualification_failed") is not False: bad.append(f"{path}: qualification_failed={d.get('qualification_failed')} {d.get('qualification_failures')}")

def check_twonode(d, path, bad):
    ph = d.get("phases") or {}
    w2, w3 = ph.get("W2_catchup_cold_proofs"), ph.get("W3_reorg_onto_cold_proofs")
    if not w2: bad.append(f"{path}: W2 missing")
    elif w2.get("synced") is not True: bad.append(f"{path}: W2 did not sync")
    if not w3: bad.append(f"{path}: W3 missing")
    elif w3.get("converged_to_a") is not True: bad.append(f"{path}: W3 did not converge")
    for w, name in ((w2, "W2"), (w3, "W3")):
        for side in ("a", "b"):
            for k in FATAL_LOG_COUNTERS:
                if ((w or {}).get(f"{side}_log_counters") or {}).get(k, 0): bad.append(f"{path}: {name} {side} log {k}")
    if d.get("qualification_failed") is not False: bad.append(f"{path}: qualification_failed={d.get('qualification_failed')} {d.get('qualification_failures')}")

def check_steady(d, path, bad):
    if (d.get("blocks") or {}).get("shielded_txs_confirmed", 0) == 0: bad.append(f"{path}: no shielded transaction confirmed")
    for k in FATAL_LOG_COUNTERS:
        if (d.get("log_counters") or {}).get(k, 0): bad.append(f"{path}: daemon log {k}")
    if d.get("qualification_failed") is not False: bad.append(f"{path}: qualification_failed={d.get('qualification_failed')} {d.get('qualification_failures')}")

def check_outcome(d, path, bad):
    if d.get("exit") != 0: bad.append(f"{path}: harness exit {d.get('exit')}")

def enforce(evidence, require):
    bad = []
    codes = os.path.join(evidence, "exit-codes.txt")
    if os.path.isfile(codes):
        for line in open(codes):
            m = re.match(r"(\w+) exit=(\d+)", line.strip())
            if m and int(m.group(2)) != 0: bad.append(f"{m.group(1)}: harness exit {m.group(2)}")
    else: bad.append("missing exit-codes.txt")
    for scen in require:
        sdir = os.path.join(evidence, scen)
        for fname in REQUIRED[scen]:
            d = load(os.path.join(sdir, fname), bad)
            if d is None: continue
            p = os.path.join(scen, fname)
            if fname == "OUTCOME.json": check_outcome(d, p, bad)
            elif fname == "hostile.json": check_hostile(d, p, bad)
            elif fname == "two_node.json": check_twonode(d, p, bad)
            elif fname == "steady.json": check_steady(d, p, bad)
        for f in glob.glob(os.path.join(sdir, "HARNESS_ERROR.txt")) + glob.glob(os.path.join(sdir, "QUALIFICATION_FAILED.txt")):
            bad.append(f"{f}: {open(f).read().strip()[:200]}")
    return bad

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("evidence"); ap.add_argument("--require", nargs="+", default=["hostile", "twonode", "steady"]); ap.add_argument("--out")
    a = ap.parse_args()
    bad = enforce(a.evidence, a.require)
    text = "\n".join(bad) + "\n" if bad else "PASS\n"
    if a.out: open(a.out, "w").write(text)
    sys.stdout.write(text)
    sys.exit(1 if bad else 0)

if __name__ == "__main__": main()
