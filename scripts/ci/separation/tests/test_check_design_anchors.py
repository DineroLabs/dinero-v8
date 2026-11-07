import subprocess
import sys
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent.parent / "check_design_anchors.py"


def run(repo, tsv):
    return subprocess.run([sys.executable, str(SCRIPT), str(repo), str(tsv)],
                          capture_output=True, text=True)


def test_reports_moved_and_missing(tmp_path):
    (tmp_path / "a.cpp").write_text("x\ny\nfoo\nz\n")
    tsv = tmp_path / "anchors.tsv"
    tsv.write_text("a.cpp\t2\tfoo\na.cpp\t9\tbar\n")
    r = run(tmp_path, tsv)
    assert r.returncode == 1
    lines = r.stdout.splitlines()
    assert "MOVED a.cpp 2->3" in lines
    assert "MISSING a.cpp:9" in lines


def test_all_match_exit_zero_and_comments_skipped(tmp_path):
    (tmp_path / "a.cpp").write_text("x\nfoo\n")
    tsv = tmp_path / "anchors.tsv"
    tsv.write_text("# path\tline\tsubstring\n\na.cpp\t2\tfoo\n")
    r = run(tmp_path, tsv)
    assert r.returncode == 0, r.stdout


def test_missing_file_is_missing(tmp_path):
    tsv = tmp_path / "anchors.tsv"
    tsv.write_text("nope.cpp\t1\tfoo\n")
    r = run(tmp_path, tsv)
    assert r.returncode == 1
    assert "MISSING nope.cpp:1" in r.stdout
