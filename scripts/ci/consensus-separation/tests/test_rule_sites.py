"""CV2 rule-site inventory tests."""
import os, subprocess, sys, tempfile, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "..", "rule_sites.py")
HEAD = "rule_id\tfile\tfunction\tanchor\tnote\n"


class RuleSites(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        os.makedirs(os.path.join(self.root, "src"))
        open(os.path.join(self.root, "src/a.cpp"), "w").write(
            "bool Check(){\n  if (sigops > MAX) return false;\n  return true;\n}\n// twice\n// twice\n")
        self.tsv = os.path.join(self.root, "sites.tsv")

    def write(self, *rows):
        open(self.tsv, "w").write(HEAD + "".join("\t".join(r) + "\n" for r in rows))

    def run_tool(self):
        return subprocess.run([sys.executable, TOOL, self.root, self.tsv], capture_output=True, text=True)

    def test_anchor_found_once_passes(self):
        self.write(("R1", "src/a.cpp", "Check", "if (sigops > MAX)", "n"))
        r = self.run_tool()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("R1 src/a.cpp:2", r.stdout)

    def test_missing_anchor_fails(self):
        self.write(("R1", "src/a.cpp", "Check", "if (count > MAX)", "n"))
        r = self.run_tool()
        self.assertEqual(r.returncode, 1)
        self.assertIn("MISSING R1", r.stdout)

    def test_duplicate_anchor_fails(self):
        self.write(("R1", "src/a.cpp", "Check", "// twice", "n"))
        r = self.run_tool()
        self.assertEqual(r.returncode, 1)
        self.assertIn("AMBIGUOUS R1", r.stdout)

    def test_missing_file_fails(self):
        self.write(("R1", "src/gone.cpp", "Check", "x", "n"))
        r = self.run_tool()
        self.assertEqual(r.returncode, 1)
        self.assertIn("NO_FILE R1", r.stdout)

    def test_duplicate_rule_id_is_bad_input(self):
        self.write(("R1", "src/a.cpp", "Check", "if (sigops > MAX)", "n"),
                   ("R1", "src/a.cpp", "Check", "return true;", "n"))
        r = self.run_tool()
        self.assertEqual(r.returncode, 2)
        self.assertIn("ERROR: duplicate rule_id R1", r.stderr)

    def test_bad_header_is_bad_input(self):
        open(self.tsv, "w").write("id\tfile\n")
        r = self.run_tool()
        self.assertEqual(r.returncode, 2)
        self.assertIn("ERROR: header", r.stderr)

    def test_empty_inventory_is_bad_input(self):
        open(self.tsv, "w").write(HEAD)
        r = self.run_tool()
        self.assertEqual(r.returncode, 2)
        self.assertIn("ERROR: no rows", r.stderr)


if __name__ == "__main__":
    unittest.main()
