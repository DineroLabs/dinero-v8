#!/usr/bin/env python3
"""Fail closed on missing tests, instrumentation or daemon sanitizer reports."""
import argparse
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET

from check_shielded_sanitizer_coverage import COMPACT_DAEMON_SOURCES

EXPECTED_TESTS = (
    'P2PHeaderParserAlignment', 'DaemonServiceRelease',
    'PackedHeaderAlignment', 'SerializationEmptyBuffers',
    'CompactRegtestFixedVectors', 'CompactRegtestVectorOracle', 'ShieldedResourceLimits',
    'CompactProductionV6Vectors', 'CompactProductionV6Oracle',
    'ShieldedReindexEquivalence', 'ShieldedAuthRelayLifecycle', 'CompactRegtestLifecycle',
    'CsnManualInvalidation', 'CSNShieldedReorgInvertibility',
)
DIAGNOSTIC = re.compile(r'(?:ERROR|SUMMARY): (?:AddressSanitizer|LeakSanitizer|UndefinedBehaviorSanitizer)'
                        r'|AddressSanitizer:DEADLYSIGNAL|runtime error:')


def validate(directory):
    failures, names, reports = [], [], []
    try:
        if (directory / 'ctest.exit').read_text().strip() != '0':
            failures.append('CTest returned a nonzero exit status')
        log = (directory / 'ctest.log').read_text(errors='replace')
        if DIAGNOSTIC.search(log):
            failures.append('Sanitizer diagnostic in CTest output')
        suite = ET.parse(directory / 'ctest.xml').getroot()
        if any(int(suite.attrib.get(key, '0')) != 0
               for key in ('failures', 'errors', 'skipped', 'disabled')):
            failures.append('JUnit summary reports failed, skipped or disabled tests')
        for case in suite.iter('testcase'):
            names.append(case.attrib.get('name'))
            if case.attrib.get('status') != 'run':
                failures.append(f"Test did not run: {case.attrib.get('name')}")
        if len(names) != len(EXPECTED_TESTS) or set(names) != set(EXPECTED_TESTS):
            failures.append('Missing, duplicate or unexpected selected test')
        if any(list(suite.iter(tag)) for tag in ('failure', 'error', 'skipped')):
            failures.append('JUnit contains failed or skipped tests')
        coverage = json.loads((directory / 'coverage.json').read_text())
        required = {'address', 'undefined'}
        if coverage.get('failures') or set(coverage.get('required', [])) != required:
            failures.append('Instrumentation audit failed or has wrong sanitizer scope')
        checked = coverage.get('checked', [])
        if not set(COMPACT_DAEMON_SOURCES) <= {c['source'] for c in checked}:
            failures.append('Instrumentation audit omitted required sources')
        if any(not required <= set(c['enabled']) for c in checked):
            failures.append('A checked source lacks required instrumentation')
    except (OSError, ValueError, KeyError, TypeError, ET.ParseError) as error:
        failures.append(f'Missing or invalid evidence: {error}')

    runtime = directory / 'runtime'
    if not runtime.is_dir():
        failures.append('Missing sanitizer runtime-log directory')
    else:
        # Runtime log_path survives harness cleanup and swallowed child exit codes.
        # Any nonempty runtime report is fatal, including unrecognized diagnostics.
        for path in sorted(runtime.rglob('*')):
            if path.is_file() and path.stat().st_size:
                reports.append(str(path.relative_to(directory)))
        if reports:
            failures.append('Sanitizer runtime produced reports')
    return {'expected_tests': list(EXPECTED_TESTS), 'observed_tests': names,
            'runtime_reports': reports, 'failures': failures}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('evidence_dir', type=Path)
    args = parser.parse_args()
    result = validate(args.evidence_dir)
    print(json.dumps(result, indent=2))
    return 1 if result['failures'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
