#!/usr/bin/env python3
"""Actual dinerod routes only an authenticated full-archive v3 index.

The test-only DaemonApp executable creates the same v2 index and offline v3
upgrade used by wallet schema tests. Production startup runs under its real
datadir guard; malformed and incompatible sources still refuse before services.
"""

from pathlib import Path
from contextlib import closing
import hashlib
import os
import signal
import shutil
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time

if sys.platform != "win32":
    import fcntl


dinerod = str(Path(sys.argv[1]).resolve())
fixture = str(Path(sys.argv[2]).resolve())
root = Path(tempfile.mkdtemp(prefix="dinero-wallet-v3-startup-"))
success = False


def run(args, timeout=120):
    result = subprocess.run(args, stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, timeout=timeout)
    return result.returncode, result.stdout


def run_to_ready(args, log_path, timeout=60):
    with log_path.open("w+") as log:
        process = subprocess.Popen(args, stdin=subprocess.DEVNULL,
                                   stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                log.flush()
                output = log_path.read_text(errors="replace")
                # The main-loop banner uses a buffered newline when stdout is
                # redirected. The service-start line is explicitly flushed.
                if "[DaemonApp] All services started successfully" in output:
                    process.send_signal(signal.SIGINT if sys.platform != "win32"
                                        else signal.SIGTERM)
                    process.wait(timeout=45)
                    if sys.platform != "win32":
                        assert process.returncode == 0, output[-18000:]
                        assert "Clean shutdown complete" in log_path.read_text(errors="replace")
                    return log_path.read_text(errors="replace")
                if process.poll() is not None:
                    raise AssertionError(output[-18000:])
                time.sleep(0.1)
            raise AssertionError("v3 production daemon did not become ready:\n" + output[-18000:])
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=15)


def ports():
    listeners = [socket.socket() for _ in range(3)]
    try:
        for listener in listeners:
            listener.bind(("127.0.0.1", 0))
        return [listener.getsockname()[1] for listener in listeners]
    finally:
        for listener in listeners:
            listener.close()


def configure(datadir):
    datadir.mkdir()
    config = datadir / "dinero.conf"
    config.write_text("p2p.offline=true\npool.accounting.enable=true\n"
                      "lightning.oracles.enable=false\nwallet.socket.enable=false\n")
    return config


def command(binary, datadir, config, compact=False):
    rpc, p2p, wallet = ports()
    return [binary, "--regtest", f"--datadir={datadir}", f"--conf={config}",
            f"--rpcport={rpc}", f"--port={p2p}",
            f"--wallet-socket-port={wallet}", "--listen=0", "--utreexo=1",
            "--utreexo-stateless=1" if compact else "--utreexo-bridge=1"]


def upgrade_command(datadir, config):
    return [dinerod, "--regtest", f"--datadir={datadir}", f"--conf={config}",
            "--upgrade-wallet-index-v3"]


def marker(path):
    with closing(sqlite3.connect(path.as_uri() + "?mode=ro", uri=True)) as db:
        version = db.execute("PRAGMA user_version").fetchone()[0]
        entries = dict(db.execute("SELECT key,value FROM utxo_metadata "
                                  "WHERE key LIKE 'wallet_index_v3:%'"))
    return version, entries


def inventory(index, backup):
    # Include SQLite sidecars: a refused startup must leave the exact v3
    # source and its durable rollback copy alone.
    paths = [p for p in index.parent.iterdir()
             if p.name == index.name or p.name.startswith(index.name + "-")
             or p.name.startswith(index.name + ".v2-pre-v3-")]
    assert index in paths and backup.resolve() in [p.resolve() for p in paths], paths
    return {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}


def chain_inventory(chaindb):
    return {str(p.relative_to(chaindb)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in chaindb.rglob("*") if p.is_file()}


try:
    v2_cases = [(False, False), (True, False)]
    if sys.platform != "win32":
        v2_cases.append((False, True))
    for compact, linked in v2_cases:
        datadir = root / ("compact-v2" if compact else
                          "linked-full-v2" if linked else "full-v2")
        config = configure(datadir)
        index = datadir / "blockchain" / "utxo"
        index.parent.mkdir()
        backing = index.parent / "utxo-linked-target" if linked else index
        rc, output = run([fixture, "--fixture-create-wallet-index-v2", str(backing)])
        assert rc == 0 and "PASS v2 fixture" in output, output[-12000:]
        if linked:
            index.symlink_to(backing.name)
        assert marker(index)[0] == 2
        rc, output = run(command(fixture, datadir, config, compact))
        assert rc == 0 and "PASS actual daemon" in output, output[-18000:]
        if not compact and not linked:
            # Ordinary v2 remains eligible for the existing offline upgrade;
            # the new footprint fence must not classify it as damaged v3.
            rc, output = run([dinerod, "--regtest", f"--datadir={datadir}",
                              f"--conf={config}", "--wallet-index-v3-status"])
            assert rc == 0 and "wallet_index_v3_status=upgrade_available" in output, output[-18000:]
            assert marker(index)[0] == 2
        print(f"PASS {'stateless' if compact else 'symlinked full' if linked else 'full'} v2 DaemonApp startup and stop")

    # The production executable's maintenance path has no DaemonApp bootstrap:
    # it must inspect the existing full store under its lock, make the durable
    # v2 backup, verify v3, and exit before any service starts.
    datadir = root / "offline-full-v2"
    config = configure(datadir)
    index = datadir / "blockchain" / "utxo"
    index.parent.mkdir()
    rc, output = run([fixture, "--fixture-create-wallet-index-v2", str(index)])
    assert rc == 0 and "PASS v2 fixture" in output, output[-12000:]
    rc, output = run(command(fixture, datadir, config))
    assert rc == 0 and "PASS actual daemon" in output, output[-18000:]
    assert marker(index)[0] == 2

    # Configuration and imported-source refusals must leave v2 unmodified.
    base_config = config.read_text()
    for setting in ("sync-profile=ios_utreexo\n", "utreexo-stateless=1\n",
                    "prune=500\n", "archival=false\n"):
        config.write_text(base_config + setting)
        before = marker(index)
        rc, output = run(upgrade_command(datadir, config))
        assert rc != 0 and "requires full archival profile" in output, output[-12000:]
        assert marker(index) == before
        assert not list(index.parent.glob("utxo.v2-pre-v3-*"))
    config.write_text(base_config)
    with closing(sqlite3.connect(index)) as db:
        db.execute("INSERT OR REPLACE INTO utxo_metadata(key,value) VALUES (?,?)",
                   ("wallet_snapshot_recovery_base_height", "99700"))
        db.commit()
    rc, output = run(upgrade_command(datadir, config))
    assert rc != 0 and "refuses imported wallet snapshot state" in output, output[-12000:]
    assert marker(index)[0] == 2
    with closing(sqlite3.connect(index)) as db:
        db.execute("DELETE FROM utxo_metadata WHERE key='wallet_snapshot_recovery_base_height'")
        db.commit()

    rc, output = run(upgrade_command(datadir, config) + ["--daemon"])
    assert rc != 0 and "foreground-only maintenance" in output, output[-12000:]
    assert marker(index)[0] == 2
    if sys.platform != "win32":
        with (datadir / "dinerod.lock").open("a+b") as lock_file:
            fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            rc, output = run(upgrade_command(datadir, config))
            assert rc != 0 and "already in use" in output, output[-12000:]
            assert marker(index)[0] == 2
            fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)
        print("PASS offline upgrade refused while datadir lock was held")

    # The lock protects only this datadir. Every writable SQLite path must
    # remain inside it and have its own inode, even when a clone was made by
    # copying or linking files from another tree.
    hardlink = index.with_name("utxo-hardlink")
    os.link(index, hardlink)
    rc, output = run(upgrade_command(datadir, config))
    assert rc != 0 and "refuses hardlinked file" in output, output[-12000:]
    assert marker(index)[0] == 2 and not list(index.parent.glob("utxo.v2-pre-v3-*"))
    hardlink.unlink()
    print("PASS offline upgrade refused source index with another hardlink")

    if sys.platform != "win32":
        target = index.with_name("utxo-real")
        index.rename(target)
        index.symlink_to(target.name)
        try:
            rc, output = run(upgrade_command(datadir, config))
            assert rc != 0 and "refuses missing or linked path" in output, output[-12000:]
            rc, output = run([fixture, "--fixture-upgrade-wallet-index-v3", str(index)])
            assert rc != 0 and "source file unavailable or linked" in output, output[-12000:]
            assert marker(target)[0] == 2
        finally:
            index.unlink()
            target.rename(index)
        print("PASS CLI and public schema upgrade refused linked source index")

        chaindb = index.parent / "chaindb"
        target = index.parent / "chaindb-real"
        chaindb.rename(target)
        chaindb.symlink_to(target.name, target_is_directory=True)
        try:
            rc, output = run(upgrade_command(datadir, config))
            assert rc != 0 and "refuses missing or linked path" in output, output[-12000:]
        finally:
            chaindb.unlink()
            target.rename(chaindb)
        print("PASS offline upgrade refused linked ChainDB directory")

        blockchain = datadir / "blockchain"
        target = datadir / "blockchain-real"
        blockchain.rename(target)
        blockchain.symlink_to(target.name, target_is_directory=True)
        try:
            rc, output = run(upgrade_command(datadir, config))
            assert rc != 0 and "refuses missing or linked path" in output, output[-12000:]
        finally:
            blockchain.unlink()
            target.rename(blockchain)
        print("PASS offline upgrade refused linked blockchain directory")

        sidecar = index.with_name(index.name + "-shm")
        saved_sidecar = index.with_name(index.name + "-shm-saved")
        if sidecar.exists():
            sidecar.rename(saved_sidecar)
        external = datadir / "external-shm"
        external.write_bytes(b"sentinel")
        sidecar.symlink_to(external)
        try:
            rc, output = run(upgrade_command(datadir, config))
            assert rc != 0 and "refuses missing or linked path" in output, output[-12000:]
            assert external.read_bytes() == b"sentinel"
        finally:
            sidecar.unlink()
            external.unlink()
            if saved_sidecar.exists():
                saved_sidecar.rename(sidecar)
        print("PASS offline upgrade refused linked SQLite sidecar")
    assert marker(index)[0] == 2 and not list(index.parent.glob("utxo.v2-pre-v3-*"))

    rc, output = run(upgrade_command(datadir, config))
    assert rc == 0 and "offline upgrade verified; rollback backup:" in output, output[-18000:]
    assert "[DaemonApp] Initializing services" not in output, output[-18000:]
    backup = Path(output.split("rollback backup: ", 1)[1].splitlines()[0].strip().strip('"'))
    assert backup.is_file(), backup
    before_marker = marker(index)
    assert before_marker[0] == 3 and "wallet_index_v3:rollback_backup_digest" in before_marker[1]
    backup_digest = hashlib.sha256(backup.read_bytes()).hexdigest()
    rc, output = run(upgrade_command(datadir, config))
    assert rc == 0 and "offline upgrade verified" in output, output[-18000:]
    assert marker(index) == before_marker and hashlib.sha256(backup.read_bytes()).hexdigest() == backup_digest
    print("PASS actual dinerod offline v2-to-v3 upgrade, durable backup and idempotent retry")

    # Crash-window simulation: a matching backup was published but the v2
    # transaction had not committed. The next invocation must consume it.
    retry_dir = root / "offline-published-backup-v2"
    retry_conf = configure(retry_dir)
    retry_index = retry_dir / "blockchain" / "utxo"
    retry_index.parent.mkdir()
    rc, output = run([fixture, "--fixture-create-wallet-index-v2", str(retry_index)])
    assert rc == 0 and "PASS v2 fixture" in output, output[-12000:]
    rc, output = run(command(fixture, retry_dir, retry_conf))
    assert rc == 0 and "PASS actual daemon" in output, output[-18000:]
    for sidecar in (retry_index.with_name(retry_index.name + "-wal"),
                    retry_index.with_name(retry_index.name + "-shm")):
        sidecar.unlink(missing_ok=True)
    shutil.copy2(backup, retry_index)
    published = retry_index.parent / backup.name
    shutil.copy2(backup, published)
    rc, output = run(upgrade_command(retry_dir, retry_conf))
    assert rc == 0 and marker(retry_index)[0] == 3 and str(published) in output, output[-18000:]
    assert hashlib.sha256(published.read_bytes()).hexdigest() == backup_digest
    print("PASS actual dinerod reused a published v2 backup after simulated interrupted upgrade")

    # A published name that is merely another link to the live v2 inode
    # cannot serve as a rollback copy. This formerly passed the digest check
    # and then both names became v3 in the same SQLite transaction.
    original_v2 = published.read_bytes()
    for sidecar in (retry_index.with_name(retry_index.name + "-wal"),
                    retry_index.with_name(retry_index.name + "-shm")):
        sidecar.unlink(missing_ok=True)
    retry_index.unlink()
    published.unlink()
    retry_index.write_bytes(original_v2)
    os.link(retry_index, published)
    assert marker(retry_index)[0] == 2
    rc, output = run(upgrade_command(retry_dir, retry_conf))
    assert rc != 0 and ("rollback backup is not independent from source" in output or
                        "refuses hardlinked file" in output), output[-18000:]
    assert marker(retry_index)[0] == 2 and os.stat(retry_index).st_ino == os.stat(published).st_ino
    print("PASS actual dinerod refused a hardlinked rollback backup before v3 mutation")
    published.unlink()
    external_backup = retry_index.parent / "external-v2-rollback-copy"
    external_backup.write_bytes(original_v2)
    os.link(external_backup, published)
    rc, output = run(upgrade_command(retry_dir, retry_conf))
    assert rc != 0 and "rollback backup is hardlinked outside this index" in output, output[-18000:]
    assert marker(retry_index)[0] == 2 and external_backup.read_bytes() == original_v2
    published.unlink()
    external_backup.unlink()
    print("PASS actual dinerod refused a rollback backup hardlinked to an external file")
    if sys.platform != "win32":
        published.symlink_to(retry_index.name)
        rc, output = run(upgrade_command(retry_dir, retry_conf))
        assert rc != 0 and "rollback backup link refused" in output, output[-18000:]
        assert marker(retry_index)[0] == 2
        print("PASS actual dinerod refused a symlinked rollback backup before v3 mutation")

    backup.unlink()
    rc, output = run(upgrade_command(datadir, config))
    assert rc != 0 and "rollback backup missing" in output, output[-18000:]
    assert marker(index) == before_marker, "missing backup refusal changed v3"
    print("PASS actual dinerod refused missing v3 rollback backup without changing index")

    v3_cases = [(False, False), (True, False)]
    if sys.platform != "win32":
        v3_cases.append((False, True))
    for damaged, linked in v3_cases:
        datadir = root / ("damaged-v3" if damaged else
                          "linked-v3" if linked else "authentic-v3")
        config = configure(datadir)
        index = datadir / "blockchain" / "utxo"
        index.parent.mkdir()
        backing = index.parent / "utxo-linked-target" if linked else index
        if not damaged and not linked:
            rc, output = run([fixture, "--fixture-create-wallet-index-v2", str(backing)])
            assert rc == 0 and "PASS v2 fixture" in output, output[-12000:]
            rc, output = run(command(fixture, datadir, config))
            assert rc == 0 and "PASS actual daemon" in output, output[-18000:]
            rc, output = run(upgrade_command(datadir, config))
            assert rc == 0 and "offline upgrade verified; rollback backup:" in output, output[-18000:]
            backup = Path(output.split("rollback backup: ", 1)[1].splitlines()[0].strip().strip('"'))
        else:
            rc, output = run([fixture, "--fixture-create-wallet-index-v2", str(backing)])
            assert rc == 0 and "PASS v2 fixture" in output, output[-12000:]
            if linked:
                index.symlink_to(backing.name)
            rc, output = run(command(fixture, datadir, config))
            assert rc == 0 and "PASS actual daemon" in output, output[-18000:]
            rc, output = run([fixture, "--fixture-upgrade-wallet-index-v3", str(backing)])
            assert rc == 0 and "PASS v3 fixture " in output, output[-12000:]
            backup = Path(output.split("PASS v3 fixture ", 1)[1].splitlines()[0])
        assert backup.is_file(), output
        version, entries = marker(index)
        assert version == 3 and "wallet_index_v3:rollback_backup_digest" in entries
        if damaged:
            with closing(sqlite3.connect(backing)) as db:
                changed = db.execute("UPDATE utxo_metadata SET value=? "
                                     "WHERE key='wallet_index_v3:rollback_backup_digest'",
                                     ("0" * 64,)).rowcount
                db.commit()
            assert changed == 1
        before_marker = marker(index)
        before_files = inventory(backing, backup)
        startup_backup_digest = hashlib.sha256(backup.read_bytes()).hexdigest()
        if not damaged and not linked:
            # The v2 fixture used pool accounting to exercise its normal
            # lifecycle. Remove that isolated DB so rejected v3 starts prove
            # they do not create a pool DB before source authentication.
            pool_dir = datadir / "pool"
            if pool_dir.exists():
                shutil.rmtree(pool_dir)
            base_config = config.read_text()
            for setting in ("sync-profile=ios_utreexo\n", "utreexo-stateless=1\n",
                            "prune=500\n", "archival=false\n"):
                config.write_text(base_config + setting)
                rc, output = run(command(dinerod, datadir, config), timeout=30)
                assert rc != 0 and "Wallet index v3 startup requires full archival profile without pruning" in output, output[-18000:]
                assert marker(index) == before_marker
                assert hashlib.sha256(backup.read_bytes()).hexdigest() == startup_backup_digest
                assert not pool_dir.exists(), "refused v3 startup created pool state"
            config.write_text(base_config)
            for attempt in range(2):
                output = run_to_ready(command(dinerod, datadir, config),
                                      datadir / f"v3-startup-{attempt}.log")
                assert "Wallet index v3 full-archive metadata-only route" in output, output[-18000:]
                assert "deferred to read-only full-archive source preflight" in output, output[-18000:]
                assert marker(index) == before_marker
                assert hashlib.sha256(backup.read_bytes()).hexdigest() == startup_backup_digest
            assert (pool_dir / "pool_accounting.sqlite").is_file(), "valid v3 startup did not initialize pool"
            print("PASS actual dinerod started and cleanly restarted authenticated v3 under full archival route")
            # A lost SQLite file or a damaged PRAGMA version must not cause a
            # formerly authenticated v3 source to enter the legacy genesis
            # guard, return Qt's wipe-offer code 10, or delete its backup.
            for damage in ("missing-index", "downgraded-version"):
                clone = root / ("v3-footprint-" + damage)
                shutil.copytree(datadir, clone)
                clone_config = clone / "dinero.conf"
                clone_index = clone / "blockchain" / "utxo"
                clone_backup = clone_index.parent / backup.name
                if damage == "missing-index":
                    clone_index.unlink()
                else:
                    with closing(sqlite3.connect(clone_index)) as db:
                        db.execute("PRAGMA user_version=2")
                        db.commit()
                saved_backup = hashlib.sha256(clone_backup.read_bytes()).hexdigest()
                saved_chain = chain_inventory(clone / "blockchain" / "chaindb")
                for args in (
                    command(dinerod, clone, clone_config),
                    command(dinerod, clone, clone_config) + ["--wipe-stale-chain"],
                    [dinerod, "--regtest", f"--datadir={clone}",
                     f"--conf={clone_config}", "--wallet-index-v3-status"],
                    upgrade_command(clone, clone_config),
                    [dinerod, "--regtest", f"--datadir={clone}",
                     "--repair-db", "--confirm"],
                ):
                    rc, output = run(args, timeout=30)
                    assert rc == 1 and "Wallet index v3" in output and "footprint" in output, output[-18000:]
                    assert "[GenesisGuard] Wiping stale chain data" not in output, output[-18000:]
                    assert hashlib.sha256(clone_backup.read_bytes()).hexdigest() == saved_backup
                    assert chain_inventory(clone / "blockchain" / "chaindb") == saved_chain
                assert (not clone_index.exists()) == (damage == "missing-index")
            print("PASS missing/downgraded v3 footprint refused startup, Qt wipe route, repair and offline upgrade without deleting rollback backup")
            before_chain = chain_inventory(index.parent / "chaindb")
            for option, expected in (("--reindex", "refuses legacy maintenance options"),
                                     ("--assumeutxo_snapshot=/nonexistent", "refuses configured snapshot bootstrap"),
                                     ("--assumeutxo_snapshot_fallbacks=/nonexistent", "refuses configured snapshot bootstrap"),
                                     ("--assumeutxo_forward_connect=1", "refuses configured snapshot bootstrap"),
                                     ("--wipe-stale-chain", "startup refuses --wipe-stale-chain")):
                rc, output = run(command(dinerod, datadir, config) + [option], timeout=30)
                assert rc != 0 and expected in output, output[-18000:]
            rc, output = run(command(dinerod, datadir, config) +
                             ["--repair-db", "--confirm"], timeout=30)
            assert rc != 0 and "Wallet index v3 repair refused" in output, output[-18000:]
            imported = "wallet_snapshot_recovery_base_height"
            with closing(sqlite3.connect(index)) as db:
                db.execute("INSERT OR REPLACE INTO utxo_metadata(key,value) VALUES (?,?)",
                           (imported, "99700"))
                db.commit()
            rc, output = run(command(dinerod, datadir, config), timeout=30)
            assert rc != 0 and "refuses imported wallet snapshot state" in output, output[-18000:]
            with closing(sqlite3.connect(index)) as db:
                db.execute("DELETE FROM utxo_metadata WHERE key=?", (imported,))
                db.commit()
            hardlink = index.with_name("utxo-hardlink")
            os.link(index, hardlink)
            try:
                rc, output = run(command(dinerod, datadir, config), timeout=30)
                assert rc != 0 and "refuses hardlinked file" in output, output[-18000:]
            finally:
                hardlink.unlink()
            assert marker(index) == before_marker
            assert hashlib.sha256(backup.read_bytes()).hexdigest() == startup_backup_digest
            assert chain_inventory(index.parent / "chaindb") == before_chain, "refused startup changed ChainDB"
            print("PASS production v3 refused maintenance, snapshot import and linked source before writes")
            continue
        rc, output = run(command(dinerod, datadir, config), timeout=90)
        assert rc != 0, output[-18000:]
        if damaged:
            assert "Wallet index v3 rollback backup" in output, output[-18000:]
        elif linked:
            assert "Wallet index v3 startup refuses missing or linked path" in output, output[-18000:]
        assert "[DaemonApp] Initializing chainstate" not in output, output[-18000:]
        assert marker(index) == before_marker, "refusal changed the v3 markers"
        assert inventory(backing, backup) == before_files, "refusal changed v3 index or backup bytes"
        assert not linked or index.is_symlink() and index.readlink() == Path(backing.name)
        print(f"PASS actual dinerod refused {'damaged' if damaged else 'symlinked'} v3 before legacy wallet init without changing index or backup")
    success = True
finally:
    if success:
        shutil.rmtree(root)
    else:
        print(f"Retained v3 startup evidence: {root}", flush=True)
