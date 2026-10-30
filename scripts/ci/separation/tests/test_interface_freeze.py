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


# --- Windows MSVC .lib path ---

import os

WIN_NM = """\

nodecore_ffi.cpp.obj:
0000000000000000 T nodecore_start
0000000000000010 T nodecore_stop
0000000000000020 T ?Start@nodecore@dinero@@YAXXZ
0000000000000000 R __real@3ff0000000000000
0000000000000000 B g_wallet_manager
"""


def capture_nm(monkeypatch, stdout):
    calls = []

    def run(cmd, **k):
        calls.append(cmd)
        return types.SimpleNamespace(returncode=0, stdout=stdout, stderr="")
    monkeypatch.setattr(ifz.subprocess, "run", run)
    return calls


def test_lib_path_selects_windows_llvm_nm_command(monkeypatch):
    # platform.system() must not matter: the .lib input decides.
    monkeypatch.setattr(ifz.platform, "system", lambda: "Darwin")
    calls = capture_nm(monkeypatch, WIN_NM)
    got = ifz.symbols("C:/b/nodecore_ffi.lib")
    assert calls == [["llvm-nm", "--extern-only", "--defined-only", "C:/b/nodecore_ffi.lib"]]
    assert got == {("T", "nodecore_start"), ("T", "nodecore_stop"), ("B", "g_wallet_manager")}
    ifz.symbols("x.LIB", nm="C:/LLVM/bin/llvm-nm.exe")
    assert calls[-1][0] == "C:/LLVM/bin/llvm-nm.exe"


def test_non_lib_paths_keep_posix_commands(monkeypatch):
    calls = capture_nm(monkeypatch, LINUX_NM)
    monkeypatch.setattr(ifz.platform, "system", lambda: "Linux")
    ifz.symbols("libx.a")
    ifz.symbols("libx.so")
    monkeypatch.setattr(ifz.platform, "system", lambda: "Darwin")
    ifz.symbols("libx.a")
    assert calls == [["nm", "-g", "--defined-only", "libx.a"],
                     ["nm", "-D", "--defined-only", "libx.so"],
                     ["nm", "-gU", "libx.a"]]


def win_tree(tmp_path):
    base = tmp_path / "base"
    base.mkdir()
    (base / "nodecore_ffi.h").write_text(
        "/* nodecore_legacy(void) is gone */\n"
        "int32_t nodecore_start(const char* d);\nint32_t nodecore_stop(void);\n")
    (base / "wallet_ffi.h").write_text("int dinero_wallet_create(void);\n")
    build = tmp_path / "build"
    build.mkdir()
    for n in ("nodecore_ffi.lib", "dinero_wallet_ffi.lib"):
        (build / n).write_text("")
    args = types.SimpleNamespace(nodecore_ffi=str(build / "nodecore_ffi.lib"),
                                 wallet_ffi=str(build / "dinero_wallet_ffi.lib"),
                                 nm=None, baseline_dir=str(base))
    return build, args


def test_lib_with_every_declared_export_passes(tmp_path, monkeypatch):
    build, args = win_tree(tmp_path)
    table = {"nodecore_ffi.lib": {("T", "nodecore_start"), ("T", "nodecore_stop")},
             "dinero_wallet_ffi.lib": {("T", "dinero_wallet_create")}}
    monkeypatch.setattr(ifz, "symbols", lambda path, nm=None: table[os.path.basename(path)])
    assert ifz.symbol_text(str(build), args) == (
        "dinero_wallet_ffi T dinero_wallet_create\n"
        "nodecore_ffi T nodecore_start\nnodecore_ffi T nodecore_stop\n")


def test_lib_short_symbol_list_is_an_input_error(tmp_path, monkeypatch):
    build, args = win_tree(tmp_path)
    table = {"nodecore_ffi.lib": {("T", "nodecore_start")},          # nodecore_stop missing
             "dinero_wallet_ffi.lib": {("T", "dinero_wallet_create")}}
    monkeypatch.setattr(ifz, "symbols", lambda path, nm=None: table[os.path.basename(path)])
    with pytest.raises(ifz.InputError, match="nodecore_stop"):
        ifz.symbol_text(str(build), args)


def test_lib_empty_symbol_list_is_an_input_error(tmp_path, monkeypatch):
    build, args = win_tree(tmp_path)
    capture_nm(monkeypatch, "nodecore_ffi.cpp.obj:\n")
    with pytest.raises(ifz.InputError):
        ifz.symbol_text(str(build), args)


def test_lib_header_with_no_declarations_is_an_input_error(tmp_path, monkeypatch):
    build, args = win_tree(tmp_path)
    (tmp_path / "base" / "nodecore_ffi.h").write_text("/* nothing */\n")
    monkeypatch.setattr(ifz, "symbols", lambda path, nm=None: {("T", "nodecore_start")})
    with pytest.raises(ifz.InputError, match="no C functions parsed"):
        ifz.symbol_text(str(build), args)


def test_committed_headers_declarations_match_macos_c_exports():
    # The export check's expected list (headers) equals the committed macOS
    # baseline's defined C functions, so it is neither too short nor too long.
    base = os.path.join(os.path.dirname(__file__), "..", "..", "..", "..",
                        "tests", "separation", "interface_baseline")
    for cfg in ("off", "on"):
        rows = [l.split() for l in open(os.path.join(base, "symbols_macos_%s.txt" % cfg))]
        for lib, header in ifz.LIB_HEADER.items():
            exported = {r[2].lstrip("_") for r in rows if r[0] == lib and r[1] == "T"}
            declared = ifz.declared_c_functions(open(os.path.join(base, header)).read())
            assert declared == exported, (cfg, lib)
