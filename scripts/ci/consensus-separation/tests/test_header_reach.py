"""CV1 header-reach gate tests. They run the real C++ preprocessor on a tiny temp tree."""
import json, os, shutil, subprocess, sys, tempfile, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "..", "header_reach.py")
CXX = shutil.which("c++") or shutil.which("clang++")


@unittest.skipUnless(CXX, "needs a C++ compiler")
class HeaderReach(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.root = os.path.join(self.tmp, "root")
        self.build = os.path.join(self.tmp, "build")
        os.makedirs(self.build)
        self.hdr("include/storage/db.h", "")
        self.hdr("include/mining/header_layout.h", "")
        self.hdr("include/consensus/pure.h", "")
        self.base = os.path.join(self.tmp, "base.json")
        self.rules = os.path.join(self.tmp, "rules.json")
        self.sources = {}
        self.set_rules()

    def hdr(self, rel, body):
        p = os.path.join(self.root, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, "w").write(body)

    def src(self, rel, includes, target="dinero_consensus", extra=""):
        self.hdr(rel, "".join('#include "%s"\n' % i for i in includes) + extra + "int f_%d(){return 0;}\n" % len(self.sources))
        self.sources[rel] = target
        cc = [{"directory": self.build, "file": os.path.join(self.root, s),
               "output": "CMakeFiles/%s.dir/%s.o" % (t, s),
               "command": "%s -I%s/include -c %s -o CMakeFiles/%s.dir/%s.o" % (CXX, self.root, os.path.join(self.root, s), t, s)}
              for s, t in self.sources.items()]
        json.dump(cc, open(os.path.join(self.build, "compile_commands.json"), "w"))

    def set_rules(self, bucket_a=(), allow=("include/mining/header_layout.h",), controls=()):
        json.dump({"targets": ["dinero_consensus"], "bucket_a_sources": list(bucket_a),
                   "allowlisted_headers": list(allow), "controls": list(controls)}, open(self.rules, "w"))

    def run_tool(self, mode):
        return subprocess.run([sys.executable, TOOL, mode, self.build, self.root, self.rules, self.base],
                              capture_output=True, text=True)

    def record_ok(self):
        r = self.run_tool("record")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_record_finds_forbidden_reach(self):
        self.src("src/a.cpp", ["storage/db.h", "consensus/pure.h"])
        self.record_ok()
        self.assertEqual(json.load(open(self.base))["reach"], {"src/a.cpp": ["include/storage/db.h"]})

    def test_allowlisted_header_is_ignored(self):
        self.src("src/a.cpp", ["mining/header_layout.h"])
        self.record_ok()
        self.assertEqual(json.load(open(self.base))["reach"], {})

    def test_unchanged_passes(self):
        self.src("src/a.cpp", ["storage/db.h"])
        self.record_ok()
        r = self.run_tool("check")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_new_reach_fails(self):
        self.src("src/a.cpp", ["consensus/pure.h"])
        self.record_ok()
        self.src("src/a.cpp", ["consensus/pure.h", "storage/db.h"])
        r = self.run_tool("check")
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn("NEW_REACH src/a.cpp -> include/storage/db.h", r.stdout)

    def test_transitive_reach_is_seen(self):
        self.hdr("include/consensus/leaky.h", '#include "storage/db.h"\n')
        self.src("src/a.cpp", ["consensus/leaky.h"])
        self.record_ok()
        self.assertEqual(json.load(open(self.base))["reach"], {"src/a.cpp": ["include/storage/db.h"]})

    def test_bucket_a_reach_is_reported_not_failing(self):
        self.src("src/a.cpp", [])
        self.set_rules(bucket_a=["src/a.cpp"])
        self.record_ok()
        self.src("src/a.cpp", ["storage/db.h"])
        r = self.run_tool("check")
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("BUCKET_A src/a.cpp -> include/storage/db.h", r.stdout)

    def test_resolved_reach_is_reported_not_failing(self):
        self.src("src/a.cpp", ["storage/db.h"])
        self.record_ok()
        self.src("src/a.cpp", [])
        r = self.run_tool("check")
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("RESOLVED src/a.cpp -> include/storage/db.h", r.stdout)

    def test_unresolved_dependency_fails(self):
        self.src("src/a.cpp", ["storage/not_there.h"])
        r = self.run_tool("record")
        self.assertEqual(r.returncode, 2, r.stdout)
        self.assertIn("UNRESOLVED storage/not_there.h", r.stdout)

    def test_untrusted_check_still_reports_new_reach(self):
        self.src("src/a.cpp", [])
        self.record_ok()
        self.src("src/a.cpp", ["storage/db.h", "storage/not_there.h"])
        r = self.run_tool("check")
        self.assertEqual(r.returncode, 2, r.stdout)
        self.assertIn("NEW_REACH src/a.cpp -> include/storage/db.h", r.stdout)
        self.assertIn("UNRESOLVED storage/not_there.h", r.stdout)

    def test_preprocess_error_fails(self):
        self.src("src/a.cpp", [], extra="#error boom\n")
        r = self.run_tool("record")
        self.assertEqual(r.returncode, 2, r.stdout)
        self.assertIn("PREPROCESS_ERROR src/a.cpp", r.stdout)

    def test_external_rocksdb_header_is_a_family(self):
        self.hdr("third_party/rocksdb/include/rocksdb/db.h", "")
        self.src("src/a.cpp", ["../third_party/rocksdb/include/rocksdb/db.h"])
        self.record_ok()
        self.assertEqual(json.load(open(self.base))["family"], {"third_party/rocksdb/include/rocksdb/db.h": "rocksdb"})

    def test_positive_control_must_hit(self):
        self.src("src/a.cpp", ["storage/db.h"])
        self.src("src/b.cpp", [])
        self.set_rules(controls=[{"source": "src/b.cpp", "family": "storage"}])
        r = self.run_tool("record")
        self.assertEqual(r.returncode, 2, r.stdout)
        self.assertIn("CONTROL src/b.cpp -> storage: FAIL", r.stdout)
        self.set_rules(controls=[{"source": "src/a.cpp", "family": "storage"}])
        self.record_ok()

    def test_no_entries_for_target_fails(self):
        self.src("src/a.cpp", [], target="dinero_other")
        r = self.run_tool("record")
        self.assertEqual(r.returncode, 2)
        self.assertIn("ERROR: no compile entries for dinero_consensus", r.stderr)


if __name__ == "__main__":
    unittest.main()
