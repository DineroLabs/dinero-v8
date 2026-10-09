#!/usr/bin/env python3
"""Synthetic parser cases only. No compiler, CTest, node or child process."""
import copy,json,tempfile,unittest
from pathlib import Path
from unittest.mock import patch
import verify_orchard_windows as gate

class EvidenceGate(unittest.TestCase):
    def setUp(self):
        self.nochild=patch('subprocess.Popen', side_effect=AssertionError('No child process authorized in parser tests'));self.nochild.start();self.addCleanup(self.nochild.stop)
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup);self.root=Path(self.tmp.name);self.build=self.root/'build'
        self.data={'kind':'ctestInfo','version':{'major':1},'tests':[]}
        for name in sorted(gate.INVENTORY):
            exe='test_orchard_backend.exe' if name==gate.SELECTED[0] else 'test_orchard_wallet_spend.exe'
            args=[str(self.build/exe)]
            if name==gate.SELECTED[0]:args.append(str(self.root/'rust/orchard_backend/tests/fixtures'))
            props={'TIMEOUT':600.0,'RUN_SERIAL':True,'ENVIRONMENT':['RAYON_NUM_THREADS=2'],'WORKING_DIRECTORY':str(self.build),'LABELS':['orchard']}
            self.data['tests'].append({'name':name,'command':args,'properties':[{'name':k,'value':v} for k,v in props.items()]})
        self.xml='<testsuite tests="2" failures="0" disabled="0" skipped="0">'+''.join('<testcase name="'+n+'" status="run"><system-out>'+gate.MARKERS[n].replace('>','&gt;')+'</system-out></testcase>' for n in gate.SELECTED)+'</testsuite>'
        self.log='1/2 Test #1: OrchardBackendCpp .... Passed\n2/2 Test #4: OrchardWalletSpend .... Passed\n100% tests passed, 0 tests failed out of 2\nPASS test platform argument quoting and owned temporary directories\n'
    def check(self,data=None,selected=False):return gate.inventory(data or self.data,self.root,self.build,selected=selected)
    def test_exact_inventories(self):
        self.check();d=copy.deepcopy(self.data);d['tests']=[t for t in d['tests'] if t['name'] in gate.SELECTED];self.check(d,True)
    def test_missing_extra_duplicate_inventory(self):
        for op in ('missing','extra','duplicate'):
            with self.subTest(op=op):
                d=copy.deepcopy(self.data)
                if op=='missing':d['tests'].pop()
                elif op=='extra':d['tests'].append({'name':'UnapprovedTest'})
                else:d['tests'][-1]=d['tests'][0]
                with self.assertRaises(ValueError):self.check(d)
    def test_hooks_disable_and_environment_refuse(self):
        for prop,value in [('DISABLED',True),('FIXTURES_REQUIRED',['other']),('FIXTURES_SETUP',['other']),('ENVIRONMENT_MODIFICATION',['PATH=set:bad']),('TIMEOUT',601),('TIMEOUT',0),('RUN_SERIAL',False),('ENVIRONMENT',['RAYON_NUM_THREADS=4'])]:
            with self.subTest(prop=prop,value=value):
                d=copy.deepcopy(self.data);t=next(t for t in d['tests'] if t['name']==gate.SELECTED[0]);t['properties']=[p for p in t['properties'] if p['name']!=prop]+[{'name':prop,'value':value}]
                with self.assertRaises(ValueError):self.check(d)
    def test_changed_command_and_working_directory_refuse(self):
        for change in ('exe','arg','directory'):
            with self.subTest(change=change):
                d=copy.deepcopy(self.data);t=next(t for t in d['tests'] if t['name']==gate.SELECTED[0])
                if change=='exe':t['command'][0]=str(self.build/'other.exe')
                elif change=='arg':t['command'].append('--other')
                else:next(p for p in t['properties'] if p['name']=='WORKING_DIRECTORY')['value']=str(self.root)
                with self.assertRaises(ValueError):self.check(d)
    def test_exact_reports(self):self.assertEqual(gate.reports(self.xml,self.log),list(gate.SELECTED))
    def test_bad_totals_status_and_skips_refuse(self):
        for bad in [self.xml.replace('tests="2"','tests="0"'),self.xml.replace('failures="0"','failures="1"'),self.xml.replace('disabled="0"','disabled="1"'),self.xml.replace('skipped="0"','skipped="1"'),self.xml.replace('status="run"','status="notrun"',1),self.xml.replace('</testcase>','<skipped/></testcase>',1),self.xml.replace('</testcase>','<failure/></testcase>',1),self.xml.replace('</testcase>','<error/></testcase>',1)]:
            with self.subTest(bad=bad):
                with self.assertRaises(ValueError):gate.reports(bad,self.log)
    def test_missing_and_duplicate_execution_refuse(self):
        for bad in [self.xml.replace(gate.SELECTED[1],gate.SELECTED[0]),self.xml.replace('Orchard C++ ownership','missing')]:
            with self.assertRaises(ValueError):gate.reports(bad,self.log)
        with self.assertRaises(ValueError):gate.reports(self.xml,self.log+self.log.splitlines()[0]+'\n')
    def test_missing_log_markers_refuse(self):
        for text in ['100% tests passed, 0 tests failed out of 2','PASS test platform argument quoting and owned temporary directories','2/2 Test #4: OrchardWalletSpend .... Passed']:
            with self.assertRaises(ValueError):gate.reports(self.xml,self.log.replace(text,''))
if __name__=='__main__':unittest.main(verbosity=2)
