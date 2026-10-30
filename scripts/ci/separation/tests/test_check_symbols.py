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
    assert r.stderr.startswith("ERROR "), r.stderr


def test_empty_nm_output_is_input_error(tmp_path):
    nm = tmp_path / "nm.txt"
    nm.write_text("")
    r = run("--nm-output", nm, "--gate", "K3")
    assert r.returncode == 2
    assert r.stderr.startswith("ERROR "), r.stderr


# --- --control and --coff-archive (Windows K3/W3) ---

import os
import shutil
import stat

import pytest

CONTROLLED_NM = CLEAN_NM + "0000000100004200 T dinero::consensus::EvalScript(char const*, int)\n"


def test_control_present_is_measured(tmp_path):
    nm = tmp_path / "nm.txt"
    nm.write_text(CONTROLLED_NM)
    r = run("--nm-output", nm, "--gate", "K3", "--control", "dinero::consensus::EvalScript")
    assert r.returncode == 0, r.stdout + r.stderr
    assert "UNMEASURED" not in r.stdout


def test_control_missing_is_unmeasured_even_with_hits(tmp_path):
    nm = tmp_path / "nm.txt"
    nm.write_text(CLEAN_NM + "0000000100005000 T rocksdb::DB::Open\n")
    r = run("--nm-output", nm, "--gate", "K3", "--control", "dinero::consensus::EvalScript")
    assert r.returncode == 3, r.stdout + r.stderr
    assert any(l.startswith("UNMEASURED control symbol") for l in r.stdout.splitlines())


def test_control_only_undefined_is_unmeasured(tmp_path):
    nm = tmp_path / "nm.txt"
    nm.write_text(CLEAN_NM + "                 U dinero::consensus::EvalScript(char const*, int)\n")
    r = run("--nm-output", nm, "--gate", "K3", "--control", "dinero::consensus::EvalScript")
    assert r.returncode == 3


def test_control_mangled_output_is_unmeasured(tmp_path):
    nm = tmp_path / "nm.txt"
    nm.write_text("00000000 T ?EvalScript@consensus@dinero@@YA_NPEBDH@Z\n")
    r = run("--nm-output", nm, "--gate", "K3", "--control", "EvalScript")
    assert r.returncode == 3
    assert "no defined demangled dinero:: symbol" in r.stdout


def test_several_binaries_need_coff_archive(tmp_path):
    a = tmp_path / "a"
    a.write_bytes(b"x")
    r = run("--binary", a, "--binary", a, "--gate", "K3")
    assert r.returncode == 2
    assert "need --coff-archive" in r.stderr


def test_coff_archive_rejects_non_archive(tmp_path):
    exe = tmp_path / "probe.exe"
    exe.write_bytes(b"MZ\x90\x00")
    r = run("--coff-archive", "--binary", exe, "--nm", sys.executable, "--gate", "K3")
    assert r.returncode == 2
    assert "is not an archive" in r.stderr


def test_coff_archive_rejects_non_llvm_nm(tmp_path):
    lib = tmp_path / "x.lib"
    lib.write_bytes(b"!<arch>\n")
    fake = tmp_path / "nm"
    fake.write_text("#!/bin/sh\necho 'GNU nm (GNU Binutils) 2.42'\n")
    fake.chmod(fake.stat().st_mode | stat.S_IEXEC)
    r = run("--coff-archive", "--binary", lib, "--nm", fake, "--gate", "K3")
    assert r.returncode == 2
    assert "needs llvm-nm" in r.stderr


def _llvm_bin():
    for d in (os.environ.get("SEP_LLVM_BIN"), "/opt/homebrew/opt/llvm/bin", "/usr/local/opt/llvm/bin"):
        if d and all(os.path.isfile(os.path.join(d, t)) for t in ("clang++", "llvm-lib", "llvm-nm")):
            return d
    return None


COFF_SRC = """
namespace dinero { namespace consensus { bool EvalScript(const char* s, int n) { return s && n > 0; } } }
extern "C" int dinero_c_export(int x) { return x + 1; }
"""
PLANTED = """
namespace rocksdb { struct DB { static int Open(const char*); }; }
int use_rocks() { return rocksdb::DB::Open("x"); }
"""


def _coff_lib(tmp_path, name, src):
    d = _llvm_bin()
    cpp = tmp_path / (name + ".cpp")
    cpp.write_text(src)
    obj = tmp_path / (name + ".obj")
    lib = tmp_path / (name + ".lib")
    subprocess.run([os.path.join(d, "clang++"), "--target=x86_64-pc-windows-msvc", "-c",
                    str(cpp), "-o", str(obj)], check=True)
    subprocess.run([os.path.join(d, "llvm-lib"), "/out:" + str(lib), str(obj)], check=True)
    return lib


@pytest.mark.skipif(_llvm_bin() is None, reason="no LLVM toolchain (clang++, llvm-lib, llvm-nm)")
def test_coff_archive_real_msvc_objects(tmp_path):
    nm = os.path.join(_llvm_bin(), "llvm-nm")
    clean = _coff_lib(tmp_path, "clean", COFF_SRC)
    planted = _coff_lib(tmp_path, "planted", COFF_SRC + PLANTED)
    ok = run("--coff-archive", "--binary", clean, "--nm", nm, "--gate", "K3",
             "--control", "dinero::consensus::EvalScript")
    assert ok.returncode == 0, ok.stdout + ok.stderr
    bad = run("--coff-archive", "--binary", clean, "--binary", planted, "--nm", nm,
              "--gate", "K3", "--control", "dinero::consensus::EvalScript")
    assert bad.returncode == 1, bad.stdout + bad.stderr
    assert forbidden(bad) == [
        "FORBIDDEN rocksdb:: public: static int __cdecl rocksdb::DB::Open(char const *)"]
