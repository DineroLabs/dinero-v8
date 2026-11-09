#!/usr/bin/env python3
"""Chain-read inventory for src/daemon/mempool.cpp (spec section 7.4).

usage: chain_read_sites.py record <mempool.cpp> --out <json>
       chain_read_sites.py check  <mempool.cpp> --baseline <json> [--adapter <header>]

For each definition at namespace scope (function, free/static helper, class or
struct), records the multiset of chain reads, the number of chainstate
read-guard acquisitions, and whether each guard is taken before the
definition's first pool-lock (m_mutex) acquisition. Reads through ChainDB
(before) and through IMempoolChainView (after) map to the same names, so the
move to the view must leave the inventory identical.

Source is lexed first: comments are removed and string/char literals blanked,
so commented-out reads don't count and '//' inside a string hides nothing.
Definition boundaries follow brace depth, so split signatures, free helpers and
code after a class body are attributed correctly. Reads are matched with
whitespace (including newlines) removed around '->' and '.'. Calls to a chain
method on any other receiver are recorded as 'other:<read>', so an aliased read
never compares equal to a direct one.

'check' prints 'DIFF <definition> <baseline> -> <current>' per difference,
'UNRESOLVED <definition>' for a guard in a definition with no recognisable
m_mutex acquisition, and with --adapter 'ADAPTER_LOCK <line>' for any locking
primitive in the adapter header. Exit 0 clean, 1 on any finding, 2 on input errors.
"""
import argparse
import json
import re
import sys
from collections import Counter
from pathlib import Path

CHAIN_RECEIVER = re.compile(r"^(?:chain_db_|chain_view_)$")
FALLBACK_RECEIVER = re.compile(r"^(?:chain_db_fallback_|chain_view_fallback_)$")
METHODS = {
    "getTip": "getTip", "GetTip": "getTip",
    "getCoinWithConfidentialFallback": "getCoin", "GetCoinWithConfidentialFallback": "getCoin",
    "getTxLocation": "getTxLocation", "HaveTransaction": "getTxLocation",
    "forEachUTXO": "commitmentScan", "HaveConfidentialCommitment": "commitmentScan",
}
FALLBACK_METHODS = {"getCoin": "fallbackGetCoin", "hasCoin": "fallbackHasCoin"}
# receiver(->|.)method(   after whitespace normalisation
CALL = re.compile(r"([A-Za-z_]\w*)?(->|\.)([A-Za-z_]\w*)\(")
UTXO_VIEW = re.compile(r"\bMempoolUTXOView\s+\w+\s*[({]")
GUARD = re.compile(r"chainstate_read_guard_factory_\s*\(\s*\)")
POOL_LOCK = re.compile(
    r"\b(?:unique_lock|shared_lock|lock_guard|scoped_lock)\b[^;]*?[({]\s*m_mutex\b"
    r"|\bm_mutex\s*\.\s*(?:lock|lock_shared|try_lock|try_lock_shared)\s*\(")
ADAPTER_LOCK = re.compile(r"\b(?:mutex|lock_guard|unique_lock|shared_lock|scoped_lock)\b")
CLASS_HEAD = re.compile(r"\b(?:class|struct)\s+(?:\[\[[^\]]*\]\]\s*)?([A-Za-z_]\w*)\b(?!\s*;)")
FUNC_HEAD = re.compile(r"((?:[A-Za-z_]\w*::)*~?[A-Za-z_]\w*)\s*\(")
NAMESPACE_HEAD = re.compile(r"\bnamespace\b[^;{]*$|\bextern\s+\"C\"\s*$")
SKIP_HEAD_WORDS = {"if", "for", "while", "switch", "return", "sizeof", "decltype",
                   "alignof", "static_assert", "noexcept", "throw", "catch"}


def lex(text):
    """Remove comments, blank string/char literal contents; keep newlines."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            while i < n and text[i] != "\n":
                i += 1
        elif c == "/" and nxt == "*":
            i += 2
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                if text[i] == "\n":
                    out.append("\n")
                i += 1
            i += 2
        elif c == "R" and nxt == '"':
            m = re.match(r'R"([^(\s]*)\(', text[i:])
            if not m:
                out.append(c)
                i += 1
                continue
            end = text.find(")" + m.group(1) + '"', i + m.end())
            end = n if end < 0 else end + len(m.group(1)) + 2
            out.append('""' + "\n" * text.count("\n", i, end))
            i = end
        elif c in "\"'":
            q = c
            out.append(q)
            i += 1
            while i < n and text[i] != q:
                if text[i] == "\\":
                    i += 1
                elif text[i] == "\n":
                    break
                i += 1
            out.append(q)
            i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def head_name(head):
    """Name of the definition whose body opens after `head` (text since the
    previous ';', '{' or '}' at namespace scope), or None."""
    h = " ".join(head.split())
    if not h or NAMESPACE_HEAD.search(h):
        return None
    m = CLASS_HEAD.search(h)
    if m and "(" not in h[:m.start()]:
        # These two private adapters were renamed when their ChainDB dependency
        # became a borrowed interface. Preserve the recorded read identity.
        return {"MempoolChainUTXOView": "ChainDBUTXOView",
                "MempoolChainStateView": "ChainDBStateView"}.get(m.group(1), m.group(1))
    # function: the identifier before the parameter list. The first '(' after
    # the name may follow template args or attributes; take the last qualified
    # name that is followed by '(' and is not a keyword, before any ')' at depth 0
    best = None
    for fm in FUNC_HEAD.finditer(h):
        name = fm.group(1)
        if name.split("::")[-1] in SKIP_HEAD_WORDS:
            continue
        best = name
        break
    return best


def definitions(clean):
    """Yield (name, start_line, body_text) for each namespace-scope body."""
    depth = 0           # brace depth excluding namespace braces
    ns_stack = []       # True for braces that open a namespace/extern block
    head_start = 0
    body_start = None
    name = None
    line = 1
    start_line = 1
    for i, c in enumerate(clean):
        if c == "\n":
            line += 1
        if c == "{":
            if depth == 0:
                head = clean[head_start:i]
                if NAMESPACE_HEAD.search(" ".join(head.split())):
                    ns_stack.append(True)
                    head_start = i + 1
                    continue
                name = head_name(head)
                body_start = i + 1
                start_line = line
            ns_stack.append(False)
            depth += 1
        elif c == "}":
            if ns_stack and ns_stack[-1]:
                ns_stack.pop()
                head_start = i + 1
                continue
            if ns_stack:
                ns_stack.pop()
            depth -= 1
            if depth == 0:
                if name:
                    yield name, start_line, clean[body_start:i]
                name = None
                head_start = i + 1
        elif c == ";" and depth == 0:
            head_start = i + 1


def scan_body(body):
    flat = re.sub(r"\s*(->|\.)\s*", r"\1", body)
    flat = re.sub(r"\s+\(", "(", flat)
    reads = Counter()
    for m in CALL.finditer(flat):
        recv, method = m.group(1) or "", m.group(3)
        if FALLBACK_RECEIVER.match(recv) and method in FALLBACK_METHODS:
            reads[FALLBACK_METHODS[method]] += 1
        elif method in METHODS:
            if CHAIN_RECEIVER.match(recv) or FALLBACK_RECEIVER.match(recv):
                reads[METHODS[method]] += 1
            else:
                reads["other:" + METHODS[method]] += 1
    reads["mempoolUTXOView"] += len(UTXO_VIEW.findall(body))
    reads = Counter({k: v for k, v in reads.items() if v})
    guards = [m.start() for m in GUARD.finditer(body)]
    lock = POOL_LOCK.search(body)
    lock_at = lock.start() if lock else None
    before = [None if lock_at is None else g < lock_at for g in guards]
    return reads, guards, before


def inventory(text):
    clean = lex(text)
    functions = {}
    total = 0
    for name, _line, body in definitions(clean):
        reads, guards, before = scan_body(body)
        if not reads and not guards:
            continue
        key = name
        n = 2
        while key in functions:            # overloads / repeated names
            key = "%s#%d" % (name, n)
            n += 1
        functions[key] = {
            "reads": dict(sorted(reads.items())),
            "guards": len(guards),
            "guards_before_pool_lock": before,
        }
        total += sum(v for k, v in reads.items() if not k.startswith("other:"))
    return {"functions": dict(sorted(functions.items())), "total_reads": total}


def summary(entry):
    return "absent" if entry is None else json.dumps(entry, sort_keys=True)


def unresolved(inv):
    return sorted(k for k, v in inv["functions"].items() if None in v["guards_before_pool_lock"])


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
        for name in unresolved(current):
            print("UNRESOLVED %s" % name)
        print("# recorded %d definitions, %d chain reads" % (len(current["functions"]), current["total_reads"]))
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
    for name in unresolved(current):
        print("UNRESOLVED %s (guard with no recognisable m_mutex acquisition)" % name)
        bad = True
    if a.adapter:
        try:
            adapter = lex(Path(a.adapter).read_text())
        except OSError as e:
            print("ERROR %s" % e, file=sys.stderr)
            return 2
        for lineno, raw in enumerate(adapter.splitlines(), 1):
            if ADAPTER_LOCK.search(raw):
                print("ADAPTER_LOCK %s:%d %s" % (a.adapter, lineno, raw.strip()))
                bad = True
    print("# %d definitions, %d chain reads (baseline %d)" % (
        len(current["functions"]), current["total_reads"], base["total_reads"]))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
