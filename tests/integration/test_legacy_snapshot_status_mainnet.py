#!/usr/bin/env python3
"""Check the Windows release daemon against an isolated old mainnet index."""

from contextlib import closing
import hashlib
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile


def run(*args):
    process = subprocess.run(args, capture_output=True, text=True, timeout=60)
    output = process.stdout + process.stderr
    return process.returncode, output


daemon, fixture = map(lambda value: str(Path(value).resolve()), sys.argv[1:3])
with tempfile.TemporaryDirectory(prefix="dinero-mainnet-legacy-status-") as temporary:
    original = Path(temporary) / "original"
    original.mkdir()
    code, output = run(fixture, str(original))
    assert code == 0 and "PASS isolated mainnet v2" in output, output
    index = original / "blockchain" / "utxo"
    before = hashlib.sha256(index.read_bytes()).hexdigest()

    def status(path):
        return run(daemon, "--legacy-snapshot-status", "--datadir", str(path))

    code, output = status(original)
    assert code == 0 and "legacy_snapshot_status=current_or_full" in output, output
    assert hashlib.sha256(index.read_bytes()).hexdigest() == before

    for old_height in (99677, 148375):
        with closing(sqlite3.connect(index)) as database:
            database.execute("INSERT OR REPLACE INTO utxo_metadata(key,value) VALUES (?,?)",
                             ("assumeutxo_lc_base_height", str(old_height)))
            database.commit()
        before = hashlib.sha256(index.read_bytes()).hexdigest()
        code, output = status(original)
        assert code == 0 and "legacy_snapshot_status=legacy_empty" in output, (old_height, output)
        assert hashlib.sha256(index.read_bytes()).hexdigest() == before

    wallets = original / "wallets"
    wallets.mkdir()
    (wallets / "wallet_Existing.db").write_bytes(b"existing wallet material")
    code, output = status(original)
    assert code == 0 and "legacy_snapshot_status=legacy_wallets" in output, output
    assert hashlib.sha256(index.read_bytes()).hexdigest() == before

    fresh = Path(temporary) / "current"
    fresh.mkdir()
    code, output = status(fresh)
    assert code == 0 and "legacy_snapshot_status=missing" in output, output
    assert not (fresh / "blockchain").exists(), "status probe created a blockchain"
    print("PASS Windows mainnet old snapshot stays read-only; empty, funded and fresh folders differ")
