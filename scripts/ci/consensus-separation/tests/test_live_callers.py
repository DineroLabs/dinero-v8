"""CV3 live-caller gate tests (fake compile db + fake CMake File API reply)."""
import json, os, subprocess, sys, tempfile, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "..", "live_callers.py")


class LiveCallers(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.root = os.path.join(self.tmp, "root"); self.build = os.path.join(self.tmp, "build")
        os.makedirs(os.path.join(self.root, "src")); os.makedirs(self.build)
        self.rules = os.path.join(self.tmp, "rules.json"); self.base = os.path.join(self.tmp, "base.json")
        # dinerod -> dinero_core -> dinero_consensus ; test_x is separate
        self.graph = {"dinerod": ["dinero_core"], "dinero_core": ["dinero_consensus"], "dinero_consensus": [], "test_x": ["dinero_consensus"]}
        self.files = {}
        self.write_rules([{"id": "VT", "pattern": r"consensus::validateTransaction\s*\(", "exclude": ["src/def.cpp"]}])

    def write_rules(self, entries, roots=("dinerod",)):
        json.dump({"roots": list(roots), "entry_points": entries}, open(self.rules, "w"))

    def src(self, rel, body, target=None):
        p = os.path.join(self.root, rel); os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, "w").write(body)
        if target:
            self.files[rel] = target
        self.flush()

    def flush(self):
        cc = [{"directory": self.build, "file": os.path.join(self.root, f), "output": "CMakeFiles/%s.dir/%s.o" % (t, f),
               "command": "c++ -c %s" % f} for f, t in self.files.items()]
        json.dump(cc, open(os.path.join(self.build, "compile_commands.json"), "w"))
        reply = os.path.join(self.build, ".cmake/api/v1/reply"); os.makedirs(reply, exist_ok=True)
        targets = []
        for name, deps in self.graph.items():
            jf = "target-%s.json" % name
            json.dump({"name": name, "id": name + "::@1", "dependencies": [{"id": d + "::@1"} for d in deps]},
                      open(os.path.join(reply, jf), "w"))
            targets.append({"name": name, "id": name + "::@1", "jsonFile": jf})
        json.dump({"configurations": [{"targets": targets}]}, open(os.path.join(reply, "codemodel-v2-x.json"), "w"))
        json.dump({"objects": [{"kind": "codemodel", "jsonFile": "codemodel-v2-x.json"}]},
                  open(os.path.join(reply, "index-1.json"), "w"))

    def run_tool(self, mode):
        return subprocess.run([sys.executable, TOOL, mode, self.build, self.root, self.rules, self.base],
                              capture_output=True, text=True)

    def recorded(self):
        r = self.run_tool("record"); self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return json.load(open(self.base))["callers"]

    def test_classifies_live_test_uncompiled_and_excludes_definition(self):
        self.src("src/def.cpp", "X consensus::validateTransaction(a);", "dinero_consensus")
        self.src("src/live.cpp", "y = consensus::validateTransaction(tx);", "dinero_core")
        self.src("src/t.cpp", "consensus::validateTransaction (tx);", "test_x")
        self.src("src/dead.cpp", "consensus::validateTransaction(tx);")  # in no target
        self.assertEqual(self.recorded()["VT"], {"src/dead.cpp": "uncompiled", "src/live.cpp": "live", "src/t.cpp": "other-target"})

    def test_comment_only_line_is_not_a_caller(self):
        self.src("src/live.cpp", "  // consensus::validateTransaction(tx) used to be here\n", "dinero_core")
        self.assertEqual(self.recorded()["VT"], {})

    def test_header_caller_is_reported_as_header(self):
        self.src("src/other.cpp", "int x;", "dinero_core")
        self.src("include/h.h", "inline void f(){ consensus::validateTransaction(t); }")
        self.assertEqual(self.recorded()["VT"], {"include/h.h": "header"})

    def test_unchanged_passes_and_new_live_caller_fails(self):
        self.src("src/t.cpp", "consensus::validateTransaction(tx);", "test_x")
        self.recorded()
        self.assertEqual(self.run_tool("check").returncode, 0)
        self.src("src/live.cpp", "consensus::validateTransaction(tx);", "dinero_core")
        r = self.run_tool("check")
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn("CHANGED VT src/live.cpp: absent -> live", r.stdout)

    def test_lost_live_caller_fails(self):
        self.src("src/live.cpp", "consensus::validateTransaction(tx);", "dinero_core")
        self.recorded()
        self.src("src/live.cpp", "nothing();", "dinero_core")
        r = self.run_tool("check")
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn("CHANGED VT src/live.cpp: live -> absent", r.stdout)

    def test_unknown_root_is_bad_input(self):
        self.write_rules([{"id": "VT", "pattern": "x\\(", "exclude": []}], roots=("nodaemon",))
        self.src("src/a.cpp", "", "dinero_core")
        r = self.run_tool("record")
        self.assertEqual(r.returncode, 2)
        self.assertIn("ERROR: root target nodaemon not in codemodel", r.stderr)

    def test_missing_file_api_reply_is_bad_input(self):
        self.src("src/a.cpp", "", "dinero_core")
        import shutil; shutil.rmtree(os.path.join(self.build, ".cmake"))
        r = self.run_tool("record")
        self.assertEqual(r.returncode, 2)
        self.assertIn("ERROR: no CMake File API reply", r.stderr)


if __name__ == "__main__":
    unittest.main()
