#!/bin/bash
# Bug #7 / #41 regression guard:
# Forbids function calls in src/zk/zkvm/ where TWO OR MORE top-level
# arguments are themselves function calls that take the R1CS builder `cs`
# as their first argument.
#
# C++ does NOT specify the evaluation order of function arguments. When
# two (or more) arguments are side-effecting calls against the same `cs`,
# the compiler is free to evaluate them in any order, and different
# compilers/platforms make different choices — producing different R1CS
# constraint orderings on different architectures.
#
# This was the root cause of Bug #7 / #41 (cross-architecture Spartan
# verification consensus split between x86_64 Linux and ARM64 macOS),
# fixed in commit 0a2a7e271 by extracting each sub-call to a named local
# before the outer call.
#
# A single side-effecting sub-call is SAFE — the side effect is sequenced
# before the outer call regardless of evaluation order. This script only
# flags TWO-OR-MORE cases.
#
# Forbidden pattern:
#
#     gadgets::mul(cs,
#         fe_pack(cs, dx, label + "_dxp"),                    // ← side effect
#         gadgets::constant(cs, Scalar::one(), label + "_1a"), // ← side effect
#         label + "_dxm")
#
# Required pattern:
#
#     Variable a = gadgets::constant(cs, Scalar::one(), label + "_1a");
#     Variable b = fe_pack(cs, dx, label + "_dxp");
#     Variable c = gadgets::mul(cs, b, a, label + "_dxm");
#
# Safe (only one side-effecting sub-call):
#
#     gadgets::assert_equal(cs, ok,                               // arg 1: var ref
#         gadgets::constant(cs, Scalar::one(), label + "_1"),     // arg 2: side effect
#         label + "_check");                                       // arg 3: string
#
# To allow a flagged call site deliberately, add `// LINT:ALLOW_NESTED_CS_CALL`
# on the same line as the outer call or the line immediately above.

set -e

REPO_ROOT="$(git rev-parse --show-toplevel)"
ZK_DIR="$REPO_ROOT/src/zk/zkvm"

echo "🔍 Checking src/zk/zkvm/ for multi-arg side-effecting calls (Bug #7 / #41 guard)..."

if [ ! -d "$ZK_DIR" ]; then
    echo "⚠️  Directory $ZK_DIR not found — skipping check"
    exit 0
fi

python3 - <<'PYEOF' "$ZK_DIR"
import os
import re
import sys

zk_dir = sys.argv[1]

IDENT = r"[A-Za-z_][A-Za-z0-9_:]*"

def strip_comments_and_strings(src: str) -> str:
    """Replace comments/strings with spaces (preserving newlines) so the
    scanner cannot match inside commented-out code or string literals."""
    out = []
    i = 0
    n = len(src)
    while i < n:
        ch = src[i]
        nxt = src[i+1] if i + 1 < n else ""
        if ch == '/' and nxt == '/':
            j = src.find('\n', i)
            if j == -1:
                out.append(' ' * (n - i)); break
            out.append(' ' * (j - i))
            i = j; continue
        if ch == '/' and nxt == '*':
            j = src.find('*/', i + 2)
            if j == -1:
                out.append(' ' * (n - i)); break
            j += 2
            for k in range(i, j):
                out.append('\n' if src[k] == '\n' else ' ')
            i = j; continue
        if ch == '"':
            out.append('"')
            i += 1
            while i < n and src[i] != '"':
                if src[i] == '\\' and i + 1 < n:
                    out.append(' '); out.append(' '); i += 2; continue
                out.append('\n' if src[i] == '\n' else ' ')
                i += 1
            if i < n:
                out.append('"'); i += 1
            continue
        out.append(ch); i += 1
    return ''.join(out)

SAFE_INNER = {
    'static_cast', 'reinterpret_cast', 'const_cast', 'dynamic_cast',
    'std::move', 'std::forward', 'std::ref', 'std::cref', 'move',
    'sizeof', 'alignof', 'typeid', 'offsetof',
    'assert', 'DCHECK', 'static_assert',
}

outer_re = re.compile(r'(' + IDENT + r')\s*\(\s*cs\s*,')
arg_call_re = re.compile(r'^\s*(' + IDENT + r')\s*\(\s*cs\s*,')

def split_top_level_args(body: str):
    """Split `body` (the text between outer `(` and matching `)`) into
    top-level arguments, respecting paren / bracket / angle depth."""
    args = []
    depth = 0
    start = 0
    for i, c in enumerate(body):
        if c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
        elif c == ',' and depth == 0:
            args.append(body[start:i])
            start = i + 1
    args.append(body[start:])
    return args

violations = []

for fname in sorted(os.listdir(zk_dir)):
    if not fname.endswith('.cpp'):
        continue
    path = os.path.join(zk_dir, fname)
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        raw = f.read()
    src = strip_comments_and_strings(raw)
    line_starts = [0]
    for idx, c in enumerate(raw):
        if c == '\n':
            line_starts.append(idx + 1)

    def pos_to_line(p: int) -> int:
        lo, hi = 0, len(line_starts) - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if line_starts[mid] <= p:
                lo = mid
            else:
                hi = mid - 1
        return lo + 1

    def line_has_allow(lineno: int) -> bool:
        i = line_starts[lineno - 1]
        j = line_starts[lineno] if lineno < len(line_starts) else len(raw)
        line = raw[i:j]
        above = ""
        if lineno >= 2:
            ai = line_starts[lineno - 2]
            aj = line_starts[lineno - 1]
            above = raw[ai:aj]
        return 'LINT:ALLOW_NESTED_CS_CALL' in line or 'LINT:ALLOW_NESTED_CS_CALL' in above

    for m in outer_re.finditer(src):
        outer_name = m.group(1)
        # Body starts right after the outer `(`, which is at m.end() - 1 in
        # the " ( cs ," capture; we need to re-anchor to find the opening
        # paren precisely.
        # Find the `(` after outer_name.
        open_paren = src.find('(', m.start(1) + len(outer_name))
        if open_paren == -1:
            continue
        depth = 1
        end = open_paren + 1
        while end < len(src) and depth > 0:
            c = src[end]
            if c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
                if depth == 0:
                    break
            end += 1
        body = src[open_paren + 1:end]
        # Split into top-level args (the outer's direct arguments)
        args = split_top_level_args(body)
        # The first arg is `cs`. Check how many of args[1:] are themselves
        # function calls of the form `IDENT(cs, ...)`.
        side_effecting_args = []
        for a in args[1:]:
            am = arg_call_re.match(a)
            if am:
                inner = am.group(1)
                if inner in SAFE_INNER:
                    continue
                side_effecting_args.append(inner)
        if len(side_effecting_args) >= 2:
            lineno = pos_to_line(open_paren)
            if line_has_allow(lineno):
                continue
            violations.append((path, lineno, outer_name, side_effecting_args))

if not violations:
    print("   ✅ No multi-arg side-effecting (cs, ...) calls in src/zk/zkvm/")
    sys.exit(0)

print()
print("❌ FAIL: multi-arg side-effecting (cs, ...) calls found in src/zk/zkvm/")
print()
print("C++ does not specify function argument evaluation order. Two or more")
print("sub-calls that mutate `cs` are evaluated in compiler-specific order,")
print("producing different R1CS on different architectures. Bug #7 / #41.")
print()
print("Fix: extract each sub-call to a named local before the outer call.")
print()
for path, lineno, outer, inners in violations:
    inners_str = " + ".join(f"{name}(cs, ...)" for name in inners)
    print(f"{path}:{lineno}: {outer}(cs, ...) with {len(inners)} side-effecting args: {inners_str}")
print()
print("To deliberately allow a specific call site, add:")
print("    // LINT:ALLOW_NESTED_CS_CALL")
print("on the same line as the outer call or the line immediately above.")
print()
sys.exit(1)
PYEOF
