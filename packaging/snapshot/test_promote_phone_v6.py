"""Real Ed25519/filesystem/HTTP gates with synthetic native evidence.

These tests exercise promotion refusal and artifact identity; synthetic NCPROBE
records are never evidence of a real phone import.
"""
import contextlib
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import threading
import unittest
from unittest.mock import patch

import promote_phone_v6 as promote


class Promotion(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name); self.generation = self.root/'private/archive/candidate'
        self.generation.mkdir(parents=True)
        self.key = self.root/'key.pem'
        subprocess.run(['openssl', 'genpkey', '-algorithm', 'ED25519', '-out', str(self.key)],
                       check=True, capture_output=True)
        public = subprocess.check_output(['openssl', 'pkey', '-in', str(self.key), '-pubout', '-outform', 'DER'])[-32:]
        self.key_patch = patch.object(promote, 'PUBLIC_KEY', public); self.key_patch.start()
        self.addCleanup(self.key_patch.stop)
        self.block = 'ab'*32; self.tip_hash = 'cd'*32; self.commitment = 'ef'*32
        section = b'ORCH'+struct.pack('<I', 1)+b'\x02'+bytes(1048576)
        payload = (b'UXTO'+struct.pack('<I', 6)+bytes.fromhex(self.block)[::-1]+
                   struct.pack('<IQQQ', 150000, 0, 0, 0)+struct.pack('<Q', len(section))+section+bytes(32))
        (self.generation/'snapshot.dat').write_bytes(payload)
        self.manifest = {'network': 'mainnet', 'format_version': 6, 'snapshot_profile': 'compact',
            'snapshot': {'height': 150000, 'block_hash': self.block, 'bytes': len(payload),
                         'sha256': hashlib.sha256(payload).hexdigest(), 'snapshot_file': 'snapshot.dat'}}
        (self.generation/'manifest.json').write_text(json.dumps(self.manifest))
        self.sign()
        self.staging = {'state': 'SERVER_NATIVE_IMPORT_PASSED_PHONE_QUALIFICATION_REQUIRED',
                        'generation': str(self.generation), 'snapshot': self.manifest['snapshot'],
                        'manifest_sha256': promote.digest(self.generation/'manifest.json')}
        self.staging_path = self.root/'staging.json'; self.save(self.staging_path, self.staging)
        self.phones = {platform: self.phone(platform) for platform in ('ios', 'android')}
        self.public = self.root/'public'; old = self.public/'archive/old'; old.mkdir(parents=True)
        (old/'manifest.json').write_text(json.dumps({'network': 'mainnet', 'format_version': 5,
             'snapshot': {'height': 128475}}))
        (self.public/'latest').symlink_to('archive/old')
        self.pin = self.public/'pinned/v5-h149999/snapshot.dat'; self.pin.parent.mkdir(parents=True)
        self.pin.write_bytes(b'permanent boundary bytes')
        self.report = self.root/'promotion.json'

    def save(self, path, value): path.write_text(json.dumps(value))

    def sign(self):
        subprocess.run(['openssl', 'pkeyutl', '-sign', '-inkey', str(self.key), '-rawin',
            '-in', str(self.generation/'manifest.json'), '-out', str(self.generation/'manifest.sig')],
            check=True, capture_output=True)

    def native_log(self, cold):
        bootstrap = {'snapshot_base_height': 150000, 'snapshot_base_block': self.block,
                     'configured_snapshot': not cold, 'manifest_required': not cold, 'fatal': False}
        accumulator = {'validation_role': 'compact_validator', 'retains_full_state': False,
                       'forest_memory_bytes_estimate': 0, 'verified_height': 150289,
                       'verified_block_hash': self.tip_hash, 'commitment': self.commitment}
        return ('NCPROBE\tVERSION\tdinerod 8.2.2-'+promote.CORE_SHA+'\nNCPROBE\tSTART\t0\n'+
            'NCPROBE\tgetsnapshotbootstrapstatus\t'+json.dumps({'snapshot_bootstrap': bootstrap})+'\n'+
            'NCPROBE\tblockchain.getutreexocommitment\t'+json.dumps(accumulator)+'\nNCPROBE\tSTOP\t0\n')

    def phone(self, platform):
        root = self.root/platform; root.mkdir()
        evidence = {}
        for name in ('initial_import', 'cold_restart', 'transport', 'negative_controls', 'fleet_agreement'):
            path = root/(name+'.log')
            if name in ('initial_import', 'cold_restart'):
                path.write_text(self.native_log(cold=name == 'cold_restart'))
            elif name == 'fleet_agreement':
                self.save(path, {'network': 'mainnet', 'checkpoints': [{'height': 150289,
                    'block_hash': self.tip_hash, 'commitment': self.commitment, 'observers': ['EU1', 'SJ']}]})
            else: path.write_text('synthetic '+platform+' '+name+' evidence; unit test only\n')
            evidence[name] = {'path': path.name, 'sha256': promote.digest(path)}
        receipt = {'schema': 'dinero.phone-snapshot-qualification.v1', 'state': 'PASS_NATIVE_COMPACT_V6',
                   'platform': platform, 'network': 'mainnet', 'native_source_sha': promote.CORE_SHA,
                   'app_version': 'fixture only', 'payload_sha256': self.staging['snapshot']['sha256'],
                   'manifest_sha256': self.staging['manifest_sha256'], 'base_height': 150000,
                   'base_hash': self.block, 'forward_height': 150289,
                   'checks': {check: True for check in promote.CHECKS}, 'validation_role': 'compact_validator',
                   'retains_full_state': False, 'forest_memory_bytes_estimate': 0,
                   'independent_observers': ['EU1', 'SJ'], 'evidence': evidence}
        path = root/'receipt.json'; self.save(path, receipt); return path

    def run_main(self, dry=False, mirror_error=False):
        argv = ['promote', '--staging-receipt', str(self.staging_path), '--ios-receipt', str(self.phones['ios']),
                '--android-receipt', str(self.phones['android']), '--pubroot', str(self.public), '--report', str(self.report)]
        if dry: argv.append('--dry-run')
        def mirrors(staging, report, path, *_):
            if mirror_error: raise ValueError('one mirror still old')
            report['state'] = 'PROMOTED_ALL_MIRRORS_VERIFIED'; promote.write_report(path, report)
        with patch('sys.argv', argv), patch.object(promote, 'verify_chain'), \
             patch.object(promote, 'verify_mirrors', side_effect=mirrors), contextlib.redirect_stdout(io.StringIO()):
            return promote.main()

    def assert_refused(self):
        self.assertEqual(self.run_main(), 1)
        self.assertEqual((self.public/'latest').readlink(), Path('archive/old'))
        self.assertEqual(json.loads(self.report.read_text())['state'], 'REFUSED_BEFORE_PROMOTION')

    def test_exact_signed_set_and_both_native_receipts_pass(self):
        self.assertEqual(self.run_main(), 0)
        latest = (self.public/'latest').resolve()
        self.assertEqual(promote.digest(latest/'snapshot.dat'), self.staging['snapshot']['sha256'])
        self.assertEqual(promote.digest(latest/'manifest.json'), self.staging['manifest_sha256'])
        self.assertEqual(self.pin.read_bytes(), b'permanent boundary bytes')
        self.assertFalse((latest/'receipt.json').exists())
        self.assertEqual(json.loads(self.report.read_text())['state'], 'PROMOTED_ALL_MIRRORS_VERIFIED')
        self.assertEqual(self.run_main(), 0, 'idempotent retry must verify existing immutable generation')

    def test_dry_run_does_not_change_public_root(self):
        before = sorted(str(p.relative_to(self.public)) for p in self.public.rglob('*'))
        self.assertEqual(self.run_main(dry=True), 0)
        self.assertEqual(before, sorted(str(p.relative_to(self.public)) for p in self.public.rglob('*')))
        self.assertEqual((self.public/'latest').readlink(), Path('archive/old'))

    def test_payload_tamper_refused(self):
        with (self.generation/'snapshot.dat').open('ab') as stream: stream.write(b'changed')
        self.assert_refused()

    def test_manifest_tamper_refused_even_when_resigned(self):
        (self.generation/'manifest.json').write_text(json.dumps(self.manifest, indent=2)); self.sign()
        self.assert_refused()

    def test_present_but_bad_signature_refused(self):
        (self.generation/'manifest.sig').write_bytes(bytes(64)); self.assert_refused()

    def test_signature_from_another_key_refused(self):
        with patch.object(promote, 'PUBLIC_KEY', bytes(32)): self.assert_refused()

    def test_signed_full_container_cannot_claim_compact_profile(self):
        payload = bytearray((self.generation/'snapshot.dat').read_bytes()); payload[84] = 1
        (self.generation/'snapshot.dat').write_bytes(payload)
        self.manifest['snapshot']['sha256'] = hashlib.sha256(payload).hexdigest()
        self.save(self.generation/'manifest.json', self.manifest); self.sign()
        self.staging['snapshot'] = self.manifest['snapshot']
        self.staging['manifest_sha256'] = promote.digest(self.generation/'manifest.json')
        with self.assertRaisesRegex(ValueError, 'header/profile'):
            promote.verify_generation(self.generation, self.staging)

    def test_missing_phone_receipt_refused(self):
        self.phones['android'].unlink(); self.assert_refused()

    def test_wrong_platform_or_snapshot_or_core_refused(self):
        original = json.loads(self.phones['ios'].read_text())
        for key, value in (('platform', 'android'), ('payload_sha256', '00'*32),
                           ('manifest_sha256', '00'*32), ('native_source_sha', '0'*40)):
            with self.subTest(key=key):
                altered = dict(original); altered[key] = value; self.save(self.phones['ios'], altered)
                self.assert_refused()

    def test_startup_or_transport_only_receipt_refused(self):
        receipt = json.loads(self.phones['ios'].read_text())
        receipt['checks']['native_import_verified'] = False; self.save(self.phones['ios'], receipt)
        self.assert_refused()

    def test_changed_or_missing_execution_evidence_refused(self):
        (self.phones['android'].parent/'cold_restart.log').write_text('different execution')
        self.assert_refused()

    def test_receipt_booleans_cannot_substitute_for_native_import(self):
        path = self.phones['ios']; receipt = json.loads(path.read_text())
        evidence = path.parent/'initial_import.log'
        evidence.write_text('NCPROBE\tVERSION\tdinerod 8.2.2-'+promote.CORE_SHA+
                            '\nNCPROBE\tSTART\t0\nNCPROBE\tSTOP\t0\n')
        receipt['evidence']['initial_import']['sha256'] = promote.digest(evidence); self.save(path, receipt)
        self.assert_refused()

    def test_transient_busy_rpc_does_not_hide_later_native_proof(self):
        path = self.phones['ios']; receipt = json.loads(path.read_text())
        evidence = path.parent/'initial_import.log'
        evidence.write_text('NCPROBE\tgetsnapshotbootstrapstatus\tnull\n'+
                            'NCPROBE\tblockchain.getutreexocommitment\tnull\n'+evidence.read_text())
        receipt['evidence']['initial_import']['sha256'] = promote.digest(evidence)
        self.save(path, receipt)
        self.assertEqual(self.run_main(), 0)

    def test_cold_reimport_or_native_fallback_is_not_qualification(self):
        original = self.phones['ios'].read_text()
        evidence = self.phones['ios'].parent/'cold_restart.log'
        for log in (self.native_log(cold=False), self.native_log(cold=True)+'[snapshot] rejected\n'):
            with self.subTest(log=log[-40:]):
                receipt = json.loads(original); evidence.write_text(log)
                receipt['evidence']['cold_restart']['sha256'] = promote.digest(evidence)
                self.save(self.phones['ios'], receipt); self.assert_refused()

    def test_fleet_accumulator_disagreement_refused(self):
        path = self.phones['android']; receipt = json.loads(path.read_text()); evidence = path.parent/'fleet_agreement.log'
        agreement = json.loads(evidence.read_text()); agreement['checkpoints'][0]['commitment'] = '00'*32
        self.save(evidence, agreement); receipt['evidence']['fleet_agreement']['sha256'] = promote.digest(evidence)
        self.save(path, receipt); self.assert_refused()

    def test_source_reorg_or_insufficient_burial_refused_before_swap(self):
        cli = self.root/'cli'
        def configure(tip, block):
            cli.write_text('#!/usr/bin/env python3\nimport json,sys\nprint(json.dumps('+str(tip)+
                           ' if sys.argv[2]=="getblockcount" else '+repr(block)+'))\n')
            cli.chmod(0o700)
        configure(150288, self.block)
        promote.verify_chain(str(cli), self.root, self.staging['snapshot'])
        for tip, block in ((150287, self.block), (150288, '00'*32)):
            configure(tip, block)
            with self.assertRaises(ValueError):
                promote.verify_chain(str(cli), self.root, self.staging['snapshot'])
        def reorg(): raise ValueError('base left selected chain')
        with self.assertRaises(ValueError):
            promote.install_generation(self.public, self.generation, self.staging, {}, reorg)
        self.assertEqual((self.public/'latest').readlink(), Path('archive/old'))

    def test_error_after_swap_cannot_report_preflight_refusal(self):
        sync = promote.sync_directory
        def fail_public_sync(path):
            if Path(path) == self.public: raise OSError('simulated public-root fsync failure')
            return sync(path)
        with patch.object(promote, 'sync_directory', side_effect=fail_public_sync):
            self.assertEqual(self.run_main(), 1)
        report = json.loads(self.report.read_text())
        self.assertTrue(report['public_promotion_performed'])
        self.assertEqual(report['state'], 'LOCAL_PROMOTED_MIRROR_VERIFICATION_FAILED')

    def test_mirror_failure_is_incomplete_rollout_not_preflight_refusal(self):
        self.assertEqual(self.run_main(mirror_error=True), 1)
        report = json.loads(self.report.read_text())
        self.assertTrue(report['public_promotion_performed'])
        self.assertEqual(report['state'], 'LOCAL_PROMOTED_MIRROR_VERIFICATION_FAILED')
        self.assertNotEqual((self.public/'latest').readlink(), Path('archive/old'))
        self.assertEqual(self.pin.read_bytes(), b'permanent boundary bytes')

    def test_full_endpoint_is_not_repurposed(self):
        (self.public/'.snapshot-profile').write_text('full\n'); self.assert_refused()

    def test_height_rollback_refused(self):
        current = json.loads((self.public/'latest/manifest.json').read_text()); current['snapshot']['height'] = 150001
        self.save(self.public/'latest/manifest.json', current); self.assert_refused()

    def test_real_http_checks_three_mirrors_and_rejects_one_stale_or_corrupt(self):
        files = {name: (self.generation/name).read_bytes() for name in ('manifest.json', 'manifest.sig', 'snapshot.dat')}
        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                data = self.server.files.get(self.path[1:])
                if data is None: self.send_error(404); return
                self.send_response(200); self.send_header('Content-Length', str(len(data))); self.end_headers(); self.wfile.write(data)
            def log_message(self, *_): pass
        servers = []
        for _ in range(3):
            server = ThreadingHTTPServer(('127.0.0.1', 0), Handler); server.files = dict(files)
            threading.Thread(target=server.serve_forever, daemon=True).start(); servers.append(server)
            self.addCleanup(server.server_close); self.addCleanup(server.shutdown)
        urls = tuple('http://127.0.0.1:'+str(server.server_port) for server in servers)
        report = {'state': 'LOCAL_PROMOTED_VERIFYING_MIRRORS'}
        promote.verify_mirrors(self.staging, report, self.report, 0, 1, mirrors=urls)
        self.assertEqual(len(report['mirrors']), 3)
        for name, bad in (('manifest.json', b'{}'), ('manifest.sig', bytes(64)), ('snapshot.dat', b'bad payload')):
            with self.subTest(name=name):
                servers[1].files = dict(files); servers[1].files[name] = bad
                with self.assertRaises(ValueError):
                    promote.verify_mirrors(self.staging, report, self.report, 0, 1, mirrors=urls)
                self.assertEqual(report['mirrors'][urls[1]]['state'], 'NOT_VERIFIED')


if __name__ == '__main__': unittest.main()
