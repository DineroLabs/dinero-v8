"""
DineroCoin Database Corruptor

Intentionally corrupts database files to test recovery.
Tests RocksDB and SQLite recovery mechanisms.
"""

import os
import random
import shutil
import struct
from pathlib import Path
from typing import Optional, List, Dict, Any
from dataclasses import dataclass
from enum import Enum, auto


class CorruptionType(Enum):
    """Types of database corruption"""
    TRUNCATE = auto()          # Truncate file mid-way
    ZERO_BYTES = auto()        # Zero out a section
    RANDOM_BYTES = auto()      # Random garbage
    FLIP_BITS = auto()         # Flip random bits
    DELETE_FILE = auto()       # Delete entire file
    CORRUPT_HEADER = auto()    # Corrupt file header
    APPEND_GARBAGE = auto()    # Append garbage to end


@dataclass
class CorruptionResult:
    """Result of a corruption operation"""
    corruption_type: CorruptionType
    file_path: str
    offset: int
    size: int
    success: bool
    original_backed_up: bool


class DBCorruptor:
    """
    Database corruption utility for testing recovery.

    NEVER use on production data. Always backs up before corrupting.

    Usage:
        corruptor = DBCorruptor("/path/to/datadir")

        # Corrupt RocksDB
        corruptor.corrupt_rocksdb()

        # Corrupt SQLite wallet
        corruptor.corrupt_wallet_db()

        # Restore from backup
        corruptor.restore_backup()
    """

    def __init__(self, datadir: str, backup_dir: Optional[str] = None):
        self.datadir = Path(datadir)
        self.backup_dir = Path(backup_dir) if backup_dir else self.datadir / ".corruption_backup"
        self.corruptions: List[CorruptionResult] = []

    def backup(self, file_path: Path) -> bool:
        """Backup a file before corrupting"""
        try:
            self.backup_dir.mkdir(parents=True, exist_ok=True)
            rel_path = file_path.relative_to(self.datadir)
            backup_path = self.backup_dir / rel_path
            backup_path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(file_path, backup_path)
            return True
        except Exception as e:
            print(f"Backup failed: {e}")
            return False

    def restore_backup(self, file_path: Optional[Path] = None) -> bool:
        """Restore files from backup"""
        try:
            if file_path:
                rel_path = file_path.relative_to(self.datadir)
                backup_path = self.backup_dir / rel_path
                if backup_path.exists():
                    shutil.copy2(backup_path, file_path)
                    return True
            else:
                # Restore all backed up files
                for backup_file in self.backup_dir.rglob("*"):
                    if backup_file.is_file():
                        rel_path = backup_file.relative_to(self.backup_dir)
                        target = self.datadir / rel_path
                        target.parent.mkdir(parents=True, exist_ok=True)
                        shutil.copy2(backup_file, target)
                return True
        except Exception as e:
            print(f"Restore failed: {e}")
            return False

    def _corrupt_file(
        self,
        file_path: Path,
        corruption_type: CorruptionType,
        offset: Optional[int] = None,
        size: int = 64,
    ) -> CorruptionResult:
        """Apply corruption to a file"""
        backed_up = self.backup(file_path)

        try:
            file_size = file_path.stat().st_size

            if offset is None:
                # Pick random offset, avoiding very beginning for some types
                if corruption_type == CorruptionType.CORRUPT_HEADER:
                    offset = 0
                else:
                    offset = random.randint(min(16, file_size // 4), max(16, file_size - size))

            if corruption_type == CorruptionType.TRUNCATE:
                with open(file_path, 'r+b') as f:
                    f.truncate(offset)
                actual_size = file_size - offset

            elif corruption_type == CorruptionType.ZERO_BYTES:
                with open(file_path, 'r+b') as f:
                    f.seek(offset)
                    f.write(b'\x00' * size)
                actual_size = size

            elif corruption_type == CorruptionType.RANDOM_BYTES:
                with open(file_path, 'r+b') as f:
                    f.seek(offset)
                    f.write(os.urandom(size))
                actual_size = size

            elif corruption_type == CorruptionType.FLIP_BITS:
                with open(file_path, 'r+b') as f:
                    f.seek(offset)
                    data = bytearray(f.read(size))
                    for i in range(len(data)):
                        data[i] ^= random.randint(1, 255)
                    f.seek(offset)
                    f.write(bytes(data))
                actual_size = size

            elif corruption_type == CorruptionType.DELETE_FILE:
                file_path.unlink()
                actual_size = file_size
                offset = 0

            elif corruption_type == CorruptionType.CORRUPT_HEADER:
                with open(file_path, 'r+b') as f:
                    f.write(b'\xDE\xAD\xBE\xEF' + os.urandom(12))
                actual_size = 16
                offset = 0

            elif corruption_type == CorruptionType.APPEND_GARBAGE:
                with open(file_path, 'ab') as f:
                    f.write(os.urandom(size))
                actual_size = size
                offset = file_size

            result = CorruptionResult(
                corruption_type=corruption_type,
                file_path=str(file_path),
                offset=offset,
                size=actual_size,
                success=True,
                original_backed_up=backed_up,
            )

        except Exception as e:
            result = CorruptionResult(
                corruption_type=corruption_type,
                file_path=str(file_path),
                offset=offset or 0,
                size=size,
                success=False,
                original_backed_up=backed_up,
            )

        self.corruptions.append(result)
        return result

    # =========================================================================
    # RocksDB Corruption
    # =========================================================================

    def corrupt_rocksdb(
        self,
        db_name: str = "chainstate",
        corruption_type: CorruptionType = CorruptionType.RANDOM_BYTES,
    ) -> List[CorruptionResult]:
        """
        Corrupt RocksDB database files.

        RocksDB uses:
        - .sst files (data)
        - .log files (WAL)
        - MANIFEST files (metadata)
        - CURRENT file (points to manifest)
        """
        results = []

        db_path = self.datadir / "regtest" / db_name
        if not db_path.exists():
            db_path = self.datadir / db_name

        if not db_path.exists():
            print(f"RocksDB not found at {db_path}")
            return results

        # Find SST files
        sst_files = list(db_path.glob("*.sst"))
        if sst_files:
            target = random.choice(sst_files)
            results.append(self._corrupt_file(target, corruption_type))

        return results

    def corrupt_rocksdb_wal(self, db_name: str = "chainstate") -> CorruptionResult:
        """Corrupt RocksDB write-ahead log"""
        db_path = self.datadir / "regtest" / db_name
        log_files = list(db_path.glob("*.log"))
        if log_files:
            return self._corrupt_file(log_files[-1], CorruptionType.TRUNCATE)
        return None

    def corrupt_rocksdb_manifest(self, db_name: str = "chainstate") -> CorruptionResult:
        """Corrupt RocksDB MANIFEST file"""
        db_path = self.datadir / "regtest" / db_name
        manifest_files = list(db_path.glob("MANIFEST-*"))
        if manifest_files:
            return self._corrupt_file(manifest_files[-1], CorruptionType.CORRUPT_HEADER)
        return None

    # =========================================================================
    # SQLite Corruption (Wallet)
    # =========================================================================

    def corrupt_wallet_db(
        self,
        corruption_type: CorruptionType = CorruptionType.RANDOM_BYTES,
    ) -> CorruptionResult:
        """
        Corrupt SQLite wallet database.

        Tests Priority 4 persistence safety.
        """
        wallet_paths = [
            self.datadir / "regtest" / "wallet.db",
            self.datadir / "regtest" / "wallets" / "wallet.db",
            self.datadir / "wallet.db",
        ]

        for path in wallet_paths:
            if path.exists():
                return self._corrupt_file(path, corruption_type)

        print("Wallet database not found")
        return None

    def corrupt_wallet_wal(self) -> CorruptionResult:
        """Corrupt SQLite WAL file"""
        wal_paths = [
            self.datadir / "regtest" / "wallet.db-wal",
            self.datadir / "regtest" / "wallets" / "wallet.db-wal",
        ]

        for path in wal_paths:
            if path.exists():
                return self._corrupt_file(path, CorruptionType.TRUNCATE)

        return None

    # =========================================================================
    # Comprehensive Corruption Scenarios
    # =========================================================================

    def corrupt_all(self) -> Dict[str, List[CorruptionResult]]:
        """Corrupt all databases in various ways"""
        results = {
            "rocksdb": self.corrupt_rocksdb(),
            "rocksdb_wal": [self.corrupt_rocksdb_wal()],
            "wallet": [self.corrupt_wallet_db()],
        }
        return {k: [r for r in v if r] for k, v in results.items()}

    def summary(self) -> Dict[str, Any]:
        """Get summary of all corruptions"""
        return {
            "total": len(self.corruptions),
            "successful": sum(1 for c in self.corruptions if c.success),
            "backed_up": sum(1 for c in self.corruptions if c.original_backed_up),
            "by_type": {
                t.name: sum(1 for c in self.corruptions if c.corruption_type == t)
                for t in CorruptionType
            },
        }
