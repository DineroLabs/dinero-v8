#!/usr/bin/env python3
"""Chain-read inventory for src/daemon/mempool.cpp (spec section 7.4).

usage: chain_read_sites.py record <mempool.cpp> --out <json>
       chain_read_sites.py check  <mempool.cpp> --baseline <json> [--adapter <header>]

For each top-level definition (a class, or a function whose name contains
'::'), records the multiset of chain reads, the number of chainstate read-guard
acquisitions, and whether each guard is taken before the function's first
pool-lock (m_mutex) acquisition. Reads through ChainDB (before) and through
IMempoolChainView (after) map to the same names, so the move to the view must
leave the inventory identical.

'check' prints 'DIFF <function> <baseline> -> <current>' per difference and,
with --adapter, 'ADAPTER_LOCK <line>' for any locking in the adapter header.
Exit 0 clean, 1 on any difference, 2 on input errors.
"""
import argparse
import json
import re
import sys
from collections import Counter
from pathlib import Path

# receiver->method  ->  canonical read name
RECEIVER = r"\b(?:chain_db_|chain_view_)(?:fallback_)?->"
READS = [
    (re.compile(RECEIVER + r"(?:getTip|GetTip)\s*\("), "getTip"),
    (re.compile(RECEIVER + r"(?:getCoinWithConfidentialFallback|GetCoinWithConfidentialFallback)\s*\("), "getCoin"),
    (re.compile(RECEIVER + r"(?:getTxLocation|HaveTransaction)\s*\("), "getTxLocation"),
    (re.compile(RECEIVER + r"(?:forEachUTXO|HaveConfidentialCommitment)\s*\("), "commitmentScan"),
    (re.compile(r"\b(?:chain_db_fallback_|chain_view_fallback_)->getCoin\s*\("), "fallbackGetCoin"),
    (re.compile(r"\b(?:chain_db_fallback_|chain_view_fallback_)->hasCoin\s*\("), "fallbackHasCoin"),
    (re.compile(r"\bMempoolUTXOView\s+\w+\s*\("), "mempoolUTXOView"),
]
GUARD = re.compile(r"chainstate_read_guard_factory_\s*\(\s*\)")
POOL_LOCK = re.compile(r"\b(?:unique_lock|shared_lock|lock_guard)\s*<[^>]*>\s*\w+\s*\(\s*m_mutex\b")
DEF_FUNC = re.compile(r"^[A-Za-z_][^;{}()]*?\b((?:\w+::)+~?\w+)\s*\(")
DEF_CLASS = re.compile(r"^(?:class|struct)\s+(\w+)\b[^;]*$")
ADAPTER_LOCK = re.compile(r"\b(?:mutex|lock_guard|unique_lock|shared_lock|scoped_lock)\b")
COMMENT = re.compile(r"//.*$")


def inventory(text):
    functions = {}
    current = None
    total = 0
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = COMMENT.sub("", raw)
        m = DEF_FUNC.match(line) or DEF_CLASS.match(line)
        if m:
            current = m.group(1)
            functions.setdefault(current, {"reads": Counter(), "guard_lines": [], "lock_line": None})
        if current is None:
            continue
        f = functions[current]
        for pattern, name in READS:
            n = len(pattern.findall(line))
            if n:
                f["reads"][name] += n
                total += n
        if GUARD.search(line):
            f["guard_lines"].append(lineno)
        if f["lock_line"] is None and POOL_LOCK.search(line):
            f["lock_line"] = lineno
    out = {}
    for name, f in functions.items():
        if not f["reads"] and not f["guard_lines"]:
            continue
        lock = f["lock_line"]
        out[name] = {
            "reads": dict(sorted(f["reads"].items())),
            "guards": len(f["guard_lines"]),
            "guards_before_pool_lock": [lock is None or g < lock for g in f["guard_lines"]],
        }
    return {"functions": dict(sorted(out.items())), "total_reads": total}


def summary(entry):
    if entry is None:
        return "absent"
    return json.dumps(entry, sort_keys=True)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("mode", choices=["record", "check"])
    ap.add_argument("source")
    ap.add_argument("--out")
    ap.add_argument("--baseline")
    ap.add_argument("--adapter")
    a = ap.parse_args(argv)
    try:
        current = inventory(Path(a.source).read_text(errors="replace"))
    except OSError as e:
        print("ERROR %s" % e, file=sys.stderr)
        return 2
    if a.mode == "record":
        if not a.out:
            print("ERROR --out required", file=sys.stderr)
            return 2
        Path(a.out).write_text(json.dumps(current, indent=2, sort_keys=True) + "\n")
        print("# recorded %d functions, %d chain reads" % (len(current["functions"]), current["total_reads"]))
        return 0
    if not a.baseline:
        print("ERROR --baseline required", file=sys.stderr)
        return 2
    try:
        base = json.loads(Path(a.baseline).read_text())
    except (OSError, ValueError) as e:
        print("ERROR %s" % e, file=sys.stderr)
        return 2
    bad = False
    for name in sorted(set(base["functions"]) | set(current["functions"])):
        b, c = base["functions"].get(name), current["functions"].get(name)
        if b != c:
            print("DIFF %s %s -> %s" % (name, summary(b), summary(c)))
            bad = True
    if a.adapter:
        try:
            for lineno, raw in enumerate(Path(a.adapter).read_text().splitlines(), 1):
                if ADAPTER_LOCK.search(COMMENT.sub("", raw)):
                    print("ADAPTER_LOCK %s:%d %s" % (a.adapter, lineno, raw.strip()))
                    bad = True
        except OSError as e:
            print("ERROR %s" % e, file=sys.stderr)
            return 2
    print("# %d functions, %d chain reads (baseline %d)" % (
        len(current["functions"]), current["total_reads"], base["total_reads"]))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
