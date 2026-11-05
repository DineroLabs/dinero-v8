#!/usr/bin/env python3
"""Run the real entrypoint; fresh defaults must not override an existing owner."""
import os
from pathlib import Path
import stat
import subprocess
import tempfile
import unittest

ENTRYPOINT=Path(__file__).resolve().parents[2]/'docker-entrypoint.sh'

class DockerEntrypointConfig(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='dinero-entrypoint-config-')
        self.root=Path(self.temp.name);self.data=self.root/'data with spaces';self.data.mkdir()
        self.capture=self.root/'argv';stub=self.root/'bin';stub.mkdir()
        daemon=stub/'dinerod';daemon.write_text('#!/bin/sh\nprintf \'%s\\n\' "$@" > "$ARGV_CAPTURE"\n');daemon.chmod(0o700)
        self.env={**os.environ,'PATH':str(stub)+os.pathsep+os.environ['PATH'],
            'DINERO_DATADIR':str(self.data),'DINERO_SNAPSHOT':str(self.root/'missing.dat'),
            'DINERO_SNAPSHOT_FALLBACKS':str(self.root/'missing-fallback.dat'),'ARGV_CAPTURE':str(self.capture)}
    def tearDown(self):
        self.temp.cleanup()
    def run_entrypoint(self,*args):
        result=subprocess.run(['/bin/sh',str(ENTRYPOINT),*args],env=self.env,capture_output=True,text=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stderr)
        return result.stdout,self.capture.read_text().splitlines()
    def test_empty_volume_gets_explicit_private_oracle_default(self):
        self.run_entrypoint()
        config=self.data/'dinero.conf'
        self.assertTrue(config.exists(),'fresh scheduled-Orchard startup otherwise defaults to incompatible legacy oracles')
        self.assertIn('lightning.oracles.enable=0',config.read_text())
        self.assertEqual(stat.S_IMODE(config.stat().st_mode),0o600)
    def test_existing_configuration_is_never_rewritten(self):
        config=self.data/'dinero.conf';original='lightning.oracles.enable=1\ncustom.setting=keep\n';config.write_text(original)
        _,argv=self.run_entrypoint()
        self.assertEqual(config.read_text(),original)
        self.assertFalse(any('lightning.oracles.enable=0' in arg for arg in argv))
    def test_existing_data_without_config_requires_operator_decision(self):
        sentinel=self.data/'wallet-owner';sentinel.write_bytes(b'preserve')
        self.run_entrypoint()
        self.assertFalse((self.data/'dinero.conf').exists())
        self.assertEqual(sentinel.read_bytes(),b'preserve')
    def test_explicit_config_and_oracle_arguments_retain_authority(self):
        for args in [('--conf=/operator/custom.conf',),('-conf','/operator/custom.conf'),
                     ('--config=/operator/custom.conf',),('--lightning.oracles.enable=1',),('--lightning.enable=1',)]:
            with self.subTest(args=args):
                _,argv=self.run_entrypoint(*args)
                self.assertFalse((self.data/'dinero.conf').exists())
                for argument in args:self.assertIn(argument,argv)
    @unittest.skipIf(os.geteuid()==0,'root cannot model an unreadable owner directory')
    def test_unreadable_existing_volume_cannot_be_treated_as_empty(self):
        (self.data/'existing-owner').write_text('preserve')
        self.data.chmod(0o300)
        try:
            result=subprocess.run(['/bin/sh',str(ENTRYPOINT)],env=self.env,capture_output=True,text=True,timeout=10)
            self.assertNotEqual(result.returncode,0)
        finally:
            self.data.chmod(0o700)
        self.assertFalse((self.data/'dinero.conf').exists())
    def test_restart_preserves_initialized_defaults_and_never_logs_arguments(self):
        self.run_entrypoint();config=self.data/'dinero.conf';self.assertTrue(config.exists())
        original=config.read_bytes();(self.data/'blocks').mkdir()
        output,argv=self.run_entrypoint('-rpcpassword=TEST_ONLY_CANARY')
        self.assertEqual(config.read_bytes(),original)
        self.assertNotIn('TEST_ONLY_CANARY',output)
        self.assertIn('-rpcpassword=TEST_ONLY_CANARY',argv)

if __name__=='__main__':unittest.main()
