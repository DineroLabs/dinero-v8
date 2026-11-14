#!/usr/bin/env python3
"""Check the Docker image's four retained exact-base recovery payloads."""

import argparse
import hashlib
import json
from pathlib import Path
import sys

# The script runs from this source directory in local tests and from /verify in
# the image fetch stage, where snapshot_transport.py is copied beside it.
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "snapshot"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from snapshot_transport import inspect

EXTRA_DOCKER_BASE = {
    "filename": "dinero-assumeutxo-73035-v4.dat",
    "height": 73035,
    "format_version": 4,
    "sha256": "0a98ab1bd544d333afae7c8d2b42b0a910fb5e7fcdefd40642c6d3e0c6aae8a4",
}
EXPECTED_RECOVERY_ORDER = [
    "dinero-assumeutxo-148375-v5.dat",
    "dinero-assumeutxo-99677-v4.dat",
    "utxo-snapshot-65300.dat",
]


def verify(root: Path, catalog_path: Path, extra_docker_base: dict | None = None) -> list[str]:
    catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
    catalog_bases = catalog["fallbacks"]
    if [item.get("filename") for item in catalog_bases] != EXPECTED_RECOVERY_ORDER:
        raise ValueError("desktop recovery catalog is not the H148375/H99677/H65300 set")
    entries = [catalog_bases[0], catalog_bases[1],
               extra_docker_base or EXTRA_DOCKER_BASE, catalog_bases[2]]
    verified = []
    for pin in entries:
        filename = pin["filename"]
        payload = root / filename
        manifest = root / (filename + ".manifest.json")
        data = json.loads(manifest.read_text(encoding="utf-8"))
        identity = data["snapshot"]
        # Historical v4 manifests predate the format_version field. Their
        # payload headers still carry v4 and are checked by inspect() below.
        declared_format = data.get("format_version")
        if (data.get("network") != "mainnet" or
                declared_format not in ((None, 4) if pin["format_version"] == 4 else (5,)) or
                identity.get("snapshot_file") != filename):
            raise ValueError(f"{filename}: manifest network or filename differs")
        for field in ("height", "block_hash", "bytes", "sha256"):
            if field in pin and identity.get(field) != pin[field]:
                raise ValueError(f"{filename}: manifest differs from pinned {field}")
        if payload.stat().st_size != identity["bytes"]:
            raise ValueError(f"{filename}: payload length differs from manifest")
        with payload.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        if digest != identity["sha256"] or digest != pin["sha256"]:
            raise ValueError(f"{filename}: payload checksum differs from pin")
        version, height, block_hash, _profile = inspect(payload)
        if (version, height) != (pin["format_version"], pin["height"]):
            raise ValueError(f"{filename}: payload version or height differs from pin")
        if "block_hash" in pin and block_hash != pin["block_hash"]:
            raise ValueError(f"{filename}: payload base hash differs from pin")
        verified.append(filename)
    return verified


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--catalog", type=Path, required=True)
    args = parser.parse_args()
    try:
        print("Verified exact Docker recovery bases:", ", ".join(verify(args.directory, args.catalog)))
    except (OSError, ValueError, KeyError, TypeError) as exc:
        parser.exit(1, f"Docker recovery inputs refused: {exc}\n")
