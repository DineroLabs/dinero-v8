#!/usr/bin/env python3
"""W5 external interface freeze (spec sections 4, 8 W5 row, 9).

usage: interface_freeze.py record|check --build DIR [--repo .]
           [--baseline-dir tests/separation/interface_baseline]
           [--nodecore-ffi LIB] [--wallet-ffi LIB]

Frozen items, each compared exactly against the baseline directory:
  F1 headers     include/nodecore/nodecore_ffi.h and wallet-core/ffi/wallet_ffi.h,
                 byte for byte (baseline copies nodecore_ffi.h, wallet_ffi.h).
  F2 JNI exports every `extern "C" JNIEXPORT <ret> JNICALL Java_org_dinerolabs_*(...)`
                 definition in src/nodecore/nodecore_jni_android.cpp, as one line
                 `<ret> <name>(<params>)` with whitespace collapsed (jni_exports.txt).
                 The parse must account for every Java_org_dinerolabs_ occurrence,
                 or the run is an input error.
  F3 symbols     the defined external symbols of the built nodecore_ffi and
                 dinero_wallet_ffi artifacts (nm -gU on Apple; nm -D --defined-only
                 for a shared library elsewhere, nm -g --defined-only for an
                 archive), one `<lib> <type> <name>` line each, sorted, in
                 symbols_<platform>_<off|on>.txt (config read from the build's
                 CMakeCache DINERO_BUILD_ORCHARD_BACKEND).
                 On macOS (and every non-Android platform) both targets are STATIC
                 archives, so "exported" means the archive's global defined symbols.
                 Only C-linkage symbols are frozen: C++-mangled names (`_Z`/`__Z`) are
                 the internal C++ interfaces the spec allows to change (section 4),
                 and compiler-generated helpers (`___clang*`, `__clang*`) depend on
                 code generation, not on the interface. Nothing else is filtered.
`record` writes the baseline; `check` prints PASS/FAIL per item with a diff.
Exit 0 all PASS, 1 any FAIL, 2 input error (missing artifact, missing baseline
for this platform/config, unparseable source).
"""
import argparse
import difflib
import os
import platform
import re
import shutil
import subprocess
import sys

HEADERS = (("nodecore_ffi.h", "include/nodecore/nodecore_ffi.h"),
           ("wallet_ffi.h", "wallet-core/ffi/wallet_ffi.h"))
JNI_SOURCE = "src/nodecore/nodecore_jni_android.cpp"
JNI_PREFIX = "Java_org_dinerolabs_"
JNI_DEF = re.compile(r'extern\s+"C"\s+JNIEXPORT\s+(\w+)\s+JNICALL\s+(' + JNI_PREFIX +
                     r'\w+)\s*\(([^)]*)\)')
LIBS = (("nodecore_ffi", "libnodecore_ffi"), ("dinero_wallet_ffi", "libdinero_wallet_ffi"))


class InputError(Exception):
    pass


def read(path):
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError as e:
        raise InputError("cannot read %s: %s" % (path, e))


def jni_exports(repo):
    text = read(os.path.join(repo, JNI_SOURCE)).decode()
    rows = []
    for ret, name, params in JNI_DEF.findall(text):
        params = " ".join(params.split())
        rows.append("%s %s(%s)" % (ret, name, params))
    if len(rows) != text.count(JNI_PREFIX) or not rows:
        raise InputError("%s: parsed %d JNI definitions but found %d '%s' occurrences"
                         % (JNI_SOURCE, len(rows), text.count(JNI_PREFIX), JNI_PREFIX))
    return "".join(r + "\n" for r in sorted(rows))


def build_config(build):
    cache = os.path.join(build, "CMakeCache.txt")
    m = re.search(r"^DINERO_BUILD_ORCHARD_BACKEND:\w+=(\S*)$", read(cache).decode(), re.M)
    if not m:
        return "off"
    return "on" if m.group(1).upper() in ("ON", "1", "TRUE", "YES") else "off"


def platform_tag():
    s = platform.system()
    return {"Darwin": "macos", "Linux": "linux", "Windows": "windows"}.get(s, s.lower())


def find_lib(build, stem, explicit):
    if explicit:
        if not os.path.isfile(explicit):
            raise InputError("artifact not found: %s" % explicit)
        return explicit
    for ext in (".dylib", ".so", ".a"):
        p = os.path.join(build, stem + ext)
        if os.path.isfile(p):
            return p
    raise InputError("artifact %s.{dylib,so,a} not found in %s (build the target first)"
                     % (stem, build))


def symbols(path):
    if platform.system() == "Darwin":
        cmd = ["nm", "-gU", path]
    elif path.endswith(".a"):
        cmd = ["nm", "-g", "--defined-only", path]
    else:
        cmd = ["nm", "-D", "--defined-only", path]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True)
    except OSError as e:
        raise InputError("cannot run nm: %s" % e)
    if r.returncode != 0:
        raise InputError("%s failed: %s" % (" ".join(cmd), r.stderr.strip()))
    out = set()
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) < 2 or line.endswith(":"):
            continue
        typ, name = parts[-2], parts[-1]
        bare = name[1:] if platform.system() == "Darwin" and name.startswith("_") else name
        if bare.startswith("_Z") or bare.startswith("__clang"):
            continue
        out.add((typ, name))
    if not out:
        raise InputError("no C-linkage symbols found in %s" % path)
    return out


def symbol_text(build, args):
    rows = []
    for (target, stem), explicit in zip(LIBS, (args.nodecore_ffi, args.wallet_ffi)):
        for typ, name in symbols(find_lib(build, stem, explicit)):
            rows.append("%s %s %s" % (target, typ, name))
    return "".join(r + "\n" for r in sorted(rows))


def current(args):
    items = []
    for base, rel in HEADERS:
        items.append((base, read(os.path.join(args.repo, rel))))
    items.append(("jni_exports.txt", jni_exports(args.repo).encode()))
    sym_name = "symbols_%s_%s.txt" % (platform_tag(), build_config(args.build))
    items.append((sym_name, symbol_text(args.build, args).encode()))
    return items


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["record", "check"])
    ap.add_argument("--build", required=True)
    ap.add_argument("--repo", default=os.path.normpath(os.path.join(os.path.dirname(
        os.path.abspath(__file__)), "..", "..")))
    ap.add_argument("--baseline-dir")
    ap.add_argument("--nodecore-ffi")
    ap.add_argument("--wallet-ffi")
    args = ap.parse_args(argv)
    bdir = args.baseline_dir or os.path.join(args.repo, "tests", "separation", "interface_baseline")
    try:
        items = current(args)
        if args.mode == "record":
            os.makedirs(bdir, exist_ok=True)
            for name, data in items:
                with open(os.path.join(bdir, name), "wb") as f:
                    f.write(data)
                print("interface_freeze: recorded %s" % name)
            return 0
        failed = False
        for name, data in items:
            path = os.path.join(bdir, name)
            if not os.path.isfile(path):
                raise InputError("no baseline %s (record it at the baseline for this "
                                 "platform/config)" % path)
            base = read(path)
            if base == data:
                print("PASS %s" % name)
                continue
            failed = True
            print("FAIL %s" % name)
            sys.stdout.writelines(difflib.unified_diff(
                base.decode(errors="replace").splitlines(True),
                data.decode(errors="replace").splitlines(True),
                "baseline/" + name, "current/" + name, n=2))
        print("interface_freeze: %s" % ("FAIL" if failed else "PASS"))
        return 1 if failed else 0
    except InputError as e:
        print("interface_freeze: INPUT ERROR: %s" % e, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
