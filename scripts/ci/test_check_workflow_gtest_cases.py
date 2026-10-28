import unittest
from check_workflow_gtest_cases import audit


class CompiledCaseGateTests(unittest.TestCase):
    def audit_gate(self, expected, cases, pattern='Example.*', predicate=None):
        source = "      - name: Case gate\n        run: |\n"
        source += "          ctest --test-dir build-orchard-root -R '^Example$' --verbose\n"
        source += "          python3 - <<'PYCASE'\n"
        source += "          import re\n"
        source += "          actual=re.findall(r'\\[       OK \\] Example\\.(\\w+)',log)\n"
        source += "          expected=" + expected + "\n"
        source += "          " + (predicate or 'assert sorted(actual)==sorted(expected)') + "\n"
        source += "          PYCASE\n"
        return audit(source, [{'name': 'Example', 'command': ['/test', '--gtest_filter='+pattern]}],
                     {'/test': ['Example.'+name for name in cases]})

    def test_matching_inventory(self):
        self.assertEqual(self.audit_gate("['A','B']", ['B','A']), ([], 1))

    def test_stale_gate_fails_for_new_compiled_case(self):
        failures, count = self.audit_gate("['A']", ['A','B'])
        self.assertEqual(count, 1)
        self.assertEqual(len(failures), 1)

    def test_missing_compiled_case_fails(self):
        self.assertTrue(self.audit_gate("['A','B']", ['A'])[0])

    def test_selected_filter_excludes_other_cases(self):
        self.assertEqual(self.audit_gate("['A']", ['A','B'], 'Example.A'), ([], 1))

    def test_list_comprehension(self):
        self.assertEqual(self.audit_gate("[f'{x}' for x in ['A','B']]", ['A','B']), ([], 1))

    def test_explicit_refusal_predicate(self):
        self.assertTrue(self.audit_gate("['A']", ['A','B'], predicate="if sorted(actual)!=sorted(expected): raise SystemExit('wrong')")[0])

    def test_workflow_code_is_never_executed(self):
        self.assertEqual(self.audit_gate("__import__('builtins').exec('raise AssertionError()')", ['A']), ([], 0))


if __name__ == '__main__':
    unittest.main()
