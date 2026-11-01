#!/usr/bin/env python3
"""CV3: who calls each consensus entry point, and whether that caller is live.

  live_callers.py record <build_dir> <src_root> <rules.json> <baseline.json>
  live_callers.py check  <build_dir> <src_root> <rules.json> <baseline.json>

rules.json: {"roots": ["dinerod"], "entry_points": [{"id", "pattern" (regex for a call), "exclude": [files]}]}

Each caller file is classified:
  live          compiled into a target in the link closure of a root (CMake File API codemodel)
  other-target  compiled, but only into targets outside that closure (tests, tools, other executables)
  uncompiled    a source in no target
  header        a header (whether it is compiled depends on its includers)
Text match, not a call graph: a same-named member function matches if the pattern allows it, so patterns
are qualified. Comment-only lines and trailing // comments are ignored.
check exits 1 when any (entry point, file) class changes from the baseline, so "which validator is live"
changes only by review. Exit 2 = bad input.
"""
import glob, json, os, re, sys
from collections import defaultdict
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cc_index

SCAN_DIRS = ["src", "include", "tests", "tools", "wallet-core", "rust", "qt"]
SOURCE_EXT = (".cpp", ".cc", ".c", ".mm")
HEADER_EXT = (".h", ".hpp")


def root_closure(build, roots):
    reply = os.path.join(build, ".cmake", "api", "v1", "reply")
    idx = sorted(glob.glob(os.path.join(reply, "index-*.json")))
    if not idx:
        raise cc_index.InputError("no CMake File API reply in %s (create .cmake/api/v1/query/codemodel-v2 and re-run cmake)" % build)
    index = json.load(open(idx[-1]))
    cm = [o for o in index["objects"] if o["kind"] == "codemodel"]
    if not cm:
        raise cc_index.InputError("no codemodel in File API reply")
    model = json.load(open(os.path.join(reply, cm[0]["jsonFile"])))
    by_id, by_name = {}, {}
    for t in model["configurations"][0]["targets"]:
        by_id[t["id"]] = t; by_name[t["name"]] = t
    deps = {}
    for t in by_id.values():
        deps[t["id"]] = [d["id"] for d in json.load(open(os.path.join(reply, t["jsonFile"]))).get("dependencies", [])]
    closure, stack = set(), []
    for r in roots:
        if r not in by_name:
            raise cc_index.InputError("root target %s not in codemodel" % r)
        stack.append(by_name[r]["id"])
    while stack:
        i = stack.pop()
        if i in closure:
            continue
        closure.add(i); stack.extend(deps.get(i, []))
    return {by_id[i]["name"] for i in closure if i in by_id}


def code_lines(path):
    for line in open(path, errors="replace"):
        s = line.strip()
        if s.startswith("//") or s.startswith("*") or s.startswith("/*"):
            continue
        yield line.split("//", 1)[0]


def measure(build, root, rules):
    live_targets = root_closure(build, rules["roots"])
    compiled = defaultdict(set)
    for rel, target, _ in cc_index.load(build, root):
        compiled[rel].add(target)
    pats = [(e["id"], re.compile(e["pattern"]), set(e.get("exclude", []))) for e in rules["entry_points"]]
    callers = {pid: {} for pid, _, _ in pats}
    for d in SCAN_DIRS:
        for dirpath, dirnames, files in os.walk(os.path.join(root, d)):
            dirnames[:] = [x for x in dirnames if x not in ("third_party", "target", "build", ".git")]
            for f in files:
                if not f.endswith(SOURCE_EXT + HEADER_EXT):
                    continue
                full = os.path.join(dirpath, f)
                rel = os.path.relpath(full, root)
                text = "\n".join(code_lines(full))
                for pid, rx, excl in pats:
                    if rel in excl or not rx.search(text):
                        continue
                    if rel in compiled:
                        cls = "live" if compiled[rel] & live_targets else "other-target"
                    else:
                        cls = "header" if f.endswith(HEADER_EXT) else "uncompiled"
                    callers[pid][rel] = cls
    return {k: dict(sorted(v.items())) for k, v in callers.items()}


def main(argv):
    if len(argv) != 6 or argv[1] not in ("record", "check"):
        sys.stderr.write(__doc__); return 2
    mode, build, root, rules_path, base = argv[1:]
    rules = json.load(open(rules_path))
    try:
        callers = measure(build, root, rules)
    except cc_index.InputError as exc:
        cc_index.fail_input(exc)
    for pid, c in callers.items():
        summary = defaultdict(int)
        for cls in c.values():
            summary[cls] += 1
        print("%s: %s" % (pid, ", ".join("%s=%d" % kv for kv in sorted(summary.items())) or "no callers"))
    if mode == "record":
        json.dump({"gate": "CV3", "roots": rules["roots"], "callers": callers}, open(base, "w"), indent=1, sort_keys=True)
        print("CV3 recorded %d entry points" % len(callers))
        return 0
    known = json.load(open(base))["callers"]
    changed = 0
    for pid in sorted(set(known) | set(callers)):
        a, b = known.get(pid, {}), callers.get(pid, {})
        for f in sorted(set(a) | set(b)):
            if a.get(f) != b.get(f):
                print("CHANGED %s %s: %s -> %s" % (pid, f, a.get(f, "absent"), b.get(f, "absent"))); changed += 1
    print("CV3 %s: %d entry points, %d changes" % ("FAIL" if changed else "PASS", len(callers), changed))
    return 1 if changed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
