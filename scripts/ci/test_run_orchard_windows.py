#!/usr/bin/env python3
"""Local files only; all process creation denied, no node or compiler execution."""
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import run_orchard_windows as driver

class SourcePreflight(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)/'source';self.root.mkdir()
        p=patch.object(driver.subprocess,'Popen',side_effect=AssertionError('Child process forbidden'))
        p.start();self.addCleanup(p.stop)
    def fingerprint(self,names):
        with patch.object(driver.subprocess,'check_output',return_value=('\0'.join(names)+'\0').encode()):
            return driver.fingerprint(self.root)
    def test_dangling_symlink_is_recorded(self):
        (self.root/'link').symlink_to('nonexistent-target')
        try:r=self.fingerprint(['link'])
        except OSError as e:self.fail('Fingerprint followed dangling symlink: '+str(e))
        self.assertIn('link',r)
    def test_existing_symlink_target_is_never_read(self):
        target=Path(self.temp.name)/'target';target.write_bytes(b'not a source input')
        link=self.root/'link';link.symlink_to(target)
        original=Path.read_bytes
        def guarded(p):
            if p==link or p==target:raise AssertionError('Fingerprint read symlink target')
            return original(p)
        with patch.object(Path,'read_bytes',guarded):self.assertIn('link',self.fingerprint(['link']))
    def test_link_target_change_is_detected(self):
        (self.root/'a').write_bytes(b'same');(self.root/'b').write_bytes(b'same')
        p=self.root/'link';p.symlink_to('a');before=self.fingerprint(['link'])
        p.unlink();p.symlink_to('b');self.assertNotEqual(before,self.fingerprint(['link']))
    def test_regular_file_change_is_detected(self):
        p=self.root/'file';p.write_bytes(b'first');before=self.fingerprint(['file'])
        p.write_bytes(b'second');self.assertNotEqual(before,self.fingerprint(['file']))
    def test_missing_regular_file_still_refuses(self):
        with self.assertRaises(FileNotFoundError):self.fingerprint(['missing'])
    def test_preflight_failure_retains_terminal_evidence(self):
        out=Path(self.temp.name)/'output'
        argv=['run_orchard_windows.py','--source',str(self.root),'--output',str(out),'--expected-sha','a'*40]
        with patch.object(sys,'argv',argv),patch.object(sys,'platform','win32'),patch.object(driver.subprocess,'check_output',side_effect=['a'*40+'\n',b'']),patch.object(driver,'fingerprint',side_effect=FileNotFoundError('synthetic missing regular source')),contextlib.redirect_stdout(io.StringIO()):
            try:code=driver.main()
            except OSError as e:self.fail('Preflight failed without retained terminal evidence: '+str(e))
        self.assertEqual(code,1)
        r=json.loads((out/'evidence/terminal.json').read_text());self.assertEqual(r['status'],'failure')
        self.assertIn('synthetic missing regular source',r['error'])
        self.assertTrue((out/'evidence/exception.txt').is_file())
    def test_non_windows_refuses_without_output(self):
        out=Path(self.temp.name)/'output'
        argv=['run_orchard_windows.py','--source',str(self.root),'--output',str(out),'--expected-sha','a'*40]
        with patch.object(sys,'argv',argv),patch.object(sys,'platform','darwin'):
            with self.assertRaises(ValueError):driver.main()
        self.assertFalse(out.exists())

if __name__=='__main__':unittest.main(verbosity=2)
