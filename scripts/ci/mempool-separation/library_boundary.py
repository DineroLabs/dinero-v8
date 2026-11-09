#!/usr/bin/env python3
"""Check the real CMake graph, source owners and node/library stub boundary.

Default is strict: known legacy-shielded storage debt is printed and returns 1.
--owned-only checks this extraction's boundary while still reporting that debt;
it must never be presented as a fully storage-free result.
"""
import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "separation"))
from check_link_closure import InputError, Model, cycles, lib_basename, walk

SOURCES = {
    "src/daemon/mempool.cpp", "src/daemon/relay_transaction_reader.cpp",
    "src/mempool/coins_view_mempool.cpp", "src/mempool/mempool_persistence.cpp",
    "src/mempool/fee_estimator.cpp", "src/mempool/invalid_tx_cache.cpp",
    "src/mempool/tx_orphan_pool.cpp", "src/policy/mempool_policy.cpp",
    "src/policy/rbf_policy.cpp", "src/policy/fee_estimator.cpp",
    "src/mining/ct_selection_policy.cpp", "src/mining/address_validator.cpp",
}
FORBIDDEN = {"dinero_core", "dinero_chainstate", "dinero_wallet", "dinero_rpc_handlers",
             "RocksDB::rocksdb", "rocksdb", "dinero_p2p", "dinero_daemon", "grpc", "grpc++"}
LEGACY_WALLET = {
    "src/wallet/shielded_derivation.cpp", "src/wallet/shielded_note_store.cpp",
    "src/wallet/shielded_wallet_ops.cpp", "src/wallet/private_covenant_descriptor.cpp",
    "src/wallet/shielded_outgoing_view.cpp", "src/wallet/wallet_routing_engine.cpp",
    "src/wallet/bip32_deriver.cpp",
}
PURE_CODECS = {"src/wallet/p2mr_address.cpp", "src/daemon/bech32_decode.cpp",
               "src/daemon/bech32_encoder.cpp", "src/daemon/orchard_mempool_transaction.cpp"}


def orchard_reader_define_violations(commands, orchard_enabled):
    """The moved reader must retain its ON-only historical-body decoder."""
    if not orchard_enabled:
        return []
    reader = [entry for entry in commands
              if entry["file"].replace("\\", "/").endswith(
                  "/src/daemon/relay_transaction_reader.cpp")]
    define = re.compile(
        r"(?:^|\s)(?:-D|/D)DINERO_HAS_ORCHARD_RELAY_READER=1(?:\s|$)")
    if len(reader) != 1 or not define.search(reader[0].get("command", "")):
        return ["MISSING_ORCHARD_RELAY_READER_DEFINE"]
    return []


def compile_owner(command):
    text = (command.get("output", "") + " " + command.get("command", "")
            ).replace("\\", "/")
    match = re.search(r"CMakeFiles/([^/]+)\.dir", text)
    return match[1] if match else "unresolved"


def check(build, skip_source_owners=False):
    model = Model(build, None)
    root = model.find("dinero_mempool")
    closure, edges, fragments, unresolved = walk(model, root)
    violations = ["UNRESOLVED %s from=%s" % (tid, model.name(parent))
                  for tid, parent in unresolved]
    debt = []
    inherited = set()
    if any(model.name(t) == "dinero_shielded" for t in closure):
        inherited = set(walk(model, model.find("dinero_shielded"))[0])
    for tid in closure:
        name = model.name(tid)
        target = model.tjson(tid)
        if name in FORBIDDEN:
            violations.append("FORBIDDEN_TARGET " + name)
        if name in {"sqlite3", "SQLite::SQLite3"}:
            (debt if tid in inherited else violations).append("STORAGE_TARGET " + name)
        for fragment in fragments.get(tid, []):
            library = lib_basename(fragment)
            if library == "rocksdb":
                violations.append("FORBIDDEN_LIBRARY rocksdb from=" + name)
            if library == "sqlite3":
                (debt if tid in inherited else violations).append("STORAGE_LIBRARY sqlite3 from=" + name)
        for source in target.get("sources", []):
            path = model.rel(source.get("path", ""))
            if path in LEGACY_WALLET and tid in inherited:
                debt.append("LEGACY_WALLET_SOURCE " + path)
            elif path.startswith(("src/wallet/", "src/storage/", "src/p2p/", "src/rpc/")):
                if path not in PURE_CODECS:
                    violations.append("FORBIDDEN_SOURCE %s target=%s" % (path, name))
            elif path.startswith("src/daemon/") and path not in SOURCES | PURE_CODECS:
                violations.append("FORBIDDEN_SOURCE %s target=%s" % (path, name))
            if path.endswith("tests/stubs/mempool_test_stubs.cpp"):
                violations.append("STUB_IN_MEMPOOL_CLOSURE " + name)
        for group in target.get("compileGroups", []):
            for include in group.get("includes", []):
                directory = include.get("path", "").rstrip("/")
                if "rocksdb" in directory.lower() or directory.endswith("/src/daemon"):
                    violations.append("FORBIDDEN_INCLUDE %s %s" % (name, directory))
    for cycle in cycles(model, edges):
        violations.append("CYCLE " + str(cycle))
    for name in ["dinero_wallet", "dinero_consensus", "dinero_consensus_core"]:
        ids = [tid for tid in model.entries if model.name(tid) == name]
        if ids and root in walk(model, model.find(name))[0]:
            violations.append("REVERSE_EDGE " + name)
    for tid, (_, abstract) in model.entries.items():
        if abstract:
            continue
        target = model.tjson(tid)
        if any(s.get("path", "").endswith("tests/stubs/mempool_test_stubs.cpp")
               for s in target.get("sources", [])):
            if root in walk(model, tid)[0]:
                violations.append("STUB_WITH_REAL_MEMPOOL " + model.name(tid))
    owners = {}
    if not skip_source_owners:
        path = Path(build) / "compile_commands.json"
        try:
            commands = json.loads(path.read_text())
        except (OSError, ValueError) as e:
            raise InputError("cannot read compile commands: " + str(e))
        try:
            cache = (Path(build) / "CMakeCache.txt").read_text()
        except OSError as e:
            raise InputError("cannot read CMake cache: " + str(e))
        orchard_enabled = bool(re.search(
            r"^DINERO_BUILD_ORCHARD_BACKEND:(?:BOOL|UNINITIALIZED)=ON$",
            cache, re.MULTILINE))
        violations.extend(orchard_reader_define_violations(commands, orchard_enabled))
        for command in commands:
            source = model.rel(command["file"])
            if source not in SOURCES:
                continue
            owners.setdefault(source, []).append(compile_owner(command))
        for source in sorted(SOURCES):
            if owners.get(source) != ["dinero_mempool"]:
                violations.append("SOURCE_OWNER %s %s" % (source, owners.get(source, [])))
    violations = sorted(set(violations))
    debt = sorted(set(debt))
    return {"owned_boundary": "FAIL" if violations else "PASS",
            "status": "FAIL" if violations else "INHERITED_DEBT" if debt else "PASS",
            "violations": violations, "inherited_debt": debt,
            "source_owners": owners,
            "closure": sorted(model.name(t) for t in closure)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--owned-only", action="store_true")
    parser.add_argument("--skip-source-owner-check", action="store_true",
                        help="controlled graph-fixture tests only; not release qualification")
    args = parser.parse_args()
    try:
        report = check(args.build, args.skip_source_owner_check)
    except InputError as e:
        print("ERROR " + str(e), file=sys.stderr)
        return 2
    print(json.dumps(report, indent=2))
    return 1 if report["violations"] or (report["inherited_debt"] and not args.owned_only) else 0


if __name__ == "__main__":
    raise SystemExit(main())
