"""Exercise the real publisher preflight with isolated RPC/locking fixtures."""
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).with_name('dinero-snapshot-publish.sh')

class PublisherTransition(unittest.TestCase):
    def run_case(self, height, version, tip, canonical=True):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); data = root/'data'; dumps=data/'snapshots'; dumps.mkdir(parents=True)
            block='ab'*32
            (dumps/'pending.dat').write_bytes(b'UXTO'+struct.pack('<I',version))
            (dumps/'pending.json').write_text(json.dumps({'base_height':height,'base_hash':block}))
            bin_dir=root/'bin';bin_dir.mkdir()
            for name in ('flock','chown'):
                p=bin_dir/name;p.write_text('#!/bin/sh\nexit 0\n');p.chmod(0o755)
            cli=bin_dir/'cli';cli.write_text('#!/bin/sh\ncase "$2" in\ngetblockhash) echo "'+(block if canonical else 'cd'*32)+'";;\ngetblockcount) echo '+str(tip)+';;\n*) exit 99;;\nesac\n');cli.chmod(0o755)
            # Stop before artifact generation: no signing, real node or publication.
            script=SCRIPT.read_text().split('BYTES=$(stat',1)[0]+'echo PREFLIGHT_READY\n'
            env=dict(os.environ, DATADIR=str(data),PUBROOT=str(root/'www'),CLI_BIN=str(cli),PATH=str(bin_dir)+os.pathsep+os.environ['PATH'])
            r=subprocess.run(['bash','-c',script],env=env,capture_output=True,text=True)
            return r.returncode,r.stdout+r.stderr,(dumps/'pending.json').exists()
    def test_v4_pre_activation(self):
        rc,out,exists=self.run_case(110999,4,111050);self.assertEqual(rc,0);self.assertIn('PREFLIGHT_READY',out)
    def test_v4_at_activation_rejected(self):
        rc,out,exists=self.run_case(111000,4,111288);self.assertNotEqual(rc,0);self.assertIn('requires v5',out);self.assertFalse(exists)
    def test_v5_waits_and_preserves_candidate(self):
        rc,out,exists=self.run_case(111000,5,111287);self.assertEqual(rc,0);self.assertIn('waiting for tip=111288',out);self.assertNotIn('PREFLIGHT_READY',out);self.assertTrue(exists)
    def test_v5_exact_burial(self):
        rc,out,exists=self.run_case(111000,5,111288);self.assertEqual(rc,0);self.assertIn('PREFLIGHT_READY',out)
    def test_reorg_discards_candidate(self):
        rc,out,exists=self.run_case(111000,5,111288,False);self.assertNotEqual(rc,0);self.assertIn('left selected chain',out);self.assertFalse(exists)
    def test_unknown_format_rejected(self):
        rc,out,exists=self.run_case(111000,6,111288);self.assertNotEqual(rc,0);self.assertNotIn('PREFLIGHT_READY',out)

if __name__=='__main__': unittest.main()
