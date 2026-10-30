#!/usr/bin/env python3
"""Targets the separation-gate workflows prebuild (Task 0.9).

usage: sep_targets.py --build DIR --config {off,on} --out FILE
           [--list consensus_vectors.txt] [--floor never_run_floor.txt]
           [--ctest ctest] [--ninja ninja]

Prebuilding keeps the ctest drivers (1800 s TIMEOUT) to a link: the gate
libraries, dinerod and the FFI archives (W5), the W4 checker, the W6 static
set, and the executables of the K4 tests. A K4 test contributes its executable
only if it is listed for this config in consensus_vectors.txt ("on:" = ON
only), is NOT on the never-run floor, is registered in the build (its name
appears in ctest --show-only=json-v1) and its command runs an executable
inside the build dir.

ctest's json-v1 omits "command" until the executable exists, i.e. before the
build this script prepares. The command is therefore taken from the generated
CTestTestfile.cmake add_test() lines (absolute executable path, written at
configure time); json-v1's own command is used when present. A registered test
whose command cannot be found either way is UNRESOLVED: it is reported as an
::error::, written to --unresolved-out, and the script exits 3 after writing
--out (the K4 gate step then fails on it). Every target is finally kept only if the build defines it
(ninja -t targets all); a missing one is reported, never passed to the build
(an unknown target would abort ninja before it builds anything).

The sep_*_forced_link / sep_wallet_ffi_standalone probes are deliberately not
listed: they are RED at stage 0 and their ctests build them.

Writes one target per line to --out. Stdlib only. Exit 0; 2 on bad input;
3 if a registered K4 test's target is unresolved.
"""
import argparse
import json
import os
import re
import subprocess
import sys

# add_test([=[Name]=] "arg0" ...) as CMake >= 3.19 writes it; also the older
# add_test(Name "arg0" ...) and a quoted name.
ADD_TEST = re.compile(
    r'^\s*add_test\(\s*(?:\[(?P<eq>=*)\[(?P<bname>.*?)\](?P=eq)\]|"(?P<qname>[^"]*)"|(?P<name>[^\s)]+))'
    r'\s+(?:"(?P<qarg>[^"]*)"|(?P<arg>[^\s)]+))')

BASE_TARGETS = ["dinero_consensus", "dinero_wallet", "dinerod", "nodecore_ffi",
                "dinero_wallet_ffi", "sep_wallet_fixture_compat", "dinero_crypto",
                "secp256k1", "jsoncpp_static", "sqlite3"]


def read_names(text):
    return [l.strip() for l in text.splitlines() if l.strip() and not l.strip().startswith("#")]


def k4_names(list_text, config, floor_names):
    """Names from consensus_vectors.txt that apply to this config, minus floor names."""
    floor = set(floor_names)
    out = []
    for name in read_names(list_text):
        if name.startswith("on:"):
            if config != "on":
                continue
            name = name[3:]
        if name in floor or name in out:
            continue
        out.append(name)
    return out


def ctesttestfile_commands(build):
    """{test name: argv0} from every CTestTestfile.cmake under build."""
    out = {}
    for d, dirs, files in os.walk(build):
        dirs[:] = [x for x in dirs if x not in (".cmake", "CMakeFiles")]
        if "CTestTestfile.cmake" not in files:
            continue
        with open(os.path.join(d, "CTestTestfile.cmake"), errors="replace") as f:
            for line in f:
                m = ADD_TEST.match(line)
                if not m:
                    continue
                name = m.group("bname") if m.group("bname") is not None else (
                    m.group("qname") if m.group("qname") is not None else m.group("name"))
                arg0 = m.group("qarg") if m.group("qarg") is not None else m.group("arg")
                out.setdefault(name, arg0)
    return out


def k4_targets(names, tests, build, add_test_cmds=None):
    """Executable target names (in build) of the registered tests among names.

    tests: the "tests" array of ctest --show-only=json-v1 (a name there means
    registered; "command" is absent before the build). add_test_cmds: from
    ctesttestfile_commands(). Returns (targets, unregistered, unresolved).
    """
    by_name = {t.get("name"): t for t in tests}
    add_test_cmds = add_test_cmds or {}
    root = os.path.normcase(os.path.abspath(build)) + os.sep
    targets, unregistered, unresolved = [], [], []
    for n in names:
        if n not in by_name:
            unregistered.append(n)
            continue
        cmd = by_name[n].get("command") or []
        argv0 = cmd[0] if cmd else add_test_cmds.get(n)
        if not argv0:
            unresolved.append(n)
            continue
        exe = os.path.normcase(os.path.abspath(argv0))
        if not exe.startswith(root):
            continue  # e.g. a python interpreter outside the build dir
        stem = os.path.basename(exe)
        if stem.endswith(".exe"):
            stem = stem[:-4]
        if stem not in targets:
            targets.append(stem)
    return targets, unregistered, unresolved


def available(ninja_targets_text):
    return {l.split(":", 1)[0] for l in ninja_targets_text.splitlines() if ":" in l}


def select(want, avail):
    have, missing = [], []
    for t in want:
        if t in have or t in missing:
            continue
        (have if t in avail else missing).append(t)
    return have, missing


def run(cmd):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True)
    except OSError as e:
        raise SystemExit("ERROR cannot run %s: %s" % (cmd[0], e))
    if r.returncode != 0:
        raise SystemExit("ERROR %s exited %d: %s" % (" ".join(cmd), r.returncode, r.stderr.strip()))
    return r.stdout


def main(argv=None):
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--build", required=True)
    ap.add_argument("--config", required=True, choices=("off", "on"))
    ap.add_argument("--out", required=True)
    ap.add_argument("--unresolved-out", help="write unresolved K4 test names here (one per line)")
    ap.add_argument("--list", default=os.path.join(here, "consensus_vectors.txt"))
    ap.add_argument("--floor", default=os.path.join(here, "never_run_floor.txt"))
    ap.add_argument("--ctest", default="ctest")
    ap.add_argument("--ninja", default="ninja")
    a = ap.parse_args(argv)
    try:
        with open(a.list) as f:
            list_text = f.read()
        with open(a.floor) as f:
            floor = read_names(f.read())
    except OSError as e:
        print("ERROR %s" % e, file=sys.stderr)
        return 2
    if not floor:
        print("ERROR never-run floor %s is empty" % a.floor, file=sys.stderr)
        return 2
    try:
        tests = json.loads(run([a.ctest, "--test-dir", a.build, "--show-only=json-v1"]))["tests"]
        avail = available(run([a.ninja, "-C", a.build, "-t", "targets", "all"]))
    except SystemExit as e:
        print(e, file=sys.stderr)
        return 2
    except (ValueError, KeyError) as e:
        print("ERROR cannot parse ctest test list: %r" % e, file=sys.stderr)
        return 2
    names = k4_names(list_text, a.config, floor)
    k4, unregistered, unresolved = k4_targets(names, tests, a.build, ctesttestfile_commands(a.build))
    for n in unregistered:
        print("K4 test %s is not registered (run_consensus_vectors.py will report NOT_RUN)" % n)
    for n in unresolved:
        print("::error::K4 test %s is registered but its executable target is unresolved "
              "(no command in ctest json-v1 or CTestTestfile.cmake)" % n)
    print("K4: %d listed for %s, %d registered, %d resolved to %d build target(s), %d unresolved"
          % (len(names), a.config, len(names) - len(unregistered),
             len(names) - len(unregistered) - len(unresolved), len(k4), len(unresolved)))
    if a.unresolved_out:
        with open(a.unresolved_out, "w") as f:
            f.write("".join(n + "\n" for n in unresolved))
    have, missing = select(BASE_TARGETS + k4, avail)
    for t in missing:
        print("::warning::target %s does not exist in this configuration" % t)
    print("prebuilding: " + " ".join(have))
    with open(a.out, "w") as f:
        f.write("".join(t + "\n" for t in have))
    return 3 if unresolved else 0


if __name__ == "__main__":
    sys.exit(main())
