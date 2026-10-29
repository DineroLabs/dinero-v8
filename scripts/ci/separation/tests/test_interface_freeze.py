import subprocess
import types

import pytest

from _w5_load import load

ifz = load("interface_freeze")

JNI_OK = '''
extern "C" JNIEXPORT jint JNICALL
Java_org_dinerolabs_x_N_nativeStart(
    JNIEnv* env, jobject,   jstring datadir) {
    return 0;
}
'''

JNI_BLOCK = '''
extern "C" {
JNIEXPORT jint JNICALL
Java_org_dinerolabs_x_N_nativeStart(JNIEnv* env, jobject) { return 0; }
}
'''


def repo_with(tmp_path, jni):
    (tmp_path / "src/nodecore").mkdir(parents=True)
    (tmp_path / "src/nodecore/nodecore_jni_android.cpp").write_text(jni)
    return tmp_path


def test_jni_definition_is_collapsed_to_one_line(tmp_path):
    assert ifz.jni_exports(str(repo_with(tmp_path, JNI_OK))) == (
        "jint Java_org_dinerolabs_x_N_nativeStart(JNIEnv* env, jobject, jstring datadir)\n")


def test_jniexport_without_per_function_extern_c_is_an_input_error(tmp_path):
    with pytest.raises(ifz.InputError):
        ifz.jni_exports(str(repo_with(tmp_path, JNI_BLOCK)))


def test_unparsed_extra_occurrence_is_an_input_error(tmp_path):
    with pytest.raises(ifz.InputError):
        ifz.jni_exports(str(repo_with(tmp_path, JNI_OK + "// Java_org_dinerolabs_x_N_old\n")))


def fake_nm(monkeypatch, system, stdout):
    monkeypatch.setattr(ifz.platform, "system", lambda: system)
    monkeypatch.setattr(ifz.subprocess, "run", lambda *a, **k: types.SimpleNamespace(
        returncode=0, stdout=stdout, stderr=""))


DARWIN_NM = """\

libnodecore_ffi.a(nodecore_ffi.cpp.o):
0000000000000010 T _nodecore_start
0000000000000020 T __ZN6dinero8nodecore5StartEv
0000000000000030 T ___clang_call_terminate
0000000000000040 S _g_wallet_manager
"""

LINUX_NM = """\
nodecore_ffi.cpp.o:
0000000000000010 T nodecore_start
0000000000000020 T _ZN6dinero8nodecore5StartEv
0000000000000030 W __clang_call_terminate
0000000000000040 B g_wallet_manager
"""


def test_darwin_cpp_and_clang_symbols_filtered_c_symbols_kept(monkeypatch):
    fake_nm(monkeypatch, "Darwin", DARWIN_NM)
    assert ifz.symbols("x.a") == {("T", "_nodecore_start"), ("S", "_g_wallet_manager")}


def test_linux_cpp_and_clang_symbols_filtered_c_symbols_kept(monkeypatch):
    fake_nm(monkeypatch, "Linux", LINUX_NM)
    assert ifz.symbols("x.a") == {("T", "nodecore_start"), ("B", "g_wallet_manager")}


def test_archive_with_no_c_symbols_is_an_input_error(monkeypatch):
    fake_nm(monkeypatch, "Darwin", "0000000000000020 T __ZN6dinero5StartEv\n")
    with pytest.raises(ifz.InputError):
        ifz.symbols("x.a")


def make_tree(tmp_path):
    repo = repo_with(tmp_path / "repo", JNI_OK)
    (repo / "include/nodecore").mkdir(parents=True)
    (repo / "wallet-core/ffi").mkdir(parents=True)
    (repo / "include/nodecore/nodecore_ffi.h").write_text("int32_t nodecore_stop(void);\n")
    (repo / "wallet-core/ffi/wallet_ffi.h").write_text("int dinero_wallet_create(void);\n")
    build = tmp_path / "build"
    build.mkdir()
    (build / "CMakeCache.txt").write_text("DINERO_BUILD_ORCHARD_BACKEND:BOOL=OFF\n")
    for stem in ("libnodecore_ffi", "libdinero_wallet_ffi"):
        (build / (stem + ".a")).write_text("")
    return repo, build


def run_main(monkeypatch, repo, build, base, mode, syms):
    monkeypatch.setattr(ifz, "symbols", lambda path: syms)
    return ifz.main([mode, "--build", str(build), "--repo", str(repo), "--baseline-dir", str(base)])


def test_record_check_pass_then_header_symbol_and_missing_baseline(tmp_path, monkeypatch, capsys):
    repo, build = make_tree(tmp_path)
    base = tmp_path / "base"
    syms = {("T", "_nodecore_stop")}
    assert run_main(monkeypatch, repo, build, base, "record", syms) == 0
    assert run_main(monkeypatch, repo, build, base, "check", syms) == 0
    assert run_main(monkeypatch, repo, build, base, "check", set(syms) | {("T", "_new")}) == 1
    (repo / "include/nodecore/nodecore_ffi.h").write_text("int32_t nodecore_stop(int f);\n")
    assert run_main(monkeypatch, repo, build, base, "check", syms) == 1
    (build / "CMakeCache.txt").write_text("DINERO_BUILD_ORCHARD_BACKEND:BOOL=ON\n")
    assert run_main(monkeypatch, repo, build, base, "check", syms) == 2   # no ON baseline
