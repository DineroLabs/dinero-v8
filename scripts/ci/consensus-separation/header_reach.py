#!/usr/bin/env python3
"""CV1: forbidden headers reached by the objects of the consensus libraries (preprocessor, -M -MG).

  header_reach.py record <build_dir> <src_root> <rules.json> <baseline.json>
  header_reach.py check  <build_dir> <src_root> <rules.json> <baseline.json>

rules.json: {"targets": [...], "bucket_a_sources": [...], "allowlisted_headers": [...],
             "controls": [{"source": ..., "family": ...}]}

Exit 0 = pass. Exit 1 (check) = NEW_REACH: a source outside bucket A reaches a forbidden header that is
not in the baseline. Exit 2 = the measurement can't be trusted: a preprocess error, an UNRESOLVED dependency
(-MG would otherwise hide a forbidden header), a failed control, or no compile entries.
Bucket A reaches (sources the wallet plan's leg 1a moves) and RESOLVED reaches are reported, never failing.
"""
import json, os, re, shlex, subprocess, sys
from collections import defaultdict
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cc_index

FAMILIES = re.compile(r"^(?:include|src)/(storage|daemon|p2p|net|rpc|wallet|mining|indexing|mempool|node|chainstate|core)/")
EXTERNAL = [("rocksdb", re.compile(r"(^|/)rocksdb/[^/]+\.h$")), ("sqlite3", re.compile(r"(^|/)sqlite3\.h$"))]


def family(rel, absolute):
    m = FAMILIES.match(rel)
    if m:
        return m.group(1)
    for name, rx in EXTERNAL:
        if rx.search(absolute):
            return name
    return None


def header_key(rel, absolute):
    # Headers outside the source tree (SDK, Homebrew) get a machine-independent key.
    return rel if not rel.startswith("..") else "<external>/" + os.path.basename(absolute)


def dep_command(entry):
    args = shlex.split(entry["command"]) if "command" in entry else list(entry["arguments"])
    out, skip = [], False
    for a in args:
        if skip:
            skip = False; continue
        if a in ("-o", "-MF", "-MT", "-MQ"):
            skip = True; continue
        if a in ("-c", "-MD", "-MMD"):
            continue
        out.append(a)
    return out + ["-M", "-MG"]


def measure(build, root, rules):
    root = os.path.realpath(root)
    allow = set(rules.get("allowlisted_headers", []))
    targets = set(rules["targets"])
    entries = [(rel, t, e) for rel, t, e in cc_index.load(build, root) if t in targets]
    found = {t for _, t, _ in entries}
    for t in sorted(targets - found):
        raise cc_index.InputError("no compile entries for %s" % t)
    reach, fams, problems = defaultdict(set), {}, []
    for rel, target, e in sorted(entries, key=lambda x: (x[0], x[1])):
        r = subprocess.run(dep_command(e), cwd=e["directory"], capture_output=True, text=True)
        if r.returncode != 0:
            last = (r.stderr.strip().splitlines() or ["?"])[-1]
            problems.append("PREPROCESS_ERROR %s (%s): %s" % (rel, target, last)); continue
        deps = r.stdout.replace("\\\n", " ").split(":", 1)[1].split()
        for d in deps[1:]:  # deps[0] is the source itself
            p = os.path.realpath(os.path.join(e["directory"], d))
            if not os.path.exists(p):
                problems.append("UNRESOLVED %s <- %s" % (d, rel)); continue
            hrel = os.path.relpath(p, root)
            key = header_key(hrel, p)
            f = family(hrel, p)
            if f and key not in allow:
                reach[rel].add(key); fams[key] = f
    return {s: sorted(h) for s, h in sorted(reach.items())}, fams, problems, len(entries)


def controls_ok(rules, reach, fams):
    ok = True
    for c in rules.get("controls", []):
        hit = any(fams[h] == c["family"] for h in reach.get(c["source"], []))
        print("CONTROL %s -> %s: %s" % (c["source"], c["family"], "PASS" if hit else "FAIL"))
        ok = ok and hit
    return ok


def main(argv):
    if len(argv) != 6 or argv[1] not in ("record", "check"):
        sys.stderr.write(__doc__); return 2
    mode, build, root, rules_path, base = argv[1:]
    rules = json.load(open(rules_path))
    try:
        reach, fams, problems, n = measure(build, root, rules)
    except cc_index.InputError as exc:
        cc_index.fail_input(exc)
    for p in problems:
        print(p)
    ctl = controls_ok(rules, reach, fams)
    untrusted = bool(problems) or not ctl
    if untrusted and mode == "record":
        print("CV1 UNTRUSTED: %d problems, controls %s" % (len(problems), "ok" if ctl else "FAILED"))
        return 2
    if mode == "record":
        json.dump({"gate": "CV1", "targets": sorted(rules["targets"]), "objects": n, "reach": reach, "family": fams},
                  open(base, "w"), indent=1, sort_keys=True)
        print("CV1 recorded: %d objects, %d sources with forbidden reach" % (n, len(reach)))
        return 0
    known = json.load(open(base))["reach"]
    bucket_a = set(rules.get("bucket_a_sources", []))
    new = 0
    for s, hs in reach.items():
        for h in hs:
            if h in known.get(s, []):
                continue
            if s in bucket_a:
                print("BUCKET_A %s -> %s" % (s, h))
            else:
                print("NEW_REACH %s -> %s (%s)" % (s, h, fams[h])); new += 1
    for s, hs in sorted(known.items()):
        for h in hs:
            if h not in reach.get(s, []):
                print("RESOLVED %s -> %s" % (s, h))
    remaining_a = sum(len(hs) for s, hs in reach.items() if s in bucket_a)
    verdict = "UNTRUSTED" if untrusted else ("FAIL" if new else "PASS")
    print("CV1 %s: %d objects, %d new reaches, %d bucket-A reaches remaining, %d problems"
          % (verdict, n, new, remaining_a, len(problems)))
    return 2 if untrusted else (1 if new else 0)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
