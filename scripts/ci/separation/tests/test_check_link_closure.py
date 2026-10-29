import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE.parent / "check_link_closure.py"
RULES = HERE.parent / "closure_rules.json"
FIXTURES = HERE / "fixtures"


def _id(name):
    return name + "::@deadbeef"


def _entries(items):
    out = []
    for it in items:
        if isinstance(it, dict):
            out.append({"backtrace": 1, "fragment": it["fragment"]})
        else:
            out.append({"backtrace": 1, "id": _id(it)})
    return out


def materialize(fixture, build):
    """Write a CMake File API codemodel-v2 reply (real shapes) for a compact fixture spec."""
    spec = json.loads((FIXTURES / ("codemodel_" + fixture + ".json")).read_text())
    reply = build / ".cmake" / "api" / "v1" / "reply"
    reply.mkdir(parents=True)

    def write_target(name, t, abstract):
        fname = "target-" + name.replace(":", "_") + "-Release-0.json"
        tj = {
            "codemodelVersion": {"major": 2, "minor": 10},
            "id": _id(name),
            "name": name,
            "type": t["type"],
            "paths": {"build": ".", "source": "."},
            "sources": [{"path": p, "compileGroupIndex": 0} for p in t.get("sources", [])],
        }
        if abstract:
            tj["abstract"] = True
        if t.get("imported"):
            tj["imported"] = True
        if "nameOnDisk" in t:
            tj["nameOnDisk"] = t["nameOnDisk"]
        if "artifacts" in t:
            tj["artifacts"] = [{"path": p} for p in t["artifacts"]]
        if t.get("sources") or t.get("includes"):
            tj["compileGroups"] = [{
                "language": "CXX",
                "includes": [{"backtrace": 1, "path": p} for p in t.get("includes", [])],
                "sourceIndexes": list(range(len(t.get("sources", [])))),
            }]
        if "link" in t:
            tj["linkLibraries"] = _entries(t["link"])
            deps = [e for e in _entries(t["link"]) if "id" in e]
            if deps:
                tj["dependencies"] = deps
        if "deps" in t:
            tj["dependencies"] = [e for e in _entries(t["deps"])]
        if "ilink" in t:
            tj["interfaceLinkLibraries"] = _entries(t["ilink"])
        if t["type"] == "STATIC_LIBRARY" and not abstract:
            tj["archive"] = {}
        (reply / fname).write_text(json.dumps(tj))
        return {"id": _id(name), "name": name, "jsonFile": fname,
                "directoryIndex": 0, "projectIndex": 0}

    minor = spec.get("codemodel_minor", 10)
    targets = [write_target(n, t, False) for n, t in spec["targets"].items()]
    abstract = [write_target(n, t, True) for n, t in spec.get("abstract", {}).items()]
    cm = {
        "kind": "codemodel",
        "version": {"major": 2, "minor": minor},
        "paths": {"source": "/repo", "build": str(build)},
        "configurations": [{"name": "Release", "targets": targets,
                            "abstractTargets": abstract, "directories": [], "projects": []}],
    }
    (reply / "codemodel-v2-0.json").write_text(json.dumps(cm))
    index = {"reply": {"codemodel-v2": {"jsonFile": "codemodel-v2-0.json", "kind": "codemodel",
                                         "version": {"major": 2, "minor": minor}}}}
    (reply / "index-2026-01-01T00-00-00-0000.json").write_text(json.dumps(index))
    return build


def run(tmp_path, fixture, gate="K2", target="root"):
    build = materialize(fixture, tmp_path / "build")
    return subprocess.run([sys.executable, str(SCRIPT), "--build", str(build), "--target", target,
                           "--rules", str(RULES), "--gate", gate],
                          capture_output=True, text=True)


def kinds(r, kind):
    return [l for l in r.stdout.splitlines() if l.startswith(kind + " ")]


# --- brief Step 1 tests -------------------------------------------------------

def test_private_static_link_propagates(tmp_path):
    r = run(tmp_path, "private_static")
    assert r.returncode == 1, r.stdout + r.stderr
    ft = kinds(r, "FORBIDDEN_TARGET")
    assert any(l.split()[1] == "sqlite3" for l in ft), r.stdout


def test_cycle_in_closure_reported(tmp_path):
    r = run(tmp_path, "cycle")
    assert r.returncode == 1, r.stdout + r.stderr
    assert "CYCLE A->B->A" in r.stdout.splitlines(), r.stdout


def test_cycle_outside_closure_ignored(tmp_path):
    r = run(tmp_path, "cycle_outside")
    assert r.returncode == 0, r.stdout + r.stderr
    assert not kinds(r, "CYCLE")


def test_allowlisted_node_source_passes(tmp_path):
    r = run(tmp_path, "allowlisted")
    assert r.returncode == 0, r.stdout + r.stderr
    assert not kinds(r, "NODE_SOURCE")


def test_unlisted_src_rpc_source_fails(tmp_path):
    r = run(tmp_path, "unlisted_rpc")
    assert r.returncode == 1, r.stdout + r.stderr
    ns = kinds(r, "NODE_SOURCE")
    assert any(l.split()[1] == "src/rpc/v7_pq_handlers.cpp" for l in ns), r.stdout


# --- extra coverage for decisions the brief leaves implicit -------------------

def test_shared_private_dep_not_followed(tmp_path):
    r = run(tmp_path, "shared_private")
    assert r.returncode == 0, r.stdout + r.stderr


def test_externals_matched_by_name_and_basename(tmp_path):
    r = run(tmp_path, "externals", gate="K2")
    assert r.returncode == 1, r.stdout + r.stderr
    ft = [l.split()[1] for l in kinds(r, "FORBIDDEN_TARGET")]
    assert "RocksDB::rocksdb" in ft, r.stdout          # imported abstract target
    assert "-lsqlite3" in ft, r.stdout                 # raw link fragment
    assert "/opt/x/_deps/rocksdb-install/lib/librocksdb.a" in ft, r.stdout
    assert "pthread" not in ft and "lightning_core_static" not in ft, r.stdout
    inc = [l.split()[2] for l in kinds(r, "FORBIDDEN_INCLUDE")]
    assert "/repo/third_party/rocksdb/include" in inc, r.stdout
    assert "/repo/src/daemon" in inc, r.stdout
    assert "/repo/src/daemon/sub" not in inc, r.stdout  # pattern is anchored: /src/daemon$


def test_w2_allows_sqlite3_and_lightning(tmp_path):
    r = run(tmp_path, "externals", gate="W2")
    assert r.returncode == 1, r.stdout + r.stderr       # rocksdb still forbidden
    ft = [l.split()[1] for l in kinds(r, "FORBIDDEN_TARGET")]
    assert "-lsqlite3" not in ft, r.stdout
    assert "RocksDB::rocksdb" in ft, r.stdout


def test_unresolved_dependency_fails_closed(tmp_path):
    r = run(tmp_path, "unresolved")
    assert r.returncode != 0, r.stdout + r.stderr
    assert "UNRESOLVED" in r.stdout, r.stdout


def test_missing_root_fails_closed(tmp_path):
    r = run(tmp_path, "cycle", target="nope")
    assert r.returncode == 2, r.stdout + r.stderr


def test_missing_reply_fails_closed(tmp_path):
    r = subprocess.run([sys.executable, str(SCRIPT), "--build", str(tmp_path), "--target", "x",
                        "--rules", str(RULES), "--gate", "K2"], capture_output=True, text=True)
    assert r.returncode == 2, r.stdout + r.stderr


def test_legacy_codemodel_fails_closed(tmp_path):
    r = run(tmp_path, "legacy")
    assert r.returncode == 2, r.stdout + r.stderr
    assert "ERROR codemodel 2.8 < 2.9: linkLibraries required" in r.stderr, r.stderr
