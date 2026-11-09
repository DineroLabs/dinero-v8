import subprocess
import sys
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent.parent / "changed_paths.py"

PATTERNS = ["scripts/ci/separation/**", "CMakeLists.txt", "**/CMakeLists.txt",
            "src/**", "mobile-tauri/src-tauri/build.rs"]


def git(repo, *a):
    subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t", *a],
                   cwd=repo, check=True, capture_output=True)


def repo_with_pr(tmp_path, files):
    """A base commit, a PR commit changing `files`, and a merge commit of the
    two (parent 1 = base), like actions/checkout's refs/pull/N/merge."""
    git(tmp_path, "init", "-b", "main")
    (tmp_path / "README.md").write_text("base\n")
    git(tmp_path, "add", "-A")
    git(tmp_path, "commit", "-m", "base")
    git(tmp_path, "checkout", "-b", "topic")
    for f in files:
        p = tmp_path / f
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text("change\n")
    git(tmp_path, "add", "-A")
    git(tmp_path, "commit", "-m", "topic")
    git(tmp_path, "checkout", "main")
    git(tmp_path, "merge", "--no-ff", "topic", "-m", "merge")


def run(repo, event="pull_request", patterns=PATTERNS):
    out = repo / "gh_output.txt"
    out.write_text("")
    r = subprocess.run(
        [sys.executable, str(SCRIPT), "--github-output", str(out),
         "--key", "gates", "--event", event, "--", *patterns],
        capture_output=True, text=True, cwd=repo)
    return r, out.read_text()


def test_gate_path_change_is_relevant(tmp_path):
    repo_with_pr(tmp_path, ["src/consensus/tx_validation.cpp"])
    r, out = run(tmp_path)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "gates=true" in out
    assert "src/consensus/tx_validation.cpp" in r.stdout


def test_docs_only_change_is_not_relevant(tmp_path):
    repo_with_pr(tmp_path, ["docs/guide.md", "packaging/notes.txt"])
    r, out = run(tmp_path)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "gates=false" in out


def test_nested_cmakelists_matches_double_star(tmp_path):
    repo_with_pr(tmp_path, ["tests/integration/CMakeLists.txt"])
    r, out = run(tmp_path)
    assert "gates=true" in out


def test_root_cmakelists_matches(tmp_path):
    repo_with_pr(tmp_path, ["CMakeLists.txt"])
    r, out = run(tmp_path)
    assert "gates=true" in out


def test_exact_file_pattern(tmp_path):
    repo_with_pr(tmp_path, ["mobile-tauri/src-tauri/build.rs"])
    r, out = run(tmp_path)
    assert "gates=true" in out


def test_non_pr_event_fails_open(tmp_path):
    repo_with_pr(tmp_path, ["docs/guide.md"])
    r, out = run(tmp_path, event="workflow_dispatch")
    assert r.returncode == 0
    assert "gates=true" in out


def test_git_failure_fails_open(tmp_path):
    # No git repository at all: the diff fails, the answer must be true.
    out = tmp_path / "gh_output.txt"
    out.write_text("")
    r = subprocess.run(
        [sys.executable, str(SCRIPT), "--github-output", str(out),
         "--key", "gates", "--event", "pull_request", "--", *PATTERNS],
        capture_output=True, text=True, cwd=tmp_path)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "gates=true" in out.read_text()
    assert "fail open" in r.stdout


def test_prefix_pattern_does_not_match_lookalike_dir(tmp_path):
    # 'src/**' must not match 'src2/x' or 'mysrc/x'.
    repo_with_pr(tmp_path, ["src2/x.cpp", "mysrc/y.cpp"])
    r, out = run(tmp_path)
    assert "gates=false" in out
