#!/usr/bin/env python3
"""An SDK without the real Orchard runtime cannot adopt an activation root."""
from pathlib import Path
import json
import subprocess
import sys
import tempfile

binary = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="nodecore-off-activation-") as directory:
    root = Path(directory)
    datadir = root / "node"
    datadir.mkdir()
    sentinel = datadir / "sentinel"
    sentinel.write_bytes(b"retained original")
    reply = root / "reply.jsonl"
    request = {"op": "completed_activation_cold", "datadir": str(datadir), "origin_runtime": True}
    result = subprocess.run([binary, str(reply)], input=json.dumps(request) + "\n", text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
    assert result.returncode == 0, result.stdout
    rows = [json.loads(line) for line in reply.read_text().splitlines()]
    assert rows == [{"driver_error": "Completed activation requires the Orchard runtime"}], rows
    assert sentinel.read_bytes() == b"retained original"
    assert sorted(p.name for p in datadir.iterdir()) == ["sentinel"], "refusal modified the root"
    print("PASS: backend-OFF SDK refuses activation without opening or modifying the root")
