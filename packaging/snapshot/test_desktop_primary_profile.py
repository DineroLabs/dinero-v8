"""Package admission must not mistake a phone compact v6 for desktop full v6."""

import hashlib
import json
import datetime as dt
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
from unittest import mock
import sys

from check_desktop_primary import verify, verify_recent_fallback, verify_publisher
from fetch_desktop_primary import fetch, helper_dirs


class DesktopPrimaryProfileTest(unittest.TestCase):
    name = "dinero-assumeutxo-159922-v6-full.dat"
    block_hash = "ab" * 32

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.payload = self.root / self.name
        self.manifest = self.root / (self.name + ".manifest.json")
        self.catalog = self.root / "catalog.json"
        self.write_fixture(profile=1)

    def write_fixture(self, profile):
        header = bytearray(68)
        header[:4] = b"UXTO"
        struct.pack_into("<I", header, 4, 6)
        header[8:40] = bytes.fromhex(self.block_hash)[::-1]
        struct.pack_into("<I", header, 40, 159922)
        section = b"ORCH" + struct.pack("<I", 1) + bytes([profile])
        body = bytes(header) + struct.pack("<Q", len(section)) + section
        self.payload.write_bytes(body + hashlib.sha256(body).digest())
        identity = {"snapshot_file": self.name, "height": 159922,
                    "block_hash": self.block_hash, "bytes": self.payload.stat().st_size,
                    "sha256": hashlib.sha256(self.payload.read_bytes()).hexdigest()}
        self.manifest.write_text(json.dumps({"network": "mainnet", "format_version": 6,
                                             "generated_at": dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ"),
                                             "snapshot_profile": "full", "snapshot": identity}))
        self.catalog.write_text(json.dumps({"schema": "dinero.desktop-snapshot-inputs.v1",
                                            "primary": {"filename": self.name, "format_version": 6,
                                                        "snapshot_profile": "full",
                                                        **{k: identity[k] for k in
                                                           ("height", "block_hash", "bytes", "sha256")}}}))

    def test_exact_full_v6_transport_pin_passes(self):
        self.assertEqual(verify(self.payload, self.manifest, self.catalog, self.name)["profile"], "full")

    def test_transport_hash_works_without_python_311_file_digest(self):
        # Ubuntu 22.04's system Python is 3.10; package admission must work there.
        with mock.patch.object(hashlib, "file_digest", side_effect=AssertionError("requires Python 3.11"), create=True):
            self.assertEqual(verify(self.payload, self.manifest, self.catalog, self.name)["profile"], "full")

    def test_docker_shallow_verify_layout_imports(self):
        self.assertEqual(helper_dirs(Path("/verify/fetch_desktop_primary.py"))[0], Path("/verify"))
        verify_dir = self.root / "verify"
        verify_dir.mkdir()
        source_dir = Path(__file__).resolve().parent
        for name in ("fetch_desktop_primary.py", "check_desktop_primary.py", "snapshot_transport.py"):
            shutil.copy2(source_dir / name, verify_dir / name)
        shutil.copy2(source_dir.parent.parent / "scripts/ci/check_snapshot_freshness.py",
                     verify_dir / "check_snapshot_freshness.py")
        process = subprocess.run([sys.executable, str(verify_dir / "fetch_desktop_primary.py"), "--help"],
                                 capture_output=True, text=True, check=False)
        self.assertEqual(process.returncode, 0, process.stderr)

    def test_compact_v6_payload_refused(self):
        self.write_fixture(profile=2)
        with self.assertRaisesRegex(ValueError, "not the pinned full-v6 base"):
            verify(self.payload, self.manifest, self.catalog, self.name)

    def test_manifest_claim_without_full_profile_refused(self):
        data = json.loads(self.manifest.read_text())
        data["snapshot_profile"] = "compact"
        self.manifest.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, "not mainnet full-v6"):
            verify(self.payload, self.manifest, self.catalog, self.name)

    def test_payload_different_from_catalog_refused(self):
        data = json.loads(self.catalog.read_text())
        data["primary"]["sha256"] = "00" * 32
        self.catalog.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, "pinned sha256"):
            verify(self.payload, self.manifest, self.catalog, self.name)

    def test_legacy_catalog_cannot_authorize_new_primary(self):
        data = json.loads(self.catalog.read_text())
        data["primary"]["format_version"] = 5
        with self.assertRaisesRegex(ValueError, "not the expected full-v6"):
            self.catalog.write_text(json.dumps(data))
            verify(self.payload, self.manifest, self.catalog, self.name)

    def test_wrong_height_catalog_cannot_authorize_primary(self):
        data = json.loads(self.catalog.read_text())
        data["primary"]["height"] = 153577
        self.catalog.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, "not the expected full-v6"):
            verify(self.payload, self.manifest, self.catalog, self.name)

    def test_matching_pin_cannot_authorize_broken_internal_checksum(self):
        data = bytearray(self.payload.read_bytes())
        data[-1] ^= 1
        self.payload.write_bytes(data)
        identity = json.loads(self.manifest.read_text())
        catalog = json.loads(self.catalog.read_text())
        for field in ("bytes", "sha256"):
            value = len(data) if field == "bytes" else hashlib.sha256(data).hexdigest()
            identity["snapshot"][field] = value
            catalog["primary"][field] = value
        self.manifest.write_text(json.dumps(identity))
        self.catalog.write_text(json.dumps(catalog))
        with self.assertRaisesRegex(ValueError, "internal checksum mismatch"):
            verify(self.payload, self.manifest, self.catalog, self.name)

    def test_pre_release_fetch_normalizes_publisher_name(self):
        source = json.loads(self.manifest.read_text())
        source["snapshot"]["snapshot_file"] = "snapshot.dat"
        self.manifest.write_text(json.dumps(source))
        destination = self.root / "staged"
        payload, manifest = fetch(self.payload.as_uri(), self.manifest.as_uri(),
                                  self.catalog, destination, allow_file=True,
                                  allow_unsigned_fixture=True)
        self.assertEqual(payload.name, self.name)
        self.assertEqual(json.loads(manifest.read_text())["snapshot"]["snapshot_file"], self.name)
        self.assertEqual(verify(payload, manifest, self.catalog, self.name)["profile"], "full")

    def test_pre_release_fetch_refuses_unpinned_source(self):
        catalog = json.loads(self.catalog.read_text())
        catalog["primary"]["format_version"] = 5
        self.catalog.write_text(json.dumps(catalog))
        destination = self.root / "refused"
        with self.assertRaisesRegex(ValueError, "desktop primary pin"):
            fetch(self.payload.as_uri(), self.manifest.as_uri(), self.catalog,
                  destination, allow_file=True)
        self.assertFalse(destination.exists())

    def test_missing_publisher_signature_refuses_default_fetch(self):
        source = json.loads(self.manifest.read_text())
        source["snapshot"]["snapshot_file"] = "snapshot.dat"
        self.manifest.write_text(json.dumps(source))
        destination = self.root / "unsigned-refused"
        with self.assertRaisesRegex(ValueError, "publisher manifest signature is required"):
            fetch(self.payload.as_uri(), self.manifest.as_uri(), self.catalog,
                  destination, allow_file=True)
        self.assertFalse((destination / self.name).exists())
        self.assertFalse((destination / (self.name + ".manifest.json")).exists())

    @unittest.skipUnless(shutil.which("openssl"), "OpenSSL CLI unavailable")
    def test_signed_publisher_bytes_retained_separately(self):
        source = json.loads(self.manifest.read_text())
        source["snapshot"]["snapshot_file"] = "snapshot.dat"
        self.manifest.write_text(json.dumps(source))
        private = self.root / "publisher.key"
        public = self.root / "publisher.pub"
        signature = self.manifest.with_suffix(".sig")
        subprocess.run(["openssl", "genpkey", "-algorithm", "Ed25519", "-out", str(private)], check=True, capture_output=True)
        subprocess.run(["openssl", "pkey", "-in", str(private), "-pubout", "-out", str(public)], check=True, capture_output=True)
        subprocess.run(["openssl", "pkeyutl", "-sign", "-inkey", str(private), "-rawin",
                        "-in", str(self.manifest), "-out", str(signature)], check=True, capture_output=True)
        destination = self.root / "signed"
        fetch(self.payload.as_uri(), self.manifest.as_uri(), self.catalog, destination,
              publisher_key=public, allow_file=True)
        self.assertEqual((destination / (self.name + ".publisher.manifest.json")).read_bytes(),
                         self.manifest.read_bytes())
        self.assertEqual((destination / (self.name + ".publisher.manifest.sig")).read_bytes(),
                         signature.read_bytes())
        receipt = json.loads((destination / (self.name + ".publisher.receipt.json")).read_text())
        self.assertTrue(receipt["publisher_signature_verified"])
        staged_manifest = destination / (self.name + ".manifest.json")
        verify_publisher(staged_manifest, destination / (self.name + ".publisher.manifest.json"),
                         destination / (self.name + ".publisher.manifest.sig"), self.name, public)
        rewritten = json.loads(staged_manifest.read_text())
        rewritten["snapshot"]["height"] += 1
        staged_manifest.write_text(json.dumps(rewritten))
        with self.assertRaisesRegex(ValueError, "differ in height"):
            verify_publisher(staged_manifest, destination / (self.name + ".publisher.manifest.json"),
                             destination / (self.name + ".publisher.manifest.sig"), self.name, public)
        rewritten = json.loads(staged_manifest.read_text())
        rewritten["snapshot"]["height"] -= 1
        rewritten["generated_at"] = "20990101T000000Z"
        staged_manifest.write_text(json.dumps(rewritten))
        with self.assertRaisesRegex(ValueError, "differ in generated_at"):
            verify_publisher(staged_manifest, destination / (self.name + ".publisher.manifest.json"),
                             destination / (self.name + ".publisher.manifest.sig"), self.name, public)
        # The installed manifest has a different filename and no attached sig.
        self.assertNotEqual((destination / (self.name + ".manifest.json")).read_bytes(),
                            self.manifest.read_bytes())

    @unittest.skipUnless(shutil.which("openssl"), "OpenSSL CLI unavailable")
    def test_signed_h153051_exact_lifecycle_fallback_is_staged_and_pinned(self):
        fallback_name = "dinero-assumeutxo-153051-v6-full.dat"
        old_payload = self.payload.read_bytes()
        body = bytearray(old_payload[:-32])
        struct.pack_into("<I", body, 40, 153051)
        self.payload.write_bytes(body + hashlib.sha256(body).digest())
        identity = {"snapshot_file": "snapshot.dat", "height": 153051,
                    "block_hash": self.block_hash, "bytes": self.payload.stat().st_size,
                    "sha256": hashlib.sha256(self.payload.read_bytes()).hexdigest()}
        source = json.loads(self.manifest.read_text())
        source.update(generated_at="20240101T000000Z", snapshot=identity)
        self.manifest.write_text(json.dumps(source))
        catalog = json.loads(self.catalog.read_text())
        catalog["fallbacks"] = [{"filename": fallback_name,
                                 "format_version": 6, "snapshot_profile": "full",
                                 **{key: identity[key] for key in
                                    ("height", "block_hash", "bytes", "sha256")}}]
        self.catalog.write_text(json.dumps(catalog))
        private = self.root / "fallback.key"
        public = self.root / "fallback.pub"
        signature = self.manifest.with_suffix(".sig")
        subprocess.run(["openssl", "genpkey", "-algorithm", "Ed25519", "-out", str(private)], check=True, capture_output=True)
        subprocess.run(["openssl", "pkey", "-in", str(private), "-pubout", "-out", str(public)], check=True, capture_output=True)
        subprocess.run(["openssl", "pkeyutl", "-sign", "-inkey", str(private), "-rawin",
                        "-in", str(self.manifest), "-out", str(signature)], check=True, capture_output=True)
        destination = self.root / "fallback-stage"
        payload, normalized = fetch(self.payload.as_uri(), self.manifest.as_uri(),
                                    self.catalog, destination, publisher_key=public,
                                    allow_file=True, fallback_height=153051)
        self.assertEqual(payload.name, fallback_name)
        self.assertEqual(verify_recent_fallback(payload, normalized, self.catalog, fallback_name)["profile"], "full")
        verify_publisher(normalized, destination / (fallback_name + ".publisher.manifest.json"),
                         destination / (fallback_name + ".publisher.manifest.sig"), fallback_name, public)
        self.assertEqual(json.loads(normalized.read_text())["snapshot"]["snapshot_file"], fallback_name)
        catalog["fallbacks"] = []
        self.catalog.write_text(json.dumps(catalog))
        refused = self.root / "fallback-refused"
        with self.assertRaisesRegex(ValueError, "H153051 full-v6 fallback pin is unavailable"):
            fetch(self.payload.as_uri(), self.manifest.as_uri(), self.catalog,
                  refused, publisher_key=public, allow_file=True, fallback_height=153051)
        self.assertFalse(refused.exists())


if __name__ == "__main__":
    unittest.main()
