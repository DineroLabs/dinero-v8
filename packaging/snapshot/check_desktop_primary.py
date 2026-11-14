#!/usr/bin/env python3
"""Check the exact desktop full-v6 transport pin before staging an installer.

This is a package integrity check. The daemon must still authenticate the
snapshot against its selected headers and import/restart it natively.
"""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from snapshot_transport import inspect


def verify(payload: Path, manifest_path: Path, catalog_path: Path, expected_name: str) -> dict:
    catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
    if catalog.get("schema") != "dinero.desktop-snapshot-inputs.v1":
        raise ValueError("unsupported desktop snapshot catalog")
    pin = catalog["primary"]
    if (pin.get("filename") != expected_name or pin.get("format_version") != 6 or
            pin.get("height") != 152700 or pin.get("snapshot_profile") != "full"):
        raise ValueError("desktop primary pin is not the expected full-v6 release input")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if (manifest.get("network") != "mainnet" or manifest.get("format_version") != 6 or
            manifest.get("snapshot_profile") != "full"):
        raise ValueError("desktop primary manifest is not mainnet full-v6")
    identity = manifest.get("snapshot")
    if not isinstance(identity, dict) or identity.get("snapshot_file") != expected_name:
        raise ValueError("desktop primary manifest filename differs from installed name")
    for field in ("height", "block_hash", "bytes", "sha256"):
        if identity.get(field) != pin.get(field):
            raise ValueError(f"desktop primary manifest differs from pinned {field}")
    version, height, block_hash, profile = inspect(payload)
    if (version, height, block_hash, profile) != (6, pin["height"], pin["block_hash"], "full"):
        raise ValueError("desktop primary payload is not the pinned full-v6 base")
    if payload.stat().st_size != pin["bytes"]:
        raise ValueError("desktop primary payload length differs from pin")
    full_digest = hashlib.sha256()
    with payload.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            full_digest.update(chunk)
    digest = full_digest.hexdigest()
    if digest != pin["sha256"]:
        raise ValueError("desktop primary payload checksum differs from pin")
    # The transport pin binds the exact bytes; check the format's own trailer
    # too so a bad publisher payload is rejected before an installer is built.
    with payload.open("rb") as stream:
        body_digest = hashlib.sha256()
        remaining = pin["bytes"] - 32
        if remaining < 68:
            raise ValueError("desktop primary payload is too short")
        while remaining:
            chunk = stream.read(min(1024 * 1024, remaining))
            if not chunk:
                raise ValueError("truncated desktop primary payload")
            body_digest.update(chunk)
            remaining -= len(chunk)
        if stream.read(32) != body_digest.digest():
            raise ValueError("desktop primary internal checksum mismatch")
    return {"filename": expected_name, "height": height, "sha256": digest, "profile": profile}


def verify_publisher(normalized_manifest: Path, publisher_manifest: Path,
                     publisher_signature: Path, expected_name: str,
                     publisher_key: Path | None = None) -> None:
    """Bind the unsigned runtime manifest to signed original publisher bytes."""
    key = publisher_key or Path(__file__).with_name("fleet-publisher-ed25519.pub.pem")
    result = subprocess.run(
        ["openssl", "pkeyutl", "-verify", "-pubin", "-inkey", str(key),
         "-rawin", "-in", str(publisher_manifest), "-sigfile", str(publisher_signature)],
        capture_output=True, check=False)
    if result.returncode != 0:
        raise ValueError("publisher manifest signature verification failed")
    original = json.loads(publisher_manifest.read_text(encoding="utf-8"))
    normalized = json.loads(normalized_manifest.read_text(encoding="utf-8"))
    if (original.get("network") != "mainnet" or
            original.get("format_version") != 6 or
            original.get("snapshot_profile") != "full"):
        raise ValueError("publisher manifest is not mainnet full-v6")
    if original.get("generated_at") != normalized.get("generated_at"):
        raise ValueError("publisher and runtime manifests differ in generated_at")
    source = original.get("snapshot")
    installed = normalized.get("snapshot")
    if (not isinstance(source, dict) or not isinstance(installed, dict) or
            source.get("snapshot_file") not in ("snapshot.dat", expected_name) or
            installed.get("snapshot_file") != expected_name):
        raise ValueError("publisher and runtime snapshot names differ")
    for field in ("height", "block_hash", "bytes", "sha256"):
        if source.get(field) != installed.get(field):
            raise ValueError(f"publisher and runtime manifests differ in {field}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--payload", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--expected-name", required=True)
    parser.add_argument("--catalog", type=Path, default=Path(__file__).with_name("desktop-inputs-candidate.json"))
    parser.add_argument("--publisher-manifest", type=Path)
    parser.add_argument("--publisher-signature", type=Path)
    args = parser.parse_args()
    try:
        if bool(args.publisher_manifest) != bool(args.publisher_signature):
            raise ValueError("publisher manifest and signature must be supplied together")
        result = verify(args.payload, args.manifest, args.catalog, args.expected_name)
        if args.publisher_manifest:
            verify_publisher(args.manifest, args.publisher_manifest,
                             args.publisher_signature, args.expected_name)
            result["publisher_signature_verified"] = True
        print(json.dumps(result, sort_keys=True))
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as exc:
        parser.exit(1, f"desktop full-v6 primary refused: {exc}\n")
