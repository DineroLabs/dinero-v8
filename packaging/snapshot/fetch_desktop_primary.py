#!/usr/bin/env python3
"""Stage an exact pinned full-v6 desktop primary from qualified HTTPS inputs.

The publisher serves snapshot.dat/manifest.json/manifest.sig. Production
staging requires a valid detached signature under the pinned fleet public key,
then stages the payload under the release filename and adjusts only the
manifest's snapshot_file field. The catalog pin, payload profile, internal
checksum, and freshness are checked before either output appears. Native
import/restart qualification is separate.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import urlopen

# Source checkouts keep the freshness helper under scripts/ci; the prepared
# Docker fetch stage copies it beside this script under /verify.
def helper_dirs(script_path: Path) -> tuple[Path, Path]:
    script_dir = script_path.parent
    return script_dir, script_dir.parent.parent / "scripts/ci"


script_dir, source_helpers = helper_dirs(Path(__file__).resolve())
sys.path.insert(0, str(script_dir))
if source_helpers.is_dir():
    sys.path.insert(0, str(source_helpers))
from check_snapshot_freshness import validate as check_freshness
from check_desktop_primary import verify


def _download(url: str, destination: Path, limit: int) -> None:
    with urlopen(url, timeout=60) as response, destination.open("wb") as stream:
        remaining = limit
        while True:
            chunk = response.read(min(1024 * 1024, remaining + 1))
            if not chunk:
                break
            remaining -= len(chunk)
            if remaining < 0:
                raise ValueError("desktop snapshot input exceeds pinned length")
            stream.write(chunk)


def fetch(payload_url: str, manifest_url: str, catalog_path: Path, destination: Path,
          *, signature_url: str | None = None, publisher_key: Path | None = None,
          allow_file: bool = False, allow_unsigned_fixture: bool = False) -> tuple[Path, Path]:
    if allow_unsigned_fixture and not allow_file:
        raise ValueError("unsigned fixture mode requires local file inputs")
    if signature_url is None:
        if not manifest_url.endswith(".json"):
            raise ValueError("publisher manifest URL must end in .json")
        signature_url = manifest_url[:-5] + ".sig"
    for url in (payload_url, manifest_url, signature_url):
        scheme = urlsplit(url).scheme
        if scheme != "https" and not (allow_file and scheme == "file"):
            raise ValueError("snapshot input URL must use HTTPS")
    catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
    pin = catalog["primary"]
    if (pin.get("filename") != "dinero-assumeutxo-152700-v6-full.dat" or
            pin.get("height") != 152700 or pin.get("format_version") != 6 or
            pin.get("snapshot_profile") != "full"):
        raise ValueError("qualified H152700 full-v6 catalog pin is unavailable")
    if type(pin.get("bytes")) is not int or pin["bytes"] < 109 or pin["bytes"] > 536870912:
        raise ValueError("invalid pinned full-v6 payload length")
    destination.mkdir(parents=True, exist_ok=True)
    payload_out = destination / pin["filename"]
    manifest_out = destination / (pin["filename"] + ".manifest.json")
    publisher_out = destination / (pin["filename"] + ".publisher.manifest.json")
    signature_out = destination / (pin["filename"] + ".publisher.manifest.sig")
    if any(path.exists() for path in (payload_out, manifest_out, publisher_out, signature_out)):
        raise ValueError("desktop snapshot destination already contains primary files")
    with tempfile.TemporaryDirectory(prefix=".desktop-primary-", dir=destination) as work:
        root = Path(work)
        source_manifest = root / "source-manifest.json"
        _download(manifest_url, source_manifest, 1024 * 1024)
        source_signature = root / "source-manifest.sig"
        signature_verified = False
        try:
            _download(signature_url, source_signature, 1024)
        except HTTPError as exc:
            if not (allow_unsigned_fixture and exc.code == 404):
                raise ValueError("publisher manifest signature is required and must be available") from exc
        except URLError as exc:
            if not (allow_unsigned_fixture and isinstance(exc.reason, FileNotFoundError)):
                raise ValueError("publisher manifest signature is required and must be available") from exc
        else:
            key = publisher_key or Path(__file__).with_name("fleet-publisher-ed25519.pub.pem")
            result = subprocess.run(
                ["openssl", "pkeyutl", "-verify", "-pubin", "-inkey", str(key),
                 "-rawin", "-in", str(source_manifest), "-sigfile", str(source_signature)],
                capture_output=True, check=False)
            if result.returncode != 0:
                raise ValueError("publisher manifest signature verification failed")
            signature_verified = True
        if not signature_verified and not allow_unsigned_fixture:
            raise ValueError("publisher manifest signature is required")
        manifest = json.loads(source_manifest.read_text(encoding="utf-8"))
        identity = manifest.get("snapshot")
        if not isinstance(identity, dict) or identity.get("snapshot_file") not in (
                "snapshot.dat", pin["filename"]):
            raise ValueError("publisher snapshot filename is unexpected")
        check_freshness(manifest)  # Age-only; burial is a separate native gate.
        identity["snapshot_file"] = pin["filename"]
        staged_manifest = root / (pin["filename"] + ".manifest.json")
        staged_manifest.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        staged_payload = root / pin["filename"]
        _download(payload_url, staged_payload, pin["bytes"])
        verify(staged_payload, staged_manifest, catalog_path, pin["filename"])
        # The normalized manifest is deliberately unsigned. Keep the original
        # publisher bytes and any verified detached signature as separate
        # provenance artifacts; never attach the signature to rewritten JSON.
        os.replace(staged_payload, payload_out)
        os.replace(staged_manifest, manifest_out)
        os.replace(source_manifest, publisher_out)
        if signature_verified:
            os.replace(source_signature, signature_out)
        (destination / (pin["filename"] + ".publisher.receipt.json")).write_text(
            json.dumps({"source_manifest_sha256": hashlib.sha256(publisher_out.read_bytes()).hexdigest(),
                        "publisher_signature_verified": signature_verified}, indent=2) + "\n",
            encoding="utf-8")
    return payload_out, manifest_out


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--payload-url", required=True)
    parser.add_argument("--manifest-url", required=True)
    parser.add_argument("--signature-url", help="publisher detached signature URL; inferred from manifest URL if absent")
    parser.add_argument("--destination", type=Path, required=True)
    parser.add_argument("--catalog", type=Path, default=Path(__file__).with_name("desktop-inputs-candidate.json"))
    args = parser.parse_args()
    try:
        print(*fetch(args.payload_url, args.manifest_url, args.catalog, args.destination,
                     signature_url=args.signature_url))
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError, URLError) as exc:
        parser.exit(1, f"desktop full-v6 fetch refused: {exc}\n")
