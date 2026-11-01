"""CV4 duplicate-object gate tests (spec 2026-11-01-consensus-validation-design.md section 3)."""
import json, os, subprocess, sys, tempfile, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "..", "dup_objects.py")


def entry(root, src, target, build):
    return {"directory": build, "file": os.path.join(root, src),
            "output": "CMakeFiles/%s.dir/%s.o" % (target, src),
            "command": "c++ -c %s -o CMakeFiles/%s.dir/%s.o" % (os.path.join(root, src), target, src)}


class DupObjects(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.root = os.path.join(self.tmp, "src_root")
        self.build = os.path.join(self.tmp, "build")
        os.makedirs(self.root); os.makedirs(self.build)
        self.base = os.path.join(self.tmp, "baseline.json")

    def write(self, entries):
        json.dump(entries, open(os.path.join(self.build, "compile_commands.json"), "w"))

    def run_tool(self, *args):
        return subprocess.run([sys.executable, TOOL, *args], capture_output=True, text=True)

    def record(self):
        r = self.run_tool("record", self.build, self.root, self.base)
        self.assertEqual(r.returncode, 0, r.stderr)

    def check(self):
        return self.run_tool("check", self.build, self.root, self.base)

    def test_record_lists_only_library_duplicates(self):
        self.write([entry(self.root, "a.cpp", "dinero_core", self.build),
                    entry(self.root, "a.cpp", "dinero_consensus", self.build),
                    entry(self.root, "b.cpp", "dinero_core", self.build),
                    entry(self.root, "b.cpp", "test_b", self.build),          # tests don't count
                    entry(self.root, "c.cpp", "dinero_core", self.build),
                    entry(self.root, "c.cpp", "dinero_test_runtime", self.build)])  # test libs don't count
        self.record()
        self.assertEqual(json.load(open(self.base))["duplicates"],
                         {"a.cpp": ["dinero_consensus", "dinero_core"]})

    def test_unchanged_passes(self):
        self.write([entry(self.root, "a.cpp", "dinero_core", self.build),
                    entry(self.root, "a.cpp", "dinero_consensus", self.build)])
        self.record()
        r = self.check()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_new_duplicate_fails(self):
        self.write([entry(self.root, "a.cpp", "dinero_core", self.build)])
        self.record()
        self.write([entry(self.root, "a.cpp", "dinero_core", self.build),
                    entry(self.root, "a.cpp", "dinero_wallet", self.build)])
        r = self.check()
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn("NEW_DUPLICATE a.cpp", r.stdout)

    def test_extra_target_on_known_duplicate_fails(self):
        self.write([entry(self.root, "a.cpp", "dinero_core", self.build),
                    entry(self.root, "a.cpp", "dinero_consensus", self.build)])
        self.record()
        self.write([entry(self.root, "a.cpp", "dinero_core", self.build),
                    entry(self.root, "a.cpp", "dinero_consensus", self.build),
                    entry(self.root, "a.cpp", "dinero_wallet", self.build)])
        r = self.check()
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn("NEW_DUPLICATE a.cpp", r.stdout)

    def test_removed_duplicate_is_reported_not_failing(self):
        self.write([entry(self.root, "a.cpp", "dinero_core", self.build),
                    entry(self.root, "a.cpp", "dinero_consensus", self.build)])
        self.record()
        self.write([entry(self.root, "a.cpp", "dinero_core", self.build)])
        r = self.check()
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("RESOLVED a.cpp", r.stdout)

    def test_empty_compile_db_is_an_error(self):
        self.write([])
        r = self.run_tool("record", self.build, self.root, self.base)
        self.assertEqual(r.returncode, 2)
        self.assertIn("ERROR: no compile entries", r.stderr)

    def test_entry_without_target_dir_is_an_error(self):
        e = entry(self.root, "a.cpp", "dinero_core", self.build)
        e["output"] = "a.o"; e["command"] = "c++ -c a.cpp -o a.o"
        self.write([e])
        r = self.run_tool("record", self.build, self.root, self.base)
        self.assertEqual(r.returncode, 2)
        self.assertIn("ERROR: no CMake target", r.stderr)


if __name__ == "__main__":
    unittest.main()
