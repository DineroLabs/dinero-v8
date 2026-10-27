#!/usr/bin/env python3
"""Exercise the real source guard with LF, Windows CRLF and changed bytes."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(sys.argv[1]).resolve()
cmake = sys.argv[2]
original = (root / "third_party/rocksdb/util/crc32c_arm64.cc").read_bytes()
assert b"\r" not in original
with tempfile.TemporaryDirectory(prefix="dinero-rocksdb-source-") as directory:
    work = Path(directory)
    upstream = work / "upstream"
    (upstream / "util").mkdir(parents=True)
    script = work / "prepare.cmake"
    script.write_text(f'include("{root.as_posix()}/cmake/PrepareRocksDB.cmake")\n'
                      f'dinero_prepare_rocksdb_source("{upstream.as_posix()}" "{work.as_posix()}/prepared")\n')
    for label, source, accepted in (
        ("LF", original, True),
        ("CRLF", original.replace(b"\n", b"\r\n"), True),
        ("modified LF", original + b"// changed upstream\n", False),
        ("modified CRLF", original.replace(b"\n", b"\r\n") + b"// changed upstream\r\n", False),
    ):
        path = upstream / "util/crc32c_arm64.cc"
        path.write_bytes(source)
        result = subprocess.run([cmake, "-P", str(script)], capture_output=True, text=True)
        assert (result.returncode == 0) == accepted, (label, result.stdout, result.stderr)
        assert path.read_bytes() == source, "upstream checkout mutated"
        if accepted:
            assert (work / "prepared/util/crc32c_arm64.cc").read_bytes() == (
                root / "cmake/rocksdb/crc32c_arm64.cc").read_bytes()
        else:
            assert "RocksDB ARM checksum source changed" in result.stderr
        print(f"PASS: {label}")
