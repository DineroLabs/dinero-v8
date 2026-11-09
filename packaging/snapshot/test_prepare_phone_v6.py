import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).with_name('prepare_phone_v6.sh')


class PreparePhoneV6(unittest.TestCase):
    def run_case(self, tip, public=False, invalid=False):
        tmp = tempfile.TemporaryDirectory(); self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        cli = root / 'cli'
        cli.write_text('#!/bin/sh\necho ' + str(tip) + '\n'); cli.chmod(0o700)
        publisher = root / 'publisher'
        publisher.write_text('''#!/bin/bash
set -eu
touch "$DATADIR/publisher-invoked"
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
ln -s archive/candidate "$PUBROOT/latest"
''')
        env = dict(os.environ, CLI_BIN=str(cli), DATADIR=str(root), FORMAT='5' if invalid else '6',
                   PHONE_V6_PUBLISHER=str(publisher),
                   PHONE_V6_QUALIFICATION_ROOT='/var/www/dinero-snapshots' if public else str(root/'private'))
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
