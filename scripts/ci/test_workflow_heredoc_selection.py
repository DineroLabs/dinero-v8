"""Parser-only regressions: subprocesses forbidden; shell bodies are data."""
from pathlib import Path
import importlib.util,sys,tempfile,unittest
from unittest.mock import patch
sys.dont_write_bytecode=True
parser_path = sys.argv.pop(1) if len(sys.argv) > 1 and sys.argv[1].endswith('.py') else str(Path(__file__).with_name('check_mandatory_tests_execute.py'))
spec=importlib.util.spec_from_file_location('gate',parser_path);gate=importlib.util.module_from_spec(spec);spec.loader.exec_module(gate)
class HeredocSelection(unittest.TestCase):
 def setUp(self):
  self.blocker=patch('subprocess.Popen',side_effect=AssertionError('Parser regression may not launch subprocesses'));self.blocker.start();self.addCleanup(self.blocker.stop)
 def invocations(self,body):
  with tempfile.TemporaryDirectory() as td:
   p=Path(td)/'workflow.yml';p.write_text('jobs:\n  j:\n    steps:\n      - run: |\n'+''.join('          '+s+'\n' for s in body.splitlines()));return gate.ctest_blocks(str(p))
 def test_after_quoted_python_body(self):
  rows=self.invocations("python3 - <<'PY'\nvalue = line.split(' #', 1)\nctest --test-dir fake -R 'NotAShellCommand'\nPY\nctest --test-dir real -R '^Safe$'")
  self.assertEqual(len(rows),1);self.assertEqual(rows[0][1],['ctest','--test-dir','real','-R','^Safe$'])
 def test_hash_inside_shell_quotes(self):
  rows=self.invocations("ctest --test-dir real -R '^Case #1$'");self.assertEqual(len(rows),1);self.assertEqual(rows[0][1][-1],'^Case #1$')
 def test_multiple_quoted_bodies(self):
  rows=self.invocations("cat <<'A' <<\"B\"\nctest --test-dir fake\nA\n'\"#\nB\nctest --test-dir real -R '^Safe$'");self.assertEqual(len(rows),1);self.assertIn('real',rows[0][1])
 def test_tab_stripped_body(self):
  rows=self.invocations("cat <<-'END'\n\tctest --test-dir fake\n\tEND\nctest --test-dir real -R '^Safe$'");self.assertEqual(len(rows),1);self.assertIn('real',rows[0][1])
 def test_missing_terminator_refuses(self):
  with self.assertRaises(SystemExit):self.invocations("cat <<'END'\nctest --test-dir fake")
 def test_dynamic_command_selector_refuses(self):
  rows=self.invocations('ctest --test-dir real -R "$(cat selected.txt)"');self.assertEqual(len(rows),1)
  with self.assertRaises(SystemExit):gate.parse_block(*rows[0],'synthetic')
 def test_backtick_selector_refuses(self):
  rows=self.invocations('ctest --test-dir real -R "`cat selected.txt`"');self.assertEqual(len(rows),1)
  with self.assertRaises(SystemExit):gate.parse_block(*rows[0],'synthetic')
 def test_unquoted_body_expansion_refuses(self):
  with self.assertRaises(SystemExit):self.invocations('cat <<END\n$(ctest --test-dir fake)\nEND\nctest --test-dir real')
 def test_actual_generated_selector_is_not_silently_omitted(self):
  workflow=Path(__file__).resolve().parents[2]/'.github/workflows/orchard-backend.yml'
  bodies=[body for _,body in gate.run_blocks(str(workflow)) if 'orchard-real-mempool-regex.txt' in body]
  self.assertEqual(len(bodies),1);body=bodies[0]
  rows=[r for r in self.invocations(body) if '$(cat orchard-real-mempool-regex.txt)' in r[1]]
  self.assertEqual(len(rows),1)
  with self.assertRaises(SystemExit):gate.parse_block(*rows[0],'actual-copied-workflow')
if __name__=='__main__':unittest.main(verbosity=2)
