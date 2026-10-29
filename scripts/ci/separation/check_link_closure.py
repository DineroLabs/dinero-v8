#!/usr/bin/env python3
"""Transitive link-closure checker for the K2/W2 separation gates.

usage: check_link_closure.py --build <dir> --target <name> --rules closure_rules.json
                             --gate {K2,W2} [--config <name>]

Reads the CMake File API codemodel-v2 reply under <dir>/.cmake/api/v1/reply
(the lexicographically last index-*.json), walks the link closure of <target>
and prints one line per violation:

  FORBIDDEN_TARGET <name|fragment> rule=<label> ...   forbidden target / external lib
  FORBIDDEN_INCLUDE <target> <include-dir> pattern=<re>
  CYCLE a->b->a                                       cycle inside the walked closure
  NODE_SOURCE <path> target=<name>                    unlisted src/{daemon,rpc,...} source
  UNRESOLVED <id> from=<name>                         link id found in no target (fail closed)

Lines starting with '#' are informational. Exit 0 clean, 1 on any violation,
2 on bad input (missing reply, unknown target, unreadable JSON).

Edge rule (which link items are followed out of a target):
  * the root, STATIC_LIBRARY and OBJECT_LIBRARY targets: linkLibraries and
    interfaceLinkLibraries (static libraries propagate PRIVATE deps);
  * every other type (SHARED/MODULE/EXECUTABLE/INTERFACE/imported): only
    interfaceLinkLibraries (the PUBLIC/INTERFACE part).
  The build-order 'dependencies' list is never used: it cannot tell PRIVATE
  from PUBLIC, omits imported targets, and is cycle-broken for static libs.
  Codemodel 2.9 (linkLibraries, interfaceLinkLibraries, abstractTargets) is
  therefore REQUIRED, i.e. CMake >= 4.1; an older reply exits 2
  ("ERROR codemodel 2.<n> < 2.9: linkLibraries required"); a reply with a
  missing version or a major other than 2 also exits 2. There is no
  weaker fallback mode.
Imported/INTERFACE targets come from 'abstractTargets'. External libraries
are matched by exact target name, or by normalized basename of raw link
fragments and imported artifacts (librocksdb.a, -lsqlite3 -> rocksdb, sqlite3).
"""
import argparse
import json
import os
import re
import sys
from collections import deque
from pathlib import Path

FOLLOW_PRIVATE = {"STATIC_LIBRARY", "OBJECT_LIBRARY"}


class InputError(Exception):
    pass


def load_json(path):
    try:
        return json.loads(Path(path).read_text())
    except (OSError, ValueError) as e:
        raise InputError("cannot read %s: %s" % (path, e))


def id_name(tid):
    # "RocksDB::rocksdb::@6890..." -> "RocksDB::rocksdb"
    return tid.rsplit("::@", 1)[0]


def lib_basename(s):
    """Normalize a link fragment / artifact to a bare library name."""
    s = s.strip()
    if s.startswith("-l"):
        return s[2:]
    if s.startswith("-"):
        return None  # other flags (-framework, -Wl,...)
    b = os.path.basename(s.rstrip("/"))
    for ext in (".framework", ".tbd", ".dylib", ".a", ".lib", ".dll"):
        if b.endswith(ext):
            b = b[: -len(ext)]
            break
    else:
        m = re.match(r"^(.*?)\.so(\.[0-9.]+)?$", b)
        if m:
            b = m.group(1)
    if b.startswith("lib") and len(b) > 3:
        b = b[3:]
    m = re.match(r"^(.*?)(\.[0-9]+)+$", b)  # libsqlite3.0 -> sqlite3
    if m:
        b = m.group(1)
    return b


class Model:
    def __init__(self, build, config):
        reply = Path(build) / ".cmake" / "api" / "v1" / "reply"
        indexes = sorted(reply.glob("index-*.json"))
        if not indexes:
            raise InputError("no index-*.json under %s" % reply)
        index = load_json(indexes[-1])
        cm_ref = index.get("reply", {}).get("codemodel-v2")
        if not cm_ref or "jsonFile" not in cm_ref:
            raise InputError("index %s has no codemodel-v2 reply" % indexes[-1])
        self.reply = reply
        cm = load_json(reply / cm_ref["jsonFile"])
        ver = cm.get("version")
        ver = ver if isinstance(ver, dict) else {}
        major, minor = ver.get("major"), ver.get("minor")
        if not isinstance(major, int) or not isinstance(minor, int) or major != 2:
            raise InputError("codemodel %s.%s unsupported: need 2.x with x >= 9"
                             % ("none" if major is None else major,
                                "none" if minor is None else minor))
        if minor < 9:
            raise InputError("codemodel 2.%d < 2.9: linkLibraries required" % minor)
        self.source_root = cm.get("paths", {}).get("source", "")
        configs = cm.get("configurations", [])
        if not configs:
            raise InputError("codemodel has no configurations")
        if config:
            configs = [c for c in configs if c.get("name") == config]
            if not configs:
                raise InputError("no configuration named %r" % config)
        cfg = configs[0]
        self.entries = {}
        for t in cfg.get("targets", []):
            self.entries[t["id"]] = (t, False)
        for t in cfg.get("abstractTargets", []):
            self.entries.setdefault(t["id"], (t, True))
        self._json = {}

    def tjson(self, tid):
        if tid not in self._json:
            self._json[tid] = load_json(self.reply / self.entries[tid][0]["jsonFile"])
        return self._json[tid]

    def find(self, name):
        hits = [tid for tid, (t, ab) in self.entries.items() if t["name"] == name and not ab]
        hits = hits or [tid for tid, (t, ab) in self.entries.items() if t["name"] == name]
        if len(hits) != 1:
            raise InputError("target %r: %d matches in codemodel" % (name, len(hits)))
        return hits[0]

    def name(self, tid):
        return self.entries[tid][0]["name"] if tid in self.entries else id_name(tid)

    def out_items(self, tid, is_root):
        """Return (target ids, raw fragments) followed out of tid."""
        tj = self.tjson(tid)
        if is_root or tj.get("type") in FOLLOW_PRIVATE:
            items = list(tj.get("linkLibraries", [])) + list(tj.get("interfaceLinkLibraries", []))
        else:
            items = list(tj.get("interfaceLinkLibraries", []))
        ids, frags = [], []
        for e in items:
            if "id" in e:
                if e["id"] not in ids:
                    ids.append(e["id"])
            elif "fragment" in e and e["fragment"] not in frags:
                frags.append(e["fragment"])
        if is_root:
            for f in (tj.get("link") or {}).get("commandFragments", []) or []:
                if f.get("role") == "libraries" and f["fragment"] not in frags:
                    frags.append(f["fragment"])
        return ids, frags

    def rel(self, p):
        root = self.source_root.rstrip("/") + "/"
        if self.source_root and p.startswith(root):
            return p[len(root):]
        return p


def walk(model, root):
    parent = {root: None}
    edges = {}
    frags = {}
    unresolved = []
    q = deque([root])
    while q:
        tid = q.popleft()
        ids, fr = model.out_items(tid, tid == root)
        frags[tid] = fr
        edges[tid] = []
        for d in ids:
            if d not in model.entries:
                unresolved.append((d, tid))
                continue
            edges[tid].append(d)
            if d not in parent:
                parent[d] = tid
                q.append(d)
    return parent, edges, frags, unresolved


def chain(model, parent, tid):
    out = []
    while tid is not None:
        out.append(model.name(tid))
        tid = parent[tid]
    return "->".join(reversed(out))


def cycles(model, edges):
    """One shortest cycle per non-trivial SCC of the closure graph (Tarjan)."""
    index, low, on, stack, sccs = {}, {}, set(), [], []
    counter = [0]

    def strong(v):
        # iterative Tarjan
        work = [(v, 0)]
        while work:
            node, i = work.pop()
            if i == 0:
                index[node] = low[node] = counter[0]
                counter[0] += 1
                stack.append(node)
                on.add(node)
            succ = edges.get(node, [])
            if i < len(succ):
                work.append((node, i + 1))
                w = succ[i]
                if w not in index:
                    work.append((w, 0))
                elif w in on:
                    low[node] = min(low[node], index[w])
                continue
            if low[node] == index[node]:
                comp = []
                while True:
                    w = stack.pop()
                    on.discard(w)
                    comp.append(w)
                    if w == node:
                        break
                sccs.append(comp)
            if work:
                p = work[-1][0]
                low[p] = min(low[p], low[node])

    for v in sorted(edges, key=model.name):
        if v not in index:
            strong(v)
    out = []
    for comp in sccs:
        members = set(comp)
        if len(comp) == 1 and comp[0] not in edges.get(comp[0], []):
            continue
        start = min(comp, key=model.name)
        prev = {start: None}
        q = deque([start])
        found = None
        while q and found is None:
            v = q.popleft()
            for w in sorted(edges.get(v, []), key=model.name):
                if w not in members:
                    continue
                if w == start:
                    found = v
                    break
                if w not in prev:
                    prev[w] = v
                    q.append(w)
        path = []
        v = found
        while v is not None:
            path.append(model.name(v))
            v = prev[v]
        path.reverse()
        out.append(("->".join(path + [model.name(start)]), sorted(model.name(c) for c in comp)))
    return out


def check(model, root_name, gate, allowlist):
    root = model.find(root_name)
    parent, edges, frags, unresolved = walk(model, root)
    allow_src = {a["path"] for a in allowlist}
    allowed = set(gate.get("allowed_targets", []))
    by_name, by_lib = {}, {}
    for r in gate.get("forbidden_targets", []):
        for n in r.get("names", []):
            by_name[n] = r["label"]
        for l in r.get("libs", []):
            by_lib[l] = r["label"]
    pats = [(p, re.compile(p)) for p in gate.get("include_patterns", [])]
    bad_files = set(gate.get("forbidden_source_files", []))
    bad_prefixes = gate.get("forbidden_source_prefixes", [])
    node_dirs = gate.get("node_source_dirs", [])

    v = set()
    for d, frm in unresolved:
        v.add("UNRESOLVED %s from=%s" % (d, model.name(frm)))
    for tid in parent:
        name = model.name(tid)
        tj = model.tjson(tid)
        for f in frags.get(tid, []):
            b = lib_basename(f)
            if b in by_lib:
                v.add("FORBIDDEN_TARGET %s rule=%s fragment-in=%s" % (f, by_lib[b], name))
        if name in allowed:
            continue
        label = by_name.get(name)
        if label is None:
            disk = [tj.get("nameOnDisk", "")] + [a.get("path", "") for a in tj.get("artifacts", [])]
            if tj.get("imported") or tj.get("abstract"):
                for d in disk:
                    if d and lib_basename(d) in by_lib:
                        label = by_lib[lib_basename(d)]
                        break
        if label is not None:
            parents = sorted({model.name(p) for p, ds in edges.items() if tid in ds})
            v.add("FORBIDDEN_TARGET %s rule=%s via=%s parents=%s"
                  % (name, label, chain(model, parent, tid), ",".join(parents)))
        srcs = [model.rel(s["path"]) for s in tj.get("sources", [])]
        for s in srcs:
            if s in bad_files:
                v.add("FORBIDDEN_TARGET %s rule=source:%s via=%s" % (name, s, chain(model, parent, tid)))
        for pre in bad_prefixes:
            hit = sorted(s for s in srcs if s.startswith(pre))
            if hit:
                v.add("FORBIDDEN_TARGET %s rule=sources-under:%s first=%s count=%d via=%s"
                      % (name, pre, hit[0], len(hit), chain(model, parent, tid)))
        for s in srcs:
            if any(s.startswith(d) for d in node_dirs) and s not in allow_src:
                v.add("NODE_SOURCE %s target=%s" % (s, name))
        seen = set()
        for cg in tj.get("compileGroups", []):
            for inc in cg.get("includes", []):
                p = inc["path"].rstrip("/")
                for pat, rx in pats:
                    if rx.search(p) and (p, pat) not in seen:
                        seen.add((p, pat))
                        v.add("FORBIDDEN_INCLUDE %s %s pattern=%s" % (name, p, pat))
    cyc = cycles(model, edges)
    notes = ["# closure of %s: %d targets" % (root_name, len(parent))]
    for c, members in cyc:
        v.add("CYCLE %s" % c)
        notes.append("# SCC containing %s: %s" % (c.split("->")[0], ",".join(members)))
    return sorted(v), notes



def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--build", required=True)
    ap.add_argument("--target", required=True)
    ap.add_argument("--rules", required=True)
    ap.add_argument("--gate", required=True, choices=["K2", "W2"])
    ap.add_argument("--config", default=None)
    a = ap.parse_args(argv)
    try:
        rules = load_json(a.rules)
        gate = rules["gates"][a.gate]
        model = Model(a.build, a.config)
        violations, notes = check(model, a.target, gate, rules.get("source_allowlist", []))
    except (InputError, KeyError) as e:
        print("ERROR %s" % e, file=sys.stderr)
        return 2
    for n in notes:
        print(n)
    for line in violations:
        print(line)
    print("# %s %s: %d violation(s)" % (a.gate, a.target, len(violations)))
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main())
