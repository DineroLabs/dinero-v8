import subprocess
import sys
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent.parent / "consensus_freeze.py"

ALLOWED = """\
1 modify src/consensus/CMakeLists.txt
2 modify src/consensus/CMakeLists.txt
2 remove src/consensus/coins_db.cpp
2 hunk src/consensus/tx_validation.cpp ^#include "consensus/(coins_db|chain_state_view)\\.h"
2 hunk src/consensus/tx_validation.cpp (CoinsViewCache|ChainStateView)& view
"""

TXV = """\
#include "consensus/coins_db.h"
int a() { return 1; }
int validate(
    CoinsViewCache& view,
    int x) { return x; }
"""


def git(repo, *a):
    subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t", *a],
                   cwd=repo, check=True, capture_output=True)


def run(repo, *args):
    return subprocess.run([sys.executable, str(SCRIPT), *map(str, args)],
                          capture_output=True, text=True, cwd=repo)


def make_repo(tmp_path):
    repo = tmp_path / "repo"
    (repo / "src/consensus").mkdir(parents=True)
    (repo / "include/consensus").mkdir(parents=True)
    (repo / "src/consensus/CMakeLists.txt").write_text("add_library(x a.cpp)\n")
    (repo / "src/consensus/coins_db.cpp").write_text("// coins\n")
    (repo / "src/consensus/tx_validation.cpp").write_text(TXV)
    (repo / "include/consensus/a.h").write_text("#pragma once\n")
    (repo / "allowed.txt").write_text(ALLOWED)
    git(repo, "init", "-q")
    git(repo, "add", "-A")
    git(repo, "commit", "-q", "-m", "base")
    r = run(repo, "record", "--repo", repo, "--commit", "HEAD",
            "--baseline", repo / "base.sha256")
    assert r.returncode == 0, r.stdout + r.stderr
    return repo


def check(repo, stage):
    return run(repo, "check", "--repo", repo, "--baseline", repo / "base.sha256",
               "--allowed", repo / "allowed.txt", "--stage", stage)


def test_clean_tree_passes_stage0(tmp_path):
    repo = make_repo(tmp_path)
    r = check(repo, 0)
    assert r.returncode == 0, r.stdout + r.stderr


def test_unlisted_change_fails(tmp_path):
    repo = make_repo(tmp_path)
    (repo / "include/consensus/a.h").write_text("#pragma once\n// x\n")
    r = check(repo, 2)
    assert r.returncode == 1, r.stdout + r.stderr
    assert "CHANGED include/consensus/a.h" in r.stdout
    (repo / "src/consensus/new.cpp").write_text("x\n")
    (repo / "src/consensus/coins_db.cpp").unlink()
    (repo / "include/consensus/a.h").unlink()
    r = check(repo, 1)
    assert "ADDED src/consensus/new.cpp" in r.stdout
    assert "REMOVED src/consensus/coins_db.cpp" in r.stdout
    assert "REMOVED include/consensus/a.h" in r.stdout


def test_tx_validation_extra_hunk_fails(tmp_path):
    repo = make_repo(tmp_path)
    p = repo / "src/consensus/tx_validation.cpp"
    p.write_text(TXV.replace("return 1;", "return 2;"))
    r = check(repo, 2)
    assert r.returncode == 1, r.stdout + r.stderr
    assert "HUNK src/consensus/tx_validation.cpp:2 " in r.stdout


def test_allowed_stage_passes(tmp_path):
    repo = make_repo(tmp_path)
    (repo / "src/consensus/CMakeLists.txt").write_text("add_library(x b.cpp)\n")
    (repo / "src/consensus/coins_db.cpp").unlink()
    (repo / "src/consensus/tx_validation.cpp").write_text(
        TXV.replace("CoinsViewCache&", "const ChainStateView&")
           .replace('coins_db.h', 'chain_state_view.h'))
    r = check(repo, 2)
    assert r.returncode == 0, r.stdout + r.stderr
    # stage 1 allows CMakeLists only
    r = check(repo, 1)
    assert r.returncode == 1
    assert "REMOVED src/consensus/coins_db.cpp" in r.stdout
    assert "CHANGED src/consensus/tx_validation.cpp" in r.stdout


def test_input_errors_exit_2(tmp_path):
    repo = make_repo(tmp_path)
    r = run(repo, "check", "--repo", repo, "--baseline", repo / "missing",
            "--allowed", repo / "allowed.txt", "--stage", 0)
    assert r.returncode == 2
    (repo / "bad.txt").write_text("9 bogus x\n")
    r = run(repo, "check", "--repo", repo, "--baseline", repo / "base.sha256",
            "--allowed", repo / "bad.txt", "--stage", 0)
    assert r.returncode == 2
