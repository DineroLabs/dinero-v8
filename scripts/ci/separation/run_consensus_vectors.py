#!/usr/bin/env python3
"""K4 consensus-vector gate.

Runs the exact-named ctest tests in --list against a build dir, then either
records (--record) or compares (--compare) per-test pass/ran status and the set
of 64-hex-char digests each test printed.  Exits 1 if any listed test is on the
never-run floor (refused before anything runs), did not run, failed, or has a
digest set that differs from the reference.  Stdlib only.
"""
import argparse
import json
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

HEX64 = re.compile(r"(?<![0-9A-Fa-f])[0-9A-Fa-f]{64}(?![0-9A-Fa-f])")


def read_names(path):
    out = []
    for line in Path(path).read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            out.append(line)
    return out


def detect_config(build):
    cache = Path(build) / "CMakeCache.txt"
    if cache.is_file():
        for line in cache.read_text(errors="replace").splitlines():
            if line.startswith("DINERO_BUILD_ORCHARD_BACKEND:"):
                return "on" if line.split("=", 1)[1].strip().upper() in ("ON", "1", "TRUE") else "off"
        return "off"
    return None


def provenance(build):
    vc = Path(build) / "version_config.h"
    if vc.is_file():
        m = re.search(r'DINERO_GIT_COMMIT_FULL\s+"([0-9a-f]+)"', vc.read_text())
        if m:
            return m.group(1)
    return "unknown"


def fail(msg):
    print(msg)
    return 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--list", required=True)
    ap.add_argument("--floor", required=True)
    ap.add_argument("--ctest", default="ctest")
    ap.add_argument("--config", choices=("off", "on"))
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--record")
    g.add_argument("--compare")
    a = ap.parse_args()

    config = a.config or detect_config(a.build)
    if config is None:
        print("ERROR cannot determine config (no CMakeCache.txt); pass --config")
        return 1

    entries = read_names(a.list)
    floor = set(read_names(a.floor))
    bad = [e[3:] if e.startswith("on:") else e for e in entries
           if (e[3:] if e.startswith("on:") else e) in floor]
    if bad:
        for n in bad:
            print("ON_FLOOR " + n)
        return 1

    names = []
    for e in entries:
        if e.startswith("on:"):
            if config != "on":
                continue
            e = e[3:]
        if e not in names:
            names.append(e)
    if not names:
        return fail("ERROR empty test list")

    p = subprocess.run([a.ctest, "--test-dir", a.build, "--show-only=json-v1"],
                       capture_output=True, text=True)
    try:
        registered = {t["name"] for t in json.loads(p.stdout)["tests"]}
    except Exception as exc:
        return fail("ERROR cannot read ctest test list: %r\n%s" % (exc, p.stderr))
    missing = [n for n in names if n not in registered]
    if missing:
        for n in missing:
            print("NOT_RUN " + n)
        return 1

    regex = "^(" + "|".join(re.escape(n) for n in names) + ")$"
    with tempfile.TemporaryDirectory() as td:
        junit = Path(td) / "junit.xml"
        p = subprocess.run([a.ctest, "--test-dir", a.build, "-R", regex,
                            "--no-tests=error", "--output-junit", str(junit)],
                           capture_output=True, text=True)
        if not junit.is_file():
            return fail("ERROR ctest produced no junit output (rc=%d)\n%s%s"
                        % (p.returncode, p.stdout[-2000:], p.stderr[-2000:]))
        root = ET.parse(junit).getroot()

    results = {}
    problems = 0
    for tc in root.iter("testcase"):
        n = tc.get("name")
        out = (tc.findtext("system-out") or "")
        status = tc.get("status")
        if status not in ("run", "fail"):
            print("NOT_RUN " + n)
            problems += 1
            continue
        if (status == "fail" or tc.find("failure") is not None
                or tc.find("error") is not None):
            print("FAILED " + n)
            problems += 1
            continue
        digests = sorted({d.lower() for d in HEX64.findall(out)})
        results[n] = {"status": "passed", "emitted_digests": bool(digests), "digests": digests}
    for n in names:
        if n not in results and not any(tc.get("name") == n for tc in root.iter("testcase")):
            print("NOT_RUN " + n)
            problems += 1
    if problems:
        return 1

    if a.record:
        ref = {"config": config, "build_dir": str(a.build),
               "provenance_sha": provenance(a.build), "tests": results}
        Path(a.record).write_text(json.dumps(ref, indent=2, sort_keys=True) + "\n")
        print("RECORDED %d tests (%d emit digests) -> %s" %
              (len(results), sum(r["emitted_digests"] for r in results.values()), a.record))
        return 0

    ref = json.loads(Path(a.compare).read_text())
    if ref.get("config") != config:
        return fail("CONFIG_MISMATCH reference=%s run=%s" % (ref.get("config"), config))
    rc = 0
    for n in sorted(set(ref["tests"]) | set(results)):
        r, c = ref["tests"].get(n), results.get(n)
        if r is None or c is None:
            print("DIGEST_MISMATCH %s (test set differs from reference)" % n)
            rc = 1
        elif r["digests"] != c["digests"]:
            print("DIGEST_MISMATCH %s" % n)
            rc = 1
    if rc == 0:
        print("OK %d tests match reference (provenance %s)" % (len(results), ref.get("provenance_sha")))
    return rc


if __name__ == "__main__":
    sys.exit(main())
