#!/usr/bin/env python3
"""Prerequisite-build status for the separation-gate workflows (Task 0.9).

usage: prereq_status.py scan --build DIR --build-outcome OUTCOME --platform PLAT --out FILE
       prereq_status.py gate --status FILE --gate ID [--github-output FILE]
       prereq_status.py lane --status FILE

A gate measures nothing when a target it depends on failed to build: its
check would only rebuild that target and fail. Such a gate is UNMEASURED
(not a pass, not a measured fail), and the lane says why.

The base targets are the four the prerequisite build exists for:
  dinero_consensus, dinero_wallet, dinerod, sep_wallet_fixture_compat.
The FFI archives are NOT base targets: a nodecore_ffi or dinero_wallet_ffi
build failure stays a W5 FAIL ("check did not run"), never UNMEASURED.

scan: when --build-outcome is "success" nothing failed. Otherwise a base
  target failed if any of its codemodel artifacts (ignoring .pdb) is absent
  from the build dir (the CI build dir is fresh, so an artifact exists only if
  this build produced it). Each gate's anchors (below) plus their transitive
  build-order 'dependencies' from the CMake codemodel-v2 reply decide which
  failed base targets block that gate. Writes JSON {"failed": [...],
  "gates": {id: [...]}} to --out and prints the LANE line when anything failed.
gate: exit 0 and print nothing when the gate's prerequisites built (the gate
  then runs and reports its real result). Otherwise print
  `GATE <id> UNMEASURED (prerequisite build failed: <targets>)`, write
  state=UNMEASURED and why=... to --github-output, and exit 3. A missing or
  unreadable status file is "check did not run" (exit 2), never a verdict.
  Gates that need no build (K2, W2, K5) always exit 0.
lane: print `LANE UNMEASURED: prerequisite build failed: <targets>` or nothing.

Exit 2 on bad input. Stdlib only.
"""
import argparse
import glob
import json
import os
import sys

BASE_TARGETS = ("dinero_consensus", "dinero_wallet", "dinerod", "sep_wallet_fixture_compat")
# Gate -> targets whose build (with their dependencies) the gate needs.
ANCHORS = {
    "K1": ("sep_consensus_forced_link",),
    "K3": ("sep_consensus_forced_link",),
    "K4": ("dinero_consensus",),
    "W1": ("sep_wallet_forced_link",),
    "W3": ("sep_wallet_forced_link",),
    "W4": ("sep_wallet_fixture_compat",),
    "W5": ("dinerod", "dinero_wallet_ffi", "nodecore_ffi"),
    "W6": ("sep_wallet_ffi_standalone",),
}
NO_BUILD = ("K2", "W2", "K5")
# NodeCore is not a Windows target (src/nodecore/CMakeLists.txt): W5 on
# Windows does not need it.
WINDOWS_EXCLUDED = ("nodecore_ffi",)


class InputError(Exception):
    pass


def load(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError) as e:
        raise InputError("cannot read %s: %s" % (path, e))


def codemodel_targets(build):
    """{name: target json} from the codemodel-v2 reply (one configuration)."""
    reply = os.path.join(build, ".cmake", "api", "v1", "reply")
    indexes = sorted(glob.glob(os.path.join(reply, "index-*.json")))
    if not indexes:
        raise InputError("no codemodel reply in %s (query codemodel-v2 before configure)" % reply)
    ref = load(indexes[-1]).get("reply", {}).get("codemodel-v2")
    if not ref or "jsonFile" not in ref:
        raise InputError("index %s has no codemodel-v2 reply" % indexes[-1])
    configs = load(os.path.join(reply, ref["jsonFile"])).get("configurations", [])
    if len(configs) != 1:
        raise InputError("expected exactly one configuration, found %d" % len(configs))
    out = {}
    for t in configs[0].get("targets", []):
        out[t["name"]] = load(os.path.join(reply, t["jsonFile"]))
    return out


def closure(targets, names):
    by_id = {tj["id"]: n for n, tj in targets.items()}
    seen, stack = set(), [n for n in names if n in targets]
    seen.update(stack)
    while stack:
        for d in targets[stack.pop()].get("dependencies", []):
            n = by_id.get(d.get("id"))
            if n is not None and n not in seen:
                seen.add(n)
                stack.append(n)
    return seen


def built(build, tj):
    arts = [a.get("path", "") for a in tj.get("artifacts", [])]
    arts = [p for p in arts if p and not p.lower().endswith(".pdb")]
    if not arts:
        return False
    return all(os.path.isfile(p if os.path.isabs(p) else os.path.join(build, p)) for p in arts)


def scan(build, outcome, plat):
    gates = {g: [] for g in ANCHORS}
    if outcome == "success":
        return [], gates
    targets = codemodel_targets(build)
    failed = [t for t in BASE_TARGETS if t in targets and not built(build, targets[t])]
    for g, anchors in ANCHORS.items():
        if plat == "windows":
            anchors = [a for a in anchors if a not in WINDOWS_EXCLUDED]
        need = closure(targets, anchors)
        gates[g] = [t for t in failed if t in need]
    return failed, gates


def lane_line(failed):
    return "LANE UNMEASURED: prerequisite build failed: " + ", ".join(failed)


def read_status(path):
    st = load(path)
    if not isinstance(st, dict) or not isinstance(st.get("failed"), list) \
            or not isinstance(st.get("gates"), dict):
        raise InputError("malformed prerequisite status %s" % path)
    return st


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="mode", required=True)
    ps = sub.add_parser("scan")
    ps.add_argument("--build", required=True)
    ps.add_argument("--build-outcome", required=True)
    ps.add_argument("--platform", required=True, choices=("linux", "macos", "windows"))
    ps.add_argument("--out", required=True)
    pg = sub.add_parser("gate")
    pg.add_argument("--status", required=True)
    pg.add_argument("--gate", required=True, choices=sorted(ANCHORS) + list(NO_BUILD))
    pg.add_argument("--github-output")
    pl = sub.add_parser("lane")
    pl.add_argument("--status", required=True)
    a = ap.parse_args(argv)
    try:
        if a.mode == "scan":
            failed, gates = scan(a.build, a.build_outcome, a.platform)
            with open(a.out, "w") as f:
                json.dump({"failed": failed, "gates": gates}, f, indent=1, sort_keys=True)
            print("prerequisite build outcome: %s; failed base targets: %s"
                  % (a.build_outcome, " ".join(failed) or "none"))
            if failed:
                print(lane_line(failed))
            return 0
        if a.mode == "gate" and a.gate in NO_BUILD:
            return 0
        st = read_status(a.status)
        if a.mode == "lane":
            if st["failed"]:
                print(lane_line(st["failed"]))
            return 0
        blocked = st["gates"].get(a.gate)
        if not isinstance(blocked, list):
            raise InputError("gate %s missing from %s" % (a.gate, a.status))
        if not blocked:
            return 0
        why = "prerequisite build failed: " + ", ".join(blocked)
        print("GATE %s UNMEASURED (%s)" % (a.gate, why))
        if a.github_output:
            with open(a.github_output, "a") as f:
                f.write("state=UNMEASURED\nwhy=%s\n" % why)
        return 3
    except InputError as e:
        if a.mode == "gate":
            print("GATE %s FAIL rc=2 (check did not run: no prerequisite build status: %s)" % (a.gate, e))
        else:
            print("prereq_status: INPUT ERROR: %s" % e, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
