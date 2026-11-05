#!/usr/bin/env python3
import pathlib
import subprocess
import tempfile
import unittest
from check_desktop_candidate import validate


class CandidateSource(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.source = pathlib.Path(self.temp.name)
        self.cmake = self.source / 'CMakeLists.txt'
        self.cmake.write_text('project(dinero VERSION 8.2.0 LANGUAGES C CXX)\n')
        self.git('init', '-q')
        self.git('add', 'CMakeLists.txt')
        self.git('-c', 'user.name=Test', '-c', 'user.email=test@example.invalid', 'commit', '-qm', 'candidate')
        self.head = self.git('rev-parse', 'HEAD').strip()

    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.source), *args], text=True)

    def test_exact_checkout_uses_source_version(self):
        self.assertEqual(validate(self.head, self.source), '8.2.0')

    def test_branches_tags_abbreviations_and_expressions_refused(self):
        for ref in ['dinero-main', 'v8.2.0', self.head[:9], self.head + '^', self.head.upper(), '']:
            with self.subTest(ref=ref), self.assertRaises(ValueError):
                validate(ref)

    def test_wrong_full_commit_refused(self):
        with self.assertRaisesRegex(ValueError, 'does not match'):
            validate('0' * 40, self.source)

    def test_missing_or_ambiguous_version_refused(self):
        for content in ['', 'project(dinero VERSION 8.2 LANGUAGES CXX)', self.cmake.read_text() * 2]:
            self.cmake.write_text(content)
            with self.subTest(content=content), self.assertRaises(ValueError):
                validate(self.head, self.source)


if __name__ == '__main__':
    unittest.main()
