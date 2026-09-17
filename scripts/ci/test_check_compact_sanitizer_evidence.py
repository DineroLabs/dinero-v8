#!/usr/bin/env python3
"""Negative controls for the daemon sanitizer evidence gate (no daemon needed)."""
import json
from pathlib import Path
import tempfile
import unittest
import xml.etree.ElementTree as ET

import check_compact_sanitizer_evidence as gate
from check_shielded_sanitizer_coverage import enabled_sanitizers


class EvidenceGate(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        (self.root / 'runtime').mkdir()
        (self.root / 'ctest.log').write_text('All selected tests passed\n')
        (self.root / 'ctest.exit').write_text('0\n')
        (self.root / 'coverage.json').write_text(json.dumps({
            'required': ['address', 'undefined'], 'failures': [],
            'checked': [{'source': s, 'enabled': ['address', 'undefined']}
                        for s in gate.COMPACT_DAEMON_SOURCES]}))
        self.suite = ET.Element('testsuite')
        for name in gate.EXPECTED_TESTS:
            ET.SubElement(self.suite, 'testcase', name=name, status='run')
        self.save_xml()

    def save_xml(self):
        ET.ElementTree(self.suite).write(self.root / 'ctest.xml')

    def rejects(self):
        self.assertTrue(gate.validate(self.root)['failures'])

    def test_complete_evidence_passes(self):
        self.assertEqual(gate.validate(self.root)['failures'], [])

    def test_missing_evidence_cannot_pass(self):
        for name in ('ctest.xml', 'ctest.exit', 'ctest.log', 'coverage.json'):
            with self.subTest(name=name):
                path = self.root / name; original = path.read_bytes(); path.unlink()
                self.rejects(); path.write_bytes(original)
        (self.root / 'runtime').rmdir()
        self.rejects()

    def test_missing_duplicate_or_unexpected_test_fails(self):
        removed = self.suite[0]; self.suite.remove(removed); self.save_xml(); self.rejects()
        self.suite.append(removed); self.suite.append(removed); self.save_xml(); self.rejects()
        self.suite.remove(removed)
        ET.SubElement(self.suite, 'testcase', name='UnrelatedGreenTest', status='run')
        self.save_xml(); self.rejects()

    def test_failed_skipped_notrun_and_malformed_xml_fail(self):
        for tag in ('failure', 'error', 'skipped'):
            node = ET.SubElement(self.suite[0], tag)
            self.save_xml(); self.rejects(); self.suite[0].remove(node)
        self.suite[0].set('status', 'notrun'); self.save_xml(); self.rejects()
        (self.root / 'ctest.xml').write_text('<testsuite>')
        self.rejects()

    def test_process_failure_cannot_hide_behind_green_xml(self):
        (self.root / 'ctest.exit').write_text('1\n'); self.rejects()

    def test_junit_summary_failure_is_not_ignored(self):
        for key in ('failures', 'errors', 'skipped', 'disabled'):
            self.suite.set(key, '1'); self.save_xml(); self.rejects()
            self.suite.attrib.pop(key)

    def test_shutdown_report_cannot_hide_behind_green_ctest(self):
        # The shell harness can observe process exit without forwarding its code.
        (self.root / 'runtime' / 'asan.123').write_text('ERROR: LeakSanitizer: leaks\n')
        self.rejects()

    def test_diagnostic_in_stdout_fails_without_runtime_file(self):
        for line in ('ERROR: AddressSanitizer: heap-use-after-free',
                     'runtime error: signed integer overflow',
                     'SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior'):
            (self.root / 'ctest.log').write_text(line); self.rejects()

    def test_missing_or_disabled_instrumentation_fails(self):
        path = self.root / 'coverage.json'; good = json.loads(path.read_text())
        for mode in ('missing', 'disabled', 'reported_failure'):
            bad = json.loads(json.dumps(good))
            if mode == 'missing': bad['checked'].pop()
            if mode == 'disabled': bad['checked'][0]['enabled'] = ['address']
            if mode == 'reported_failure': bad['failures'] = ['uninstrumented source']
            path.write_text(json.dumps(bad)); self.rejects()

    def test_sanitizer_flag_order_is_respected(self):
        self.assertEqual(enabled_sanitizers(['-fsanitize=address,undefined',
                                           '-fno-sanitize=all']), set())
        self.assertEqual(enabled_sanitizers(['-fsanitize=address,undefined',
                                           '-fno-sanitize=undefined']), {'address'})


if __name__ == '__main__':
    unittest.main(verbosity=2)
