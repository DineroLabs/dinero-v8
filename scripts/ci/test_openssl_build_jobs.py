"""Offline job-budget checks: never download, configure or compile OpenSSL."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

SCRIPT=Path(__file__).resolve().parents[1]/'build-openssl-vendored.sh'

class OpenSSLBuildJobs(unittest.TestCase):
    def test_invalid_budget_refuses_before_external_commands(self):
        for jobs in ['0','-1','2.5','01','two','2 3','2\n3','$(id)']:
            with self.subTest(jobs=jobs), tempfile.TemporaryDirectory() as td:
                root=Path(td);before=set(root.iterdir())
                # Empty PATH proves the diagnostic comes before even dirname,
                # source lookup, download, cleanup, Configure, or make.
                env={**os.environ,'PATH':td,'OPENSSL_BUILD_JOBS':jobs}
                result=subprocess.run(['/bin/bash',str(SCRIPT)],cwd=root,env=env,capture_output=True,text=True,timeout=5)
                self.assertEqual(result.returncode,2)
                self.assertEqual(result.stderr.strip(),'OPENSSL_BUILD_JOBS must be a positive integer')
                self.assertEqual(set(root.iterdir()),before)
                self.assertNotIn('command not found',result.stderr)

    def test_default_and_explicit_budgets_reach_source_discovery(self):
        for jobs,want in [(None,'2'),('1','1'),('2','2'),('4','4')]:
            with self.subTest(jobs=jobs), tempfile.TemporaryDirectory() as td:
                # An intentional sentinel stops at the first external source-
                # discovery command. No source/build tools are available.
                root=Path(td);probe=root/'dirname'
                probe.write_text('#!/bin/bash\nprintf "budget=%s\\n" "$OPENSSL_BUILD_JOBS" >&2\nexit 91\n');probe.chmod(0o755)
                env={**os.environ,'PATH':td};env.pop('OPENSSL_BUILD_JOBS',None)
                if jobs is not None:env['OPENSSL_BUILD_JOBS']=jobs
                result=subprocess.run(['/bin/bash',str(SCRIPT)],cwd=root,env=env,capture_output=True,text=True,timeout=5)
                # Shell-local defaults are not automatically exported, so use
                # only explicit budgets for the child environment assertion.
                if jobs is not None:self.assertIn('budget='+want,result.stderr)
                self.assertNotIn('must be a positive integer',result.stderr)
                self.assertNotEqual(result.returncode,0)
                self.assertEqual(set(root.iterdir()),{probe})

    def test_make_uses_the_checked_budget(self):
        text=SCRIPT.read_text()
        self.assertEqual(text.count('CORES="$OPENSSL_BUILD_JOBS"'),1)
        self.assertIn('make -j"$CORES"',text)
        self.assertNotRegex(text,r'CORES=\$\((?:nproc|sysctl)')
        self.assertLess(text.index('OPENSSL_BUILD_JOBS="${OPENSSL_BUILD_JOBS:-2}"'),text.index('SCRIPT_DIR='))

if __name__=='__main__':unittest.main(verbosity=2)
