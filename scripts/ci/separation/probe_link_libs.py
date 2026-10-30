#!/usr/bin/env python3
"""Static archives a separation forced-link probe links, from the CMake codemodel.

usage: probe_link_libs.py --build DIR --target sep_wallet_forced_link [--config CFG]

Used on Windows for W3: an MSVC executable carries no symbol table, so the
symbol scan reads the probe's link inputs instead. tests/separation/
CMakeLists.txt links each probe against ONE root library (whole-archive) plus
exactly the deps CMake propagates from it; CMake records that resolved list in
the probe target's link.commandFragments (role "libraries") even when the link
itself fails. This prints, one per line, every such fragment that is a static
archive (.lib/.a) built inside DIR, in link order, de-duplicated. A fragment
counts as built in DIR if it is the artifact of a codemodel target, or if it
names a directory under DIR (e.g. an ExternalProject install). Archives outside
the build dir (prebuilt OpenSSL) and bare system library names (ws2_32.lib)
are skipped and listed on stderr. Existence is NOT checked here: the caller
must treat a listed but missing archive as "not measured".

Reads the reply of the codemodel-v2 query (create
<DIR>/.cmake/api/v1/query/codemodel-v2 before configuring). Stdlib only.
Exit 0, or 2 on bad input (no reply, unknown target, no archive found).
"""
import argparse
import glob
import json
import os
import sys


class InputError(Exception):
    pass


def load(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError) as e:
        raise InputError("cannot read %s: %s" % (path, e))


def target_json(build, target, config):
    reply = os.path.join(build, ".cmake", "api", "v1", "reply")
    indexes = sorted(glob.glob(os.path.join(reply, "index-*.json")))
    if not indexes:
        raise InputError("no codemodel reply in %s (query codemodel-v2 before configure)" % reply)
    index = load(indexes[-1])
    cm = None
    for obj in index.get("objects", []):
        if obj.get("kind") == "codemodel":
            cm = load(os.path.join(reply, obj["jsonFile"]))
    if cm is None:
        raise InputError("codemodel missing from %s" % indexes[-1])
    configs = cm.get("configurations", [])
    if config is not None:
        configs = [c for c in configs if c.get("name") == config]
    if len(configs) != 1:
        raise InputError("expected exactly one configuration, found %d (pass --config)" % len(configs))
    found, artifacts = None, set()
    for t in configs[0].get("targets", []):
        tj = load(os.path.join(reply, t["jsonFile"]))
        for art in tj.get("artifacts", []):
            p = art.get("path", "")
            artifacts.add(norm(p if os.path.isabs(p) else os.path.join(build, p)))
        if t.get("name") == target:
            found = tj
    if found is None:
        raise InputError("target %s not in the codemodel" % target)
    return found, artifacts


def norm(path):
    return os.path.normcase(os.path.abspath(path))


def archives(build, tj, artifacts=()):
    root = norm(build) + os.sep
    out, skipped = [], []
    for fr in tj.get("link", {}).get("commandFragments", []):
        if fr.get("role") != "libraries":
            continue
        frag = fr["fragment"].strip().strip('"')
        if not frag.lower().endswith((".lib", ".a")):
            skipped.append(frag)
            continue
        path = os.path.abspath(frag if os.path.isabs(frag) else os.path.join(build, frag))
        has_dir = os.path.dirname(frag.replace("\\", "/")) != ""
        if not norm(path).startswith(root) or (norm(path) not in artifacts and not has_dir):
            skipped.append(frag)
            continue
        if path not in out:
            out.append(path)
    return out, skipped


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--build", required=True)
    ap.add_argument("--target", required=True)
    ap.add_argument("--config", default=None)
    a = ap.parse_args(argv)
    try:
        tj, artifacts = target_json(a.build, a.target, a.config)
        libs, skipped = archives(a.build, tj, artifacts)
        if not libs:
            raise InputError("no static archive built in %s among %s's link libraries"
                             % (a.build, a.target))
    except InputError as e:
        print("ERROR %s" % e, file=sys.stderr)
        return 2
    for s in skipped:
        print("# skipped (outside build dir or not an archive): %s" % s, file=sys.stderr)
    for p in libs:
        print(p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
