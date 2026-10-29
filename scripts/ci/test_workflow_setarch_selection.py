"""Parse setarch-wrapped CTest as inert text. No subprocess or CTest may run."""
import contextlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import check_mandatory_tests_execute as gate

class SetarchSelection(unittest.TestCase):
    def setUp(self):
        block=patch('subprocess.Popen',side_effect=AssertionError('Parser tests cannot launch subprocesses'))
        block.start();self.addCleanup(block.stop)

    def rows(self, body):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'workflow.yml'
            p.write_text('jobs:\n  j:\n    steps:\n      - run: |\n'+''.join('          '+line+'\n' for line in body.splitlines()))
            return gate.ctest_blocks(str(p))

    def selection(self, body):
        rows=self.rows(body);self.assertEqual(len(rows),1)
        spec=gate.parse_block(*rows[0],'synthetic')
        return gate.selected_by(spec,[('Safe',[]),('Other',[])]),spec

    def test_environment_prefix_retains_selection(self):
        names,spec=self.selection("TSAN_OPTIONS=halt_on_error=1 setarch x86_64 -R ctest --test-dir build-tests -R '^Safe$'")
        self.assertEqual(names,{'Safe'});self.assertEqual(spec['test_dir'],'build-tests')

    def test_absolute_setarch_and_redirection(self):
        names,_=self.selection("env TSAN_OPTIONS=halt_on_error=1 /usr/bin/setarch x86_64 -R ctest --test-dir build-tests -R '^Safe$' > out.log 2>&1")
        self.assertEqual(names,{'Safe'})

    def test_show_only_does_not_count_as_execution(self):
        names,spec=self.selection('setarch x86_64 -R ctest --test-dir build-tests -N')
        self.assertEqual(names,set());self.assertTrue(spec['show_only'])

    def test_unknown_wrapped_ctest_selection_refuses(self):
        rows=self.rows('setarch x86_64 -R ctest --test-dir build-tests --rerun-failed')
        self.assertEqual(len(rows),1)
        with self.assertRaises(SystemExit):gate.parse_block(*rows[0],'synthetic')

    def test_unsupported_wrapper_shape_refuses(self):
        for prefix in ['setarch x86_64 -B','setarch x86_64','setarch ${ARCH} -R']:
            with self.subTest(prefix=prefix),self.assertRaises(SystemExit):
                self.rows(prefix+' ctest --test-dir build-tests')

    def test_other_setarch_commands_remain_unselected(self):
        self.assertEqual(self.rows('setarch x86_64 -R uname -m'),[])

    def test_exclusion_gate_sees_wrapped_names(self):
        for name in gate.EXCLUDED_TESTS:
            for flags in [[],['--explain'],['--update']]:
                with self.subTest(name=name,flags=flags),tempfile.TemporaryDirectory() as d:
                    root=Path(d);flow=root/'workflow.yml';debt=root/'baseline.txt';debt.write_text('')
                    flow.write_text('jobs:\n  j:\n    steps:\n      - run: |\n          ctest --test-dir build-tests --label-exclude integration\n          setarch x86_64 -R ctest --test-dir build-tests -R "^'+name+'$"\n')
                    inventory=[('Plain',[])]+[(n,['integration']) for n in gate.EXCLUDED_TESTS]
                    output=io.StringIO()
                    with patch.object(gate,'registered_tests',return_value=inventory),patch.object(sys,'argv',['gate','build-tests',str(flow),'--baseline',str(debt),*flags]),contextlib.redirect_stdout(output):
                        result=gate.main()
                    self.assertEqual(result,1);self.assertIn(name,output.getvalue());self.assertEqual(debt.read_text(),'')

    def test_actual_two_wrapped_ci_lanes_are_visible(self):
        path=Path(__file__).resolve().parents[2]/'.github/workflows/tests.yml'
        rows=gate.ctest_blocks(str(path))
        wrapped=[row for row in rows if 'setarch x86_64 -R' in row[2]]
        self.assertEqual(len(wrapped),2)
        specs=[gate.parse_block(*row,str(path)) for row in wrapped]
        self.assertEqual([(x['test_dir'],x['include']) for x in specs],[('build-tests','^InvalidAncestorCacheTest$'),('build-quic-tsan','^(QuicSession|QuicSessionStress)$')])

if __name__=='__main__':unittest.main(verbosity=2)
