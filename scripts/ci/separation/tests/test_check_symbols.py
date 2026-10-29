import subprocess
import sys
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent.parent / "check_symbols.py"

CLEAN_NM = """\
0000000100003f50 T _main
0000000100004000 T dinero::consensus::VerifyScript(dinero::consensus::Script const&)
0000000100004100 t dinero::detail::ParamsImpl()
                 U _memcpy
"""


def run(*args):
    return subprocess.run([sys.executable, str(SCRIPT), *map(str, args)],
                          capture_output=True, text=True)


def forbidden(r):
    return [l for l in r.stdout.splitlines() if l.startswith("FORBIDDEN ")]


def test_check_symbols_flags_rocksdb(tmp_path):
    nm = tmp_path / "nm.txt"
    nm.write_text(CLEAN_NM + "0000000100005000 T rocksdb::DB::Open\n")
    r = run("--nm-output", nm, "--gate", "K3")
    assert r.returncode == 1, r.stdout + r.stderr
    assert forbidden(r) == ["FORBIDDEN rocksdb:: rocksdb::DB::Open"]


def test_check_symbols_clean(tmp_path):
    nm = tmp_path / "nm.txt"
    nm.write_text(CLEAN_NM)
    for gate in ("K3", "W3"):
        r = run("--nm-output", nm, "--gate", gate)
        assert r.returncode == 0, r.stdout + r.stderr
        assert forbidden(r) == []


def test_gate_pattern_sets_differ(tmp_path):
    nm = tmp_path / "nm.txt"
    nm.write_text(CLEAN_NM
                  + "                 U _sqlite3_open_v2\n"
                  + "0000000100006000 T dinero::Address::fromString(std::string const&)\n"
                  + "0000000100007000 T dinero::BlockStorage::ReadBlock(int)\n"
                  + "0000000100008000 T dinero::p2p::PeerManager::Start()\n")
    k3 = run("--nm-output", nm, "--gate", "K3")
    assert k3.returncode == 1
    assert sorted(forbidden(k3)) == sorted([
        "FORBIDDEN sqlite3_ _sqlite3_open_v2",
        "FORBIDDEN dinero::Address:: dinero::Address::fromString(std::string const&)",
    ])
    w3 = run("--nm-output", nm, "--gate", "W3")
    assert w3.returncode == 1
    assert sorted(forbidden(w3)) == sorted([
        "FORBIDDEN dinero::BlockStorage dinero::BlockStorage::ReadBlock(int)",
        "FORBIDDEN PeerManager dinero::p2p::PeerManager::Start()",
    ])


def test_every_listed_pattern_is_detected(tmp_path):
    k3 = ["rocksdb::", "sqlite3_", "dinero::ChainDB", "ChainstateService",
          "dinero::Address::", "dinero::Mempool"]
    w3 = ["rocksdb::", "dinero::ChainDB", "ChainstateService", "dinero::Mempool",
          "dinero::BlockStorage", "HeaderSyncManager", "PeerManager",
          "TxInclusionAnalyzer"]
    for gate, pats in (("K3", k3), ("W3", w3)):
        for p in pats:
            nm = tmp_path / "nm.txt"
            nm.write_text(CLEAN_NM + "0000000100009000 T x" + p + "y\n")
            r = run("--nm-output", nm, "--gate", gate)
            assert r.returncode == 1, (gate, p)
            assert forbidden(r) == ["FORBIDDEN %s x%sy" % (p, p)], (gate, p)


def test_duplicate_symbol_reported_once(tmp_path):
    nm = tmp_path / "nm.txt"
    nm.write_text(CLEAN_NM + "0000000100005000 T rocksdb::DB::Open\n"
                  "0000000100005000 T rocksdb::DB::Open\n")
    r = run("--nm-output", nm, "--gate", "K3")
    assert forbidden(r) == ["FORBIDDEN rocksdb:: rocksdb::DB::Open"]


def test_missing_binary_is_input_error(tmp_path):
    r = run("--binary", tmp_path / "nope", "--gate", "K3")
    assert r.returncode == 2


def test_empty_nm_output_is_input_error(tmp_path):
    nm = tmp_path / "nm.txt"
    nm.write_text("")
    r = run("--nm-output", nm, "--gate", "K3")
    assert r.returncode == 2
