#!/usr/bin/env python3
"""CV4: sources compiled into more than one production library target.

  dup_objects.py record <build_dir> <src_root> <baseline.json>
  dup_objects.py check  <build_dir> <src_root> <baseline.json>

check exits 1 on a NEW_DUPLICATE (a source newly in >1 library, or a known duplicate in an extra library);
a duplicate that went away is printed as RESOLVED and does not fail. Exit 2 = bad input.
"""
import json, sys
from collections import defaultdict
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cc_index


def measure(build, root):
    targets = defaultdict(set)
    for rel, target, _ in cc_index.load(build, root):
        if cc_index.is_library_target(target):
            targets[rel].add(target)
    return {f: sorted(t) for f, t in sorted(targets.items()) if len(t) > 1}


def main(argv):
    if len(argv) != 5 or argv[1] not in ("record", "check"):
        sys.stderr.write(__doc__); return 2
    mode, build, root, base = argv[1:]
    try:
        dups = measure(build, root)
    except cc_index.InputError as exc:
        cc_index.fail_input(exc)
    if mode == "record":
        json.dump({"gate": "CV4", "duplicates": dups}, open(base, "w"), indent=1, sort_keys=True)
        print("CV4 recorded %d duplicate sources" % len(dups))
        return 0
    known = json.load(open(base))["duplicates"]
    bad = 0
    for f, t in dups.items():
        extra = sorted(set(t) - set(known.get(f, [])))
        if f not in known or extra:
            print("NEW_DUPLICATE %s targets=%s" % (f, ",".join(t))); bad += 1
    for f in sorted(set(known) - set(dups)):
        print("RESOLVED %s (was %s)" % (f, ",".join(known[f])))
    print("CV4 %s: %d duplicates, %d new" % ("FAIL" if bad else "PASS", len(dups), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
