#!/usr/bin/env python3
"""Qt preflights read old snapshot and v3 sources without changing wallet data."""

from contextlib import closing
import hashlib
import os
from pathlib import Path
import shutil
import socket
import sqlite3
import subprocess
import sys
import tempfile


dinerod = str(Path(sys.argv[1]).resolve())
fixture = str(Path(sys.argv[2]).resolve())
root = Path(tempfile.mkdtemp(prefix="dinero-wallet-v3-status-"))
success = False


def run(args, timeout=180):
    proc = subprocess.run(args, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True, timeout=timeout)
    return proc.returncode, proc.stdout


def ports():
    listeners = [socket.socket() for _ in range(3)]
    try:
        for listener in listeners:
            listener.bind(("127.0.0.1", 0))
        return [listener.getsockname()[1] for listener in listeners]
    finally:
        for listener in listeners:
            listener.close()


def index_files(index):
    # A SQLite read-only WAL connection may create an empty -wal and a -shm
    # coordination file. Those are not wallet state. A nonempty WAL is durable
    # committed data and must remain byte-identical, as must the index/backup.
    paths = sorted(p for p in index.parent.iterdir() if p.is_file() and
                   (p.name == index.name or p.name.startswith(index.name + "-") or
                    p.name.startswith(index.name + ".v2-pre-v3-")) and
                   not p.name.endswith("-shm") and
                   (not p.name.endswith("-wal") or p.stat().st_size > 0))
    return {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}


try:
    datadir = root / "node"
    datadir.mkdir()
    config = datadir / "dinero.conf"
    base_config = ("p2p.offline=true\npool.accounting.enable=true\n"
                   "lightning.oracles.enable=false\nwallet.socket.enable=false\n")
    config.write_text(base_config)
    index = datadir / "blockchain" / "utxo"
    index.parent.mkdir()
    rc, output = run([fixture, "--fixture-create-wallet-index-v2", str(index)])
    assert rc == 0 and "PASS v2 fixture" in output, output[-8000:]
    rpc, p2p, wallet = ports()
    rc, output = run([fixture, "--regtest", f"--datadir={datadir}",
                      f"--conf={config}", f"--rpcport={rpc}", f"--port={p2p}",
                      f"--wallet-socket-port={wallet}", "--listen=0",
                      "--utreexo=1", "--utreexo-bridge=1"])
    assert rc == 0 and "PASS actual daemon" in output, output[-12000:]

    common = [dinerod, "--regtest", f"--datadir={datadir}", f"--conf={config}"]
    status = common + ["--wallet-index-v3-status"]
    before = index_files(index)
    assert before and not any("v2-pre-v3" in name for name in before), before
    rc, output = run(status)
    assert rc == 0 and "wallet_index_v3_status=upgrade_available" in output, output[-12000:]
    assert "[DaemonApp] Initializing services" not in output, output[-12000:]
    assert index_files(index) == before, "status preflight changed durable v2 index or made backup"
    wal = index.with_name(index.name + "-wal")
    assert not wal.exists() or wal.stat().st_size == 0, "status preflight wrote a WAL"
    print("PASS eligible v2 status is read-only before any daemon services")

    legacy = root / "legacy"
    shutil.copytree(datadir / "blockchain", legacy / "blockchain")
    legacy_index = legacy / "blockchain" / "utxo"
    legacy_status = [dinerod, "--regtest", f"--datadir={legacy}",
                     "--legacy-snapshot-status"]
    rc, output = run(legacy_status)
    assert rc == 0 and "legacy_snapshot_status=current_or_full" in output, output[-12000:]
    with closing(sqlite3.connect(legacy_index)) as db:
        db.execute("INSERT OR REPLACE INTO utxo_metadata(key,value) VALUES (?,?)",
                   ("assumeutxo_lc_base_height", "52287"))
        db.commit()
    old_index = index_files(legacy_index)
    rc, output = run(legacy_status)
    assert rc == 0 and "legacy_snapshot_status=legacy_empty" in output, output[-12000:]
    assert index_files(legacy_index) == old_index, "legacy probe changed the wallet index"
    (legacy / "wallets").mkdir()
    (legacy / "wallets" / "wallet_Existing.db").write_bytes(b"wallet material")
    rc, output = run(legacy_status)
    assert rc == 0 and "legacy_snapshot_status=legacy_wallets" in output, output[-12000:]
    (legacy / "wallets" / "wallet_Existing.db").unlink()
    if os.name != "nt":
        dangling_wallet = legacy / "wallet.db"
        dangling_wallet.symlink_to(legacy / "wallet-missing.db")
        rc, output = run(legacy_status)
        assert rc == 0 and "legacy_snapshot_status=legacy_wallets" in output, output[-12000:]
        dangling_wallet.unlink()
    registry = legacy / "wallet_registry.db"
    registry_wal = legacy / "wallet_registry.db-wal"
    registry_wal.write_bytes(b"stranded wallet registry")
    rc, output = run(legacy_status)
    assert rc == 0 and "legacy_snapshot_status=legacy_wallets" in output, output[-12000:]
    registry_wal.unlink()
    with closing(sqlite3.connect(registry)) as db:
        db.execute("CREATE TABLE wallets(name TEXT NOT NULL)")
        db.execute("INSERT INTO wallets(name) VALUES ('Existing')")
        db.commit()
    rc, output = run(legacy_status)
    assert rc == 0 and "legacy_snapshot_status=legacy_wallets" in output, output[-12000:]
    with closing(sqlite3.connect(registry)) as db:
        db.execute("DROP TABLE wallets")
        db.commit()
    rc, output = run(legacy_status)
    assert rc != 0 and "Wallet registry cannot be verified" in output, output[-12000:]
    rc, output = run([dinerod, "--regtest", f"--datadir={legacy}",
                      "--wallet-index-v3-status"])
    assert rc != 0 and "refuses imported wallet snapshot state" in output, output[-12000:]
    print("PASS old lifecycle marker is never offered v3 upgrade; empty and funded data differ")

    for restriction in ("sync-profile=ios_utreexo\n", "utreexo-stateless=1\n",
                        "archival=false\n"):
        config.write_text(base_config + restriction)
        rc, output = run(status)
        assert rc != 0 and "requires full archival profile" in output, output[-12000:]
        assert index_files(index) == before
    config.write_text(base_config)
    with closing(sqlite3.connect(index)) as db:
        db.execute("PRAGMA journal_mode=WAL")
        db.execute("PRAGMA wal_autocheckpoint=0")
        db.execute("INSERT OR REPLACE INTO utxo_metadata(key,value) VALUES (?,?)",
                   ("wallet_snapshot_recovery_base_height", "99700"))
        db.commit()
        assert wal.exists() and wal.stat().st_size > 0, "fixture did not retain committed WAL"
        before_imported = index_files(index)
        rc, output = run(status)
        assert rc != 0 and "refuses imported wallet snapshot state" in output, output[-12000:]
        assert index_files(index) == before_imported, "status changed committed WAL bytes"
        db.execute("DELETE FROM utxo_metadata WHERE key='wallet_snapshot_recovery_base_height'")
        db.commit()
    print("PASS compact and imported WAL sources cannot be offered a v3 upgrade")

    rc, output = run(status + ["--daemon"])
    assert rc != 0 and "foreground-only maintenance" in output, output[-12000:]
    assert not any("v2-pre-v3" in name for name in index_files(index))

    rc, output = run(common + ["--upgrade-wallet-index-v3"])
    assert rc == 0 and "offline upgrade verified" in output, output[-12000:]
    after_upgrade = index_files(index)
    assert any("v2-pre-v3" in name for name in after_upgrade), after_upgrade
    rc, output = run(status)
    assert rc == 0 and "wallet_index_v3_status=ready" in output, output[-12000:]
    assert index_files(index) == after_upgrade, "status preflight changed v3 index or rollback"
    assert not wal.exists() or wal.stat().st_size == 0, "ready status wrote a WAL"
    print("PASS authenticated v3 status is read-only after upgrade")

    rc, output = run([fixture, "--fixture-prebase-refusal-messages"])
    assert rc == 0 and "PASS pre-base refusal messages" in output, output[-8000:]
    # A retained pre-base marker (snapshot bootstrap) is refused by name, with
    # its base height, before the index version is reported.
    rc, output = run([fixture, "--fixture-write-prebase-marker",
                      str(datadir / "blockchain" / "chaindb"), "7"])
    assert rc == 0 and "PASS pre-base marker 7" in output, output[-8000:]
    rc, output = run(status)
    assert rc != 0 and "refuses an imported pre-base source (snapshot base height 7, block" in output, output[-12000:]
    assert "could not read the pre-base marker" not in output, output[-12000:]
    print("PASS retained pre-base marker is reported with its snapshot base")
    success = True
finally:
    if success:
        shutil.rmtree(root)
    else:
        print(f"Retained wallet v3 status evidence: {root}", flush=True)
