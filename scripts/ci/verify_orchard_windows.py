#!/usr/bin/env python3
"""Fail-closed evidence gate for the two non-IPC MSVC component tests."""
import argparse
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET

SELECTED = ('OrchardBackendCpp', 'OrchardWalletSpend')
INVENTORY = {'OrchardBackendCpp', 'OrchardWalletKeys', 'OrchardWalletShield',
             'OrchardWalletSpend', 'OrchardWalletStorage', 'OrchardTransactionReader',
             'OrchardCoinSnapshot', 'OrchardTransparent', 'OrchardAuthorization',
             'OrchardStateTransition', 'OrchardTransactionContext', 'OrchardBackendRust'}
MARKERS = {
    'OrchardBackendCpp': 'Orchard C++ ownership, independent digest, 13 context mutations, payload binding and monetary bounds passed',
    'OrchardWalletSpend': 'Fresh shield -> recipient decrypt -> witnessed cross-address send -> unshield proof lifecycle passed; all amounts conserved',
}

def require(ok, message):
    if not ok:
        raise ValueError(message)

def inventory(data, source, build, *, selected=False):
    require(data.get('kind') == 'ctestInfo' and data.get('version', {}).get('major') == 1, 'Unsupported CTest inventory')
    tests = data.get('tests', [])
    names = [t.get('name') for t in tests]
    expected = set(SELECTED) if selected else INVENTORY
    require(len(names) == len(expected) and set(names) == expected, 'Unexpected or duplicate test inventory')
    for test in tests:
        if test['name'] not in SELECTED:
            continue
        props = test.get('properties', [])
        require(len({p['name'] for p in props}) == len(props), 'Duplicate test property')
        props = {p['name']: p['value'] for p in props}
        require(set(props) <= {'TIMEOUT', 'RUN_SERIAL', 'ENVIRONMENT', 'LABELS', 'WORKING_DIRECTORY'}, 'Unapproved execution property or fixture hook')
        require(type(props.get('TIMEOUT')) in (int, float) and 0 < props['TIMEOUT'] <= 600, 'Missing or excessive test timeout')
        require(props.get('RUN_SERIAL') is True, 'Test must remain serial')
        require(props.get('ENVIRONMENT') == ['RAYON_NUM_THREADS=2'], 'Changed test environment')
        require(Path(props.get('WORKING_DIRECTORY', '')).resolve() == build.resolve(), 'Wrong test working directory')
        exe = 'test_orchard_backend.exe' if test['name'] == SELECTED[0] else 'test_orchard_wallet_spend.exe'
        expected_args = [str(build / exe)]
        if test['name'] == SELECTED[0]:
            expected_args.append(str(source / 'rust/orchard_backend/tests/fixtures'))
        args = test.get('command', [])
        require(len(args) == len(expected_args) and all(Path(a).resolve() == Path(b).resolve() for a, b in zip(args, expected_args)), 'Changed test command')
    return names

def reports(xml, log):
    root = ET.fromstring(xml)
    require(root.tag == 'testsuite' and root.get('tests') == '2' and root.get('failures') == '0' and root.get('disabled') == '0', 'Incomplete JUnit suite')
    require(root.get('errors', '0') == '0' and root.get('skipped') == '0', 'JUnit infrastructure error')
    cases = root.findall('testcase')
    require(len(cases) == 2 and {c.get('name') for c in cases} == set(SELECTED), 'Missing or duplicate executed test')
    for case in cases:
        require(case.get('status') == 'run' and not list(case.iter('failure')) and not list(case.iter('skipped')) and not list(case.iter('error')), 'Failed or skipped execution')
        require(MARKERS[case.get('name')] in (case.findtext('system-out') or ''), 'Missing component completion marker')
    require('PASS test platform argument quoting and owned temporary directories' in log, 'Missing platform helper execution')
    require('100% tests passed, 0 tests failed out of 2' in log, 'Missing exact CTest completion')
    for name in SELECTED:
        require(len(re.findall(r'^\d/2 Test #\s*\d+: '+name+r' .*Passed', log, re.M)) == 1, 'Missing unique CTest execution: '+name)
    return list(SELECTED)

def main():
    p=argparse.ArgumentParser();p.add_argument('phase',choices=['inventory','results']);p.add_argument('--source',type=Path,required=True);p.add_argument('--build',type=Path,required=True);p.add_argument('--evidence',type=Path,required=True);a=p.parse_args()
    if a.phase == 'inventory':
        inventory(json.loads((a.evidence/'inventory.json').read_text(encoding='utf-8-sig')),a.source,a.build)
        inventory(json.loads((a.evidence/'selected.json').read_text(encoding='utf-8-sig')),a.source,a.build,selected=True)
    else:
        reports((a.evidence/'results.xml').read_text(encoding='utf-8-sig'),(a.evidence/'ctest.log').read_text(encoding='utf-8-sig'))
    print(a.phase.upper()+'_PASS')
if __name__=='__main__':main()
