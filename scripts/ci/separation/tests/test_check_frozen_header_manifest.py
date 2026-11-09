import hashlib
import subprocess
import sys
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent.parent / "check_frozen_header_manifest.py"

HEADER = b"// tx_validation.h\nint v();\n"


def run(repo):
    return subprocess.run([sys.executable, str(SCRIPT), "--repo", str(repo)],
                          capture_output=True, text=True)


def write(repo, header=HEADER, stored=None, entry=True):
    (repo / "include" / "consensus").mkdir(parents=True)
    (repo / "include" / "consensus" / "tx_validation.h").write_bytes(header)
    if stored is None:
        stored = hashlib.sha256(header.replace(b"\r\n", b"\n")).hexdigest()
    line = (f'    {{"include/consensus/tx_validation.h",   "{stored}"}},\n'
            if entry else "")
    (repo / "include" / "consensus" / "freeze.h").write_text(
        "constexpr std::array<ConsensusFileChecksum, 4> CONSENSUS_MANIFEST = {{\n"
        '    {"include/consensus/limits.h",          "' + "0" * 64 + '"},\n'
        + line +
        '    {"include/consensus/freeze.h",          "SELF_REFERENCE"},\n'
        "}};\n")


def test_matching_entry_passes(tmp_path):
    write(tmp_path)
    r = run(tmp_path)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "matches" in r.stdout


def test_stale_entry_fails(tmp_path):
    write(tmp_path, stored="a" * 64)
    r = run(tmp_path)
    assert r.returncode == 1
    assert "but the file hashes to" in r.stdout


def test_missing_entry_fails(tmp_path):
    write(tmp_path, entry=False)
    r = run(tmp_path)
    assert r.returncode == 1
    assert "no CONSENSUS_MANIFEST entry" in r.stdout


def test_missing_file_is_input_error(tmp_path):
    r = run(tmp_path)
    assert r.returncode == 2


def test_crlf_checkout_hashes_as_lf(tmp_path):
    # Windows runners check the header out with CRLF (autocrlf=true, no eol
    # attribute on include/consensus/*.h); the stored hash is of the LF blob.
    lf_hash = hashlib.sha256(HEADER).hexdigest()
    write(tmp_path, header=HEADER.replace(b"\n", b"\r\n"), stored=lf_hash)
    r = run(tmp_path)
    assert r.returncode == 0, r.stdout + r.stderr
