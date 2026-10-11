#!/usr/bin/env python3
"""Automatic shielded-state storage upgrade at startup: crash points and refusal.

A legacy store with a few blocks is built once. Then, on fresh copies:
  - for each durable upgrade step, the daemon is killed right after it; the next
    ordinary start must finish the upgrade with the same tip, keep the original
    as chaindb.legacy-v1 and leave no candidate or journal;
  - with (pretended) too little free disk, the upgrade is refused and the node
    starts on its legacy store, unchanged;
  - an upgraded store is left alone on the next start;
  - a plain reindex below activation keeps the separated layout, so the
    activation block can still be mined.
Regtest, isolated temporary directories. Usage: CYCLE_DAEMON MIGRATOR
"""
import os
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_orchard_daemon_wallet_cycle import Node  # noqa: E402

PHASES = ("copied", "migrated", "legacy-retired", "swapped")


def layout(datadir):
    chaindb = Path(datadir) / "blockchain" / "chaindb"
    options = sorted(chaindb.glob("OPTIONS-*"))
    separated = bool(options) and "shielded_state_v1" in options[-1].read_text(errors="replace")
    leftovers = [p.name for p in chaindb.parent.iterdir()
                 if p.name.endswith((".upgrade-candidate", ".upgrade-state", ".upgrade-state.tmp"))]
    return {"separated": separated, "legacy_kept": (chaindb.parent / "chaindb.legacy-v1").is_dir(),
            "leftovers": leftovers}


def crash_start(node, phase):
    """Start the daemon with a crash right after `phase`; it must exit 17."""
    env = {**os.environ, "DINERO_TEST_AUTOMATIC_STORAGE_UPGRADE": "1",
           "DINERO_TEST_STORAGE_UPGRADE_CRASH_AFTER": phase}
    node.starts += 1
    ports = node.ports
    args = [node.binary, "--regtest", f"--datadir={node.datadir}", f"--conf={node.config}",
            f"--rpcport={ports[0]}", f"--port={ports[1]}", f"--wallet-socket-port={ports[2]}",
            "--listen=0", "--p2p.offline=1", "--utreexo=1", "--utreexo-bridge=1"]
    log = node.root / f"process-{node.starts}-crash-{phase}.log"
    with log.open("wb") as out:
        code = subprocess.run(args, stdin=subprocess.DEVNULL, stdout=out, stderr=out, env=env, timeout=300).returncode
    text = log.read_text(errors="replace")
    assert code == 17 and f"CRASH after storage upgrade step {phase}" in text, \
        f"expected a crash after {phase}, got exit {code}; see {log}"


def main():
    assert len(sys.argv) == 3
    os.environ.pop("DINERO_TEST_AUTOMATIC_STORAGE_UPGRADE", None)
    base = Node(sys.argv[1], sys.argv[2])
    print(f"Private isolated evidence: {base.root}", flush=True)
    base.start()
    assert base.rpc("wallet.createhd", [base.wallet, 12, "", base.password])["success"] is True
    assert base.rpc("wallet.unlock", [base.password, 3600])["success"] is True
    address = base.rpc("wallet.getnewaddress", ["taproot", "upgrade fixture"])["address"]
    base.generate(5, address)
    tip, height = base.rpc("getbestblockhash"), base.rpc("getblockcount")
    base.stop()
    fixture = base.root / "fixture"
    shutil.copytree(base.datadir, fixture)
    assert layout(fixture) == {"separated": False, "legacy_kept": False, "leftovers": []}
    print(f"PASS legacy fixture at height {height}", flush=True)

    def fresh_copy(name):
        node = Node(sys.argv[1], sys.argv[2])
        shutil.rmtree(node.datadir)
        shutil.copytree(fixture, node.datadir)
        node.pow_profile = None
        holders = __import__("socket")
        socks = [holders.socket() for _ in range(3)]
        for s in socks:
            s.bind(("127.0.0.1", 0))
        node.ports = [s.getsockname()[1] for s in socks]
        for s in socks:
            s.close()
        return node

    os.environ["DINERO_TEST_AUTOMATIC_STORAGE_UPGRADE"] = "1"
    for phase in PHASES:
        node = fresh_copy(phase)
        crash_start(node, phase)
        node.start()  # ordinary start, automatic upgrade on
        assert node.rpc("getbestblockhash") == tip and node.rpc("getblockcount") == height
        node.stop()
        got = layout(node.datadir)
        assert got == {"separated": True, "legacy_kept": True, "leftovers": []}, f"after crash at {phase}: {got}"
        print(f"PASS crash after '{phase}' recovered on the next start (tip kept, legacy kept)", flush=True)

    # A missing candidate after the old live directory was retired is damage,
    # not a fresh install. Startup must leave the rollback copy intact and must
    # not let ChainDB::init create an empty database at the missing live name.
    damaged = fresh_copy("missing-candidate")
    crash_start(damaged, "legacy-retired")
    chaindb = damaged.datadir / "blockchain" / "chaindb"
    candidate = damaged.datadir / "blockchain" / "chaindb.upgrade-candidate"
    legacy = damaged.datadir / "blockchain" / "chaindb.legacy-v1"
    assert not chaindb.exists() and candidate.is_dir() and legacy.is_dir()
    shutil.rmtree(candidate)
    def rollback_files():
        return sorted((p.relative_to(legacy), hashlib.sha256(p.read_bytes()).hexdigest())
                      for p in legacy.rglob("*") if p.is_file())
    before = rollback_files()
    args = [damaged.binary, "--regtest", f"--datadir={damaged.datadir}", f"--conf={damaged.config}",
            f"--rpcport={damaged.ports[0]}", f"--port={damaged.ports[1]}",
            f"--wallet-socket-port={damaged.ports[2]}", "--listen=0", "--p2p.offline=1",
            "--utreexo=1", "--utreexo-bridge=1"]
    refusal = subprocess.run(args, stdin=subprocess.DEVNULL, capture_output=True,
                             env=os.environ.copy(), timeout=30)
    assert refusal.returncode != 0 and b"refusing to create ChainDB" in refusal.stdout + refusal.stderr
    assert not chaindb.exists() and legacy.is_dir() and rollback_files() == before
    print("PASS missing upgrade candidate refuses startup without creating ChainDB", flush=True)

    # The refusal must not depend on whether this process will perform an
    # Orchard upgrade. Either mode would otherwise let ChainDB::init create
    # an empty live store over the surviving rollback copy.
    for name, auto_upgrade, stateless in (("upgrade-off", False, False),
                                          ("stateless", True, True)):
        variant = fresh_copy(name)
        shutil.rmtree(variant.datadir)
        shutil.copytree(damaged.datadir, variant.datadir)
        variant_chaindb = variant.datadir / "blockchain" / "chaindb"
        variant_legacy = variant.datadir / "blockchain" / "chaindb.legacy-v1"
        variant_before = sorted((p.relative_to(variant_legacy), hashlib.sha256(p.read_bytes()).hexdigest())
                                for p in variant_legacy.rglob("*") if p.is_file())
        variant_env = os.environ.copy()
        if not auto_upgrade:
            variant_env.pop("DINERO_TEST_AUTOMATIC_STORAGE_UPGRADE", None)
        variant_args = [variant.binary, "--regtest", f"--datadir={variant.datadir}",
                        f"--conf={variant.config}", f"--rpcport={variant.ports[0]}",
                        f"--port={variant.ports[1]}", f"--wallet-socket-port={variant.ports[2]}",
                        "--listen=0", "--p2p.offline=1", "--utreexo=1",
                        "--utreexo-stateless=1" if stateless else "--utreexo-bridge=1"]
        variant_result = subprocess.run(variant_args, stdin=subprocess.DEVNULL, capture_output=True,
                                        env=variant_env, timeout=30)
        assert variant_result.returncode != 0 and b"refusing to create ChainDB" in (
            variant_result.stdout + variant_result.stderr), f"{name}: missing swap refusal"
        variant_after = sorted((p.relative_to(variant_legacy), hashlib.sha256(p.read_bytes()).hexdigest())
                               for p in variant_legacy.rglob("*") if p.is_file())
        assert not variant_chaindb.exists() and variant_legacy.is_dir() and variant_after == variant_before
        print(f"PASS missing upgrade candidate refuses with {name}", flush=True)

    # An upgraded store is left alone.
    node.start()
    node.stop()
    log = (node.root / f"process-{node.starts}.log").read_text(errors="replace")
    assert "Shielded-state storage upgraded" not in log and layout(node.datadir)["separated"]
    print("PASS an upgraded store is not upgraded again", flush=True)

    # A plain reindex below activation must not fall back to the legacy layout:
    # the node still mines the Orchard activation block afterwards.
    node.start(["--reindex"])
    assert node.rpc("getbestblockhash") == tip
    assert layout(node.datadir)["separated"], "reindex below activation rebuilt the legacy layout"
    node.rpc("wallet.open", [node.wallet])
    assert node.rpc("wallet.unlock", [node.password, 3600])["success"] is True
    node.generate(101 - height, address)
    node.mine(address)  # Orchard activation at regtest height 102
    node.stop()
    print("PASS reindex below activation kept the separated layout; activation block mined", flush=True)

    # Too little disk: refuse, keep the legacy store, still start.
    os.environ["DINERO_TEST_STORAGE_UPGRADE_FREE_BYTES"] = "1"
    try:
        node = fresh_copy("disk")
        node.start()
        assert node.rpc("getbestblockhash") == tip
        node.stop()
    finally:
        os.environ.pop("DINERO_TEST_STORAGE_UPGRADE_FREE_BYTES", None)
    log = (node.root / f"process-{node.starts}.log").read_text(errors="replace")
    assert "storage upgrade refused (not enough free disk" in log, "refusal not reported"
    assert layout(node.datadir) == {"separated": False, "legacy_kept": False, "leftovers": []}
    print("PASS too little disk: upgrade refused, legacy store kept, node started", flush=True)
    print("PASS automatic storage upgrade", flush=True)


if __name__ == "__main__":
    main()
