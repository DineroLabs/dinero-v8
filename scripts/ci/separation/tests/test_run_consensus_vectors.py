import json
import os
import stat
import subprocess
import sys
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent.parent / "run_consensus_vectors.py"
D1 = "a" * 64
D2 = "b" * 64

# Fake ctest: reads FAKE_CTEST_SPEC (json: name -> {status, out}); supports
# --show-only=json-v1 and a run with -R ^(..)$ --output-junit FILE.
FAKE = r'''#!/usr/bin/env python3
import json, os, re, sys
from xml.sax.saxutils import escape
spec = json.load(open(os.environ["FAKE_CTEST_SPEC"]))
a = sys.argv[1:]
with open(os.environ["FAKE_CTEST_LOG"], "a") as f:
    f.write(" ".join(a) + "\n")
if "--show-only=json-v1" in a:
    print(json.dumps({"tests": [{"name": n} for n in spec]}))
    sys.exit(0)
rx = re.compile(a[a.index("-R") + 1])
assert "--no-tests=error" in a
out = a[a.index("--output-junit") + 1]
cases = []
for n, s in spec.items():
    if not rx.search(n):
        continue
    st = s["status"]
    body = ""
    if st == "fail":
        body = "<failure message='Failed'/>"
    cases.append('<testcase name="%s" status="%s">%s<system-out>%s</system-out></testcase>'
                 % (n, "run" if st == "pass" else st, body, escape(s.get("out", ""))))
open(out, "w").write("<testsuite>" + "".join(cases) + "</testsuite>")
sys.exit(0 if all(spec[n]["status"] == "pass" for n in spec if rx.search(n)) else 8)
'''


def setup(tmp_path, tests, listed, floor=(), config="off"):
    fake = tmp_path / "ctest"
    fake.write_text(FAKE)
    fake.chmod(fake.stat().st_mode | stat.S_IEXEC)
    spec = tmp_path / "spec.json"
    spec.write_text(json.dumps(tests))
    lst = tmp_path / "list.txt"
    lst.write_text("# c\n" + "\n".join(listed) + "\n")
    fl = tmp_path / "floor.txt"
    fl.write_text("# c\n" + "\n".join(floor) + "\n")
    build = tmp_path / "build"
    build.mkdir(exist_ok=True)
    (build / "version_config.h").write_text(
        '#define DINERO_GIT_COMMIT_FULL "' + "c" * 40 + '"\n')
    env = dict(os.environ, FAKE_CTEST_SPEC=str(spec),
               FAKE_CTEST_LOG=str(tmp_path / "ctest.log"))
    return fake, lst, fl, build, env


def run(tmp_path, tests, listed, mode, floor=(), config="off", ref=None):
    fake, lst, fl, build, env = setup(tmp_path, tests, listed, floor, config)
    out = tmp_path / "ref.json"
    args = [sys.executable, str(SCRIPT), "--build", build, "--list", lst,
            "--floor", fl, "--ctest", fake, "--config", config]
    args += ["--record", out] if mode == "record" else ["--compare", ref or out]
    r = subprocess.run(list(map(str, args)), capture_output=True, text=True, env=env)
    return r, out, tmp_path / "ctest.log"


def P(out=""):
    return {"status": "pass", "out": out}


def test_refuses_floor_test(tmp_path):
    r, _, log = run(tmp_path, {"A": P(), "B": P()}, ["A", "B"], "record", floor=["B"])
    assert r.returncode == 1, r.stdout + r.stderr
    assert "ON_FLOOR B" in r.stdout
    assert not log.exists(), "ctest must not be invoked when a listed test is on the floor"


def test_missing_test_fails(tmp_path):
    r, _, _ = run(tmp_path, {"A": P()}, ["A", "B"], "record")
    assert r.returncode == 1, r.stdout + r.stderr
    assert "NOT_RUN B" in r.stdout


def test_notrun_status_fails(tmp_path):
    r, _, _ = run(tmp_path, {"A": P(), "B": {"status": "notrun"}}, ["A", "B"], "record")
    assert r.returncode == 1, r.stdout + r.stderr
    assert "NOT_RUN B" in r.stdout


def test_failed_test_fails(tmp_path):
    r, _, _ = run(tmp_path, {"A": P(), "B": {"status": "fail"}}, ["A", "B"], "record")
    assert r.returncode == 1, r.stdout + r.stderr
    assert "FAILED B" in r.stdout


def test_record_then_compare_passes_and_flags_digests(tmp_path):
    tests = {"A": P("x " + D1 + " y " + D2), "B": P("no digests here")}
    r, out, log = run(tmp_path, tests, ["A", "B"], "record")
    assert r.returncode == 0, r.stdout + r.stderr
    ref = json.loads(out.read_text())
    assert ref["provenance_sha"] == "c" * 40
    assert ref["tests"]["A"]["emitted_digests"] is True
    assert ref["tests"]["A"]["digests"] == sorted([D1, D2])
    assert ref["tests"]["B"]["emitted_digests"] is False
    # the run regex is exact-anchored and --no-tests=error is always passed
    runs = [l for l in log.read_text().splitlines() if "--output-junit" in l]
    assert "-R ^(A|B)$" in runs[0] and "--no-tests=error" in runs[0]
    r2, _, _ = run(tmp_path, tests, ["A", "B"], "compare", ref=out)
    assert r2.returncode == 0, r2.stdout + r2.stderr


def test_digest_mismatch_fails(tmp_path):
    r, out, _ = run(tmp_path, {"A": P("d " + D1)}, ["A"], "record")
    assert r.returncode == 0, r.stdout + r.stderr
    ref = tmp_path / "keep_ref.json"
    ref.write_text(out.read_text())
    r2, _, _ = run(tmp_path, {"A": P("d " + D2)}, ["A"], "compare", ref=ref)
    assert r2.returncode == 1, r2.stdout + r2.stderr
    assert "DIGEST_MISMATCH A" in r2.stdout


def test_names_are_escaped_and_exact(tmp_path):
    tests = {"A.b": P(), "AxB": P()}
    r, _, log = run(tmp_path, tests, ["A.b"], "record")
    assert r.returncode == 0, r.stdout + r.stderr
    assert r"-R ^(A\.b)$" in log.read_text()


def test_on_prefix_only_in_on_config(tmp_path):
    tests = {"A": P(), "OnlyOn": P()}
    r, out, _ = run(tmp_path, tests, ["A", "on:OnlyOn"], "record", config="off")
    assert r.returncode == 0, r.stdout + r.stderr
    assert sorted(json.loads(out.read_text())["tests"]) == ["A"]
    r, out, _ = run(tmp_path, tests, ["A", "on:OnlyOn"], "record", config="on")
    assert r.returncode == 0, r.stdout + r.stderr
    assert sorted(json.loads(out.read_text())["tests"]) == ["A", "OnlyOn"]


def test_on_prefixed_floor_entry_refused_even_in_off(tmp_path):
    r, _, _ = run(tmp_path, {"A": P()}, ["A", "on:F"], "record", floor=["F"], config="off")
    assert r.returncode == 1 and "ON_FLOOR F" in r.stdout
