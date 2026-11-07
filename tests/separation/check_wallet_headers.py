#!/usr/bin/env python3
"""SepWalletHeadersSelfContained (Task 5.4, spec T1).

Every include/wallet/*.h that a dinero_wallet source includes directly must
be self-contained for a wallet-only consumer: its include closure may not
reach include/daemon/, src/daemon/ or the RocksDB headers, and it must
compile on its own with only the public include root.

Stdlib only. Exit 0 = pass; exit 1 = violations (listed); exit 2 = input
error. No assert(): NDEBUG cannot disarm this gate.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

FORBIDDEN = (os.path.join("include", "daemon") + os.sep,
             os.path.join("src", "daemon") + os.sep,
             "third_party" + os.sep + "rocksdb" + os.sep)


def wallet_sources(repo):
    cml = os.path.join(repo, "src", "wallet", "CMakeLists.txt")
    try:
        text = open(cml, encoding="utf-8").read()
    except OSError as e:
        print(f"input error: {e}", file=sys.stderr)
        sys.exit(2)
    out = []
    for m in re.finditer(r"\$\{CMAKE_(?:CURRENT_)?SOURCE_DIR\}/([\w./-]+\.(?:cpp|c))", text):
        rel = m.group(1)
        path = os.path.join(repo, rel)
        if not os.path.exists(path):
            path = os.path.join(repo, "src", "wallet", rel)
        if os.path.exists(path):
            out.append(path)
    return out


def wallet_headers(repo, sources):
    headers = set()
    for src in sources:
        try:
            text = open(src, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        for m in re.finditer(r'#include\s+"(wallet/[\w./-]+\.h)"', text):
            if os.path.exists(os.path.join(repo, "include", m.group(1))):
                headers.add(m.group(1))
    return sorted(headers)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--cxx", default="c++")
    ap.add_argument("--extra-include", action="append", default=[],
                    help="additional -I dir for legitimate vendored deps")
    args = ap.parse_args()
    repo = os.path.abspath(args.repo)

    sources = wallet_sources(repo)
    if not sources:
        print("input error: no dinero_wallet sources found", file=sys.stderr)
        return 2
    headers = wallet_headers(repo, sources)
    if not headers:
        print("input error: no wallet public headers found", file=sys.stderr)
        return 2

    base_flags = ["-std=c++20", "-x", "c++",
                  "-I", os.path.join(repo, "include"),
                  "-I", repo]
    for inc in args.extra_include:
        base_flags += ["-I", inc]
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        for header in headers:
            tu = os.path.join(tmp, "tu.cpp")
            with open(tu, "w", encoding="utf-8") as f:
                f.write(f'#include "{header}"\nint main() {{ return 0; }}\n')
            # (a) include closure must avoid the node directories.
            dep = subprocess.run([args.cxx, *base_flags, "-MM", tu],
                                 capture_output=True, text=True)
            if dep.returncode != 0:
                failures.append((header, "COMPILE", dep.stderr.strip().splitlines()[:3]))
                continue
            bad = sorted({tok for tok in dep.stdout.replace("\\\n", " ").split()
                          for f_ in FORBIDDEN if f_ in tok})
            if bad:
                failures.append((header, "FORBIDDEN_REACH", bad[:5]))
                continue
            # (b) must compile standalone.
            syn = subprocess.run([args.cxx, *base_flags, "-fsyntax-only", tu],
                                 capture_output=True, text=True)
            if syn.returncode != 0:
                failures.append((header, "COMPILE", syn.stderr.strip().splitlines()[:3]))

    print(f"# checked {len(headers)} wallet public headers "
          f"(from {len(sources)} dinero_wallet sources)")
    for header, kind, detail in failures:
        print(f"FAIL {kind} {header}")
        for line in detail:
            print(f"     {line}")
    if failures:
        return 1
    print("# SepWalletHeadersSelfContained: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
