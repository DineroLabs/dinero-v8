#!/usr/bin/env python3
"""Synthetic harness checks: no compiler, CTest, or production mutation runs."""
import contextlib
import importlib.util
import io
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch


class CovenantHarnessResults(unittest.TestCase):
    def setUp(self):
        denied = patch('subprocess.Popen', side_effect=AssertionError('No child process allowed'))
        denied.start()
        self.addCleanup(denied.stop)
        path = Path(__file__).resolve().parents[2] / 'tools/covenant_mutation_harness.py'
        spec = importlib.util.spec_from_file_location('covenant_harness_under_test', path)
        self.harness = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.harness)
        self.temp = tempfile.TemporaryDirectory(prefix='covenant-harness-policy-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.original = b'keep_original_rule\n'
        self.target = self.root / 'synthetic_rule.txt'
        self.target.write_bytes(self.original)
        self.harness.REPO = self.root
        self.harness.MUTATIONS = [{'id': 'synthetic_rule', 'file': self.target.name,
            'rule': 'synthetic only', 'old': 'keep_original_rule', 'new': 'omit_rule'}]

    def invoke(self, builds=(0, 0), tests=(True, False)):
        build_results = [SimpleNamespace(returncode=n, stderr='synthetic build result') for n in builds]
        stream = io.StringIO()
        with patch.object(self.harness, 'build', side_effect=build_results) as build, \
             patch.object(self.harness, 'run_tests', side_effect=[(x, 'synthetic test result') for x in tests]) as run_tests, \
             patch.object(sys, 'argv', ['harness', '--build-dir', str(self.root / 'unbuilt')]), \
             contextlib.redirect_stdout(stream), contextlib.redirect_stderr(stream):
            result = self.harness.main()
        self.assertEqual(self.target.read_bytes(), self.original)
        return result, stream.getvalue(), build.call_count, run_tests.call_count

    def test_mutation_compile_failure_is_not_green(self):
        result, output, builds, tests = self.invoke(builds=(0, 1), tests=(True,))
        self.assertEqual(result, 1)
        self.assertIn('BUILD_FAILED', output)
        self.assertIn('unbuildable  1', output)
        self.assertEqual((builds, tests), (2, 1))

    def test_caught_mutation_remains_green(self):
        result, output, builds, tests = self.invoke()
        self.assertEqual(result, 0)
        self.assertIn('score        1/1', output)
        self.assertEqual((builds, tests), (2, 2))

    def test_surviving_mutation_remains_red(self):
        result, output, _, _ = self.invoke(tests=(True, True))
        self.assertEqual(result, 1)
        self.assertIn('SURVIVED', output)

    def test_stale_anchor_remains_red(self):
        self.harness.MUTATIONS[0]['old'] = 'not_in_the_file'
        result, output, builds, tests = self.invoke(builds=(0,), tests=(True,))
        self.assertEqual(result, 1)
        self.assertIn('STALE ANCHOR:', output)
        self.assertEqual((builds, tests), (1, 1))

    def test_baseline_build_failure_does_not_run_tests(self):
        result, _, builds, tests = self.invoke(builds=(1,), tests=())
        self.assertEqual(result, 2)
        self.assertEqual((builds, tests), (1, 0))

    def test_baseline_test_failure_does_not_mutate(self):
        result, _, builds, tests = self.invoke(builds=(0,), tests=(False,))
        self.assertEqual(result, 2)
        self.assertEqual((builds, tests), (1, 1))

    def test_exception_restores_synthetic_file(self):
        first = SimpleNamespace(returncode=0, stderr='')
        with patch.object(self.harness, 'build', side_effect=[first, RuntimeError('synthetic stop')]), \
             patch.object(self.harness, 'run_tests', return_value=(True, '')), \
             patch.object(sys, 'argv', ['harness']), contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, 'synthetic stop'):
                self.harness.main()
        self.assertEqual(self.target.read_bytes(), self.original)

    def test_build_command_respects_four_job_cap(self):
        with patch.object(self.harness, 'run', return_value=SimpleNamespace(returncode=0)) as run:
            self.harness.build('synthetic-build')
        command = run.call_args.args[0]
        self.assertEqual(command[:5], ['cmake', '--build', 'synthetic-build', '--target', self.harness.TARGETS[0]])
        self.assertEqual(command[4:-1], self.harness.TARGETS)
        self.assertEqual(command[-1], '-j4')

    def test_empty_ctest_selection_is_an_error(self):
        with patch.object(self.harness, 'run', return_value=SimpleNamespace(returncode=0, stdout='synthetic')) as run:
            self.assertEqual(self.harness.run_tests('synthetic-build'), (True, 'synthetic'))
        command = run.call_args.args[0]
        self.assertIn('--no-tests=error', command)
        self.assertEqual(command[command.index('-R') + 1], '^(%s)$' % '|'.join(self.harness.CTEST_TESTS))

    def test_nonzero_ctest_is_red(self):
        with patch.object(self.harness, 'run', return_value=SimpleNamespace(returncode=1, stdout='synthetic failure')):
            self.assertEqual(self.harness.run_tests('synthetic-build'), (False, 'synthetic failure'))


if __name__ == '__main__':
    unittest.main()
