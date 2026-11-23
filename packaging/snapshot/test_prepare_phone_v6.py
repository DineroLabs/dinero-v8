import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).with_name('prepare_phone_v6.sh')


class PreparePhoneV6(unittest.TestCase):
    def run_case(self, tip, public=False, invalid=False, previous=None, publishes=True, receipt=None):
        tmp = tempfile.TemporaryDirectory(); self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        private = root/'private'; served = root/'public-compact-v6'
        if previous:
            # An older generation from an earlier cycle, already behind latest.
            old = private/'archive/old'; old.mkdir(parents=True)
            manifest = json.dumps({'network':'mainnet','format_version':6,'snapshot_profile':'compact',
                                   'snapshot':{'height':150074}})
            (old/'manifest.json').write_text(manifest)
            (private/'latest').symlink_to('archive/old')
            if previous == 'promoted':
                (served/'latest').mkdir(parents=True)
                (served/'latest/manifest.json').write_text(manifest)
            if receipt:
                import hashlib
                (private/'phone-qualification-required.json').write_text(json.dumps({
                    'state':'SERVER_NATIVE_IMPORT_PASSED_PHONE_QUALIFICATION_REQUIRED',
                    'generation':str(old),'snapshot':{'height':150074},
                    'manifest_sha256':hashlib.sha256(manifest.encode()).hexdigest()}))
        cli = root / 'cli'
        cli.write_text('#!/bin/sh\necho ' + str(tip) + '\n'); cli.chmod(0o700)
        publisher = root / 'publisher'
        publisher.write_text('''#!/bin/bash
set -eu
touch "$DATADIR/publisher-invoked"
if [ "$PUBLISHES" != 1 ]; then echo "retaining v6 base; waiting for tip"; exit 0; fi
test "$SNAPSHOT_PROFILE" = compact
test "$SNAPSHOT_COMPACT_PENDING_NAMESPACE" = phone-v6-qualification
mkdir -p "$PUBROOT/archive/candidate"
export CANDIDATE="$PUBROOT/archive/candidate"
python3 - <<'PY'
import hashlib,json,os,pathlib
p=pathlib.Path(os.environ['CANDIDATE']);payload=b'qualification fixture'
(p/'snapshot.dat').write_bytes(payload);(p/'manifest.sig').write_bytes(bytes(64))
(p/'manifest.json').write_text(json.dumps({'network':'mainnet','format_version':int(os.environ['FORMAT']),
 'snapshot_profile':'compact','snapshot':{'height':150000,'snapshot_file':'snapshot.dat',
 'sha256':hashlib.sha256(payload).hexdigest(),'bytes':len(payload),'block_hash':'ab'*32}}))
PY
ln -sfn archive/candidate "$PUBROOT/latest"
''')
        env = dict(os.environ, CLI_BIN=str(cli), DATADIR=str(root), FORMAT='5' if invalid else '6',
                   PHONE_V6_PUBLISHER=str(publisher), PUBLISHES='1' if publishes else '0',
                   PHONE_V6_PUBLIC_ROOT=str(served),
                   PHONE_V6_QUALIFICATION_ROOT='/var/www/dinero-snapshots' if public else str(private))
        result = subprocess.run(['bash', str(SCRIPT)], env=env, capture_output=True, text=True, timeout=10)
        return root, result

    def test_before_activation_does_not_export_or_publish(self):
        root, result = self.run_case(149999)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('WAIT_ACTIVATION', result.stdout)
        self.assertFalse((root/'publisher-invoked').exists())

    def test_prepared_receipt_requires_both_phone_qualifications(self):
        root, result = self.run_case(150288)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        receipt = json.loads((root/'private/phone-qualification-required.json').read_text())
        self.assertFalse(receipt['ios_qualified'])
        self.assertFalse(receipt['android_qualified'])
        self.assertFalse(receipt['public_promotion_performed'])
        self.assertEqual(receipt['snapshot']['height'], 150000)

    def test_public_root_is_refused_before_export(self):
        root, result = self.run_case(150288, public=True)
        self.assertEqual(result.returncode, 2)
        self.assertFalse((root/'publisher-invoked').exists())

    def test_legacy_candidate_cannot_get_ready_receipt(self):
        root, result = self.run_case(150288, invalid=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((root/'private/phone-qualification-required.json').exists())

    def test_waiting_run_never_receipts_an_older_generation(self):
        # 2026-11-23 EU1: after an export the publisher only waited for burial,
        # and the old H150074 latest received a fresh qualification receipt.
        root, result = self.run_case(164853, previous='promoted', publishes=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('WAIT_BURIAL', result.stdout)
        self.assertFalse((root/'private/phone-qualification-required.json').exists())

    def test_promoted_receipt_rearms_preparation(self):
        root, result = self.run_case(164853, previous='promoted', publishes=False, receipt=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue((root/'publisher-invoked').exists())
        self.assertFalse((root/'private/phone-qualification-required.json').exists())
        archived = list((root/'private').glob('phone-qualification-required.h150074-promoted-*.json'))
        self.assertEqual(len(archived), 1)

    def test_unpromoted_receipt_still_waits_for_phone_qualification(self):
        root, result = self.run_case(164853, previous='pending', publishes=False, receipt=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('qualification still required', result.stdout)
        self.assertFalse((root/'publisher-invoked').exists())
        self.assertTrue((root/'private/phone-qualification-required.json').exists())

    def test_new_generation_after_older_latest_gets_receipt(self):
        root, result = self.run_case(165141, previous='promoted', publishes=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        receipt = json.loads((root/'private/phone-qualification-required.json').read_text())
        self.assertEqual(receipt['snapshot']['height'], 150000)
        self.assertTrue(receipt['generation'].endswith('archive/candidate'))
