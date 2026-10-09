"""CMake bridge contracts only; no Rust/C++ compiler or Windows qualification."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2] / 'rust/orchard_backend'
CMAKE = 'cmake'

class PlatformContract(unittest.TestCase):
    def run_script(self, text):
        with tempfile.TemporaryDirectory(prefix='orchard-cmake-contract-') as temp:
            path = Path(temp) / 'case.cmake'
            path.write_text('cmake_minimum_required(VERSION 3.20)\n' + text)
            return subprocess.run([CMAKE, '-P', str(path)], text=True,
                                  capture_output=True, timeout=15)

    def platform(self, settings, success=True, diagnostic=None):
        defaults = dict(WIN32='TRUE', CMAKE_HOST_WIN32='TRUE', MSVC='TRUE',
                        CMAKE_CXX_COMPILER_ID='MSVC', MSVC_CXX_ARCHITECTURE_ID='x64',
                        CMAKE_SIZEOF_VOID_P='8', CMAKE_SYSTEM_PROCESSOR='AMD64',
                        DINERO_ORCHARD_WINDOWS_QUALIFICATION='TRUE')
        defaults.update(settings)
        source = '\n'.join(f'set({k} "{v}")' for k,v in defaults.items())
        source += f'\ninclude("{(ROOT / "NativeRustPlatform.cmake").as_posix()}")\n'
        source += 'dinero_orchard_native_platform(target archive)\nmessage("RESULT=${target}|${archive}")\n'
        result = self.run_script(source)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        if diagnostic: self.assertIn(diagnostic, result.stderr)
        return result.stderr

    def test_msvc_native_release_contract(self):
        self.assertIn('RESULT=x86_64-pc-windows-msvc|dinero_orchard_backend.lib', self.platform({}))

    def test_windows_release_guard_remains(self):
        self.platform({'DINERO_ORCHARD_WINDOWS_QUALIFICATION':'FALSE'}, False, 'Windows release is not qualified')

    def test_wrong_compiler_architecture_or_host_refuses(self):
        for settings in ({'MSVC':'FALSE'}, {'CMAKE_CXX_COMPILER_ID':'Clang'},
                         {'CMAKE_SIZEOF_VOID_P':'4'}, {'CMAKE_SYSTEM_PROCESSOR':'ARM64'},
                         {'MSVC_CXX_ARCHITECTURE_ID':'ARM64'}, {'CMAKE_HOST_WIN32':'FALSE'}):
            with self.subTest(settings=settings): self.platform(settings, False, 'native x64 MSVC')

    def test_cross_and_mobile_remain_refused(self):
        for settings in ({'CMAKE_CROSSCOMPILING':'TRUE'}, {'ANDROID':'TRUE'}, {'CMAKE_SYSTEM_NAME':'iOS'}):
            with self.subTest(settings=settings): self.platform(settings, False, 'cross/mobile')

    def test_static_or_unknown_runtime_refuses(self):
        for runtime in ('MultiThreaded','MultiThreadedDebugDLL','Unknown'):
            with self.subTest(runtime=runtime): self.platform({'CMAKE_MSVC_RUNTIME_LIBRARY':runtime}, False, 'dynamic MSVC runtime')

    def test_dynamic_runtime_variants(self):
        for runtime in ('MultiThreadedDLL','MultiThreaded$<$<CONFIG:Debug>:Debug>DLL'):
            with self.subTest(runtime=runtime): self.platform({'CMAKE_MSVC_RUNTIME_LIBRARY':runtime})

    def test_native_unix_archive_unchanged(self):
        for apple in ('FALSE','TRUE'):
            with self.subTest(apple=apple):
                self.assertIn('RESULT=|libdinero_orchard_backend.a', self.platform({'WIN32':'FALSE','APPLE':apple}))

    def test_foreign_macos_architecture_refuses(self):
        self.platform({'WIN32':'FALSE','APPLE':'TRUE','CMAKE_OSX_ARCHITECTURES':'x86_64','CMAKE_HOST_SYSTEM_PROCESSOR':'arm64'}, False, 'native macOS architecture')

    def libraries(self, report, success=True):
        result = self.run_script(f'include("{(ROOT / "MsvcRustNativeLibraries.cmake").as_posix()}")\n'
            f'dinero_orchard_msvc_native_libraries([==[{report}]==] response)\nmessage("RESULT=${{response}}END")\n')
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return result.stderr

    def test_native_libraries_preserve_order_and_duplicates(self):
        self.assertIn('RESULT=kernel32.lib\nuserenv.lib\nkernel32.lib\nEND',
                      self.libraries('note: native-static-libs: kernel32.lib userenv.lib kernel32.lib\n'))

    def test_actual_rust_release_runtime_directive(self):
        report = 'note: native-static-libs: bcrypt.lib advapi32.lib kernel32.lib ntdll.lib userenv.lib ws2_32.lib dbghelp.lib /defaultlib:msvcrt\n'
        expected = 'RESULT=bcrypt.lib\nadvapi32.lib\nkernel32.lib\nntdll.lib\nuserenv.lib\nws2_32.lib\ndbghelp.lib\n/defaultlib:msvcrt\nEND'
        self.assertIn(expected, self.libraries(report))
        self.assertIn('RESULT=/defaultlib:msvcrt\nkernel32.lib\n/defaultlib:msvcrt\nEND',
                      self.libraries('native-static-libs: /defaultlib:msvcrt kernel32.lib /defaultlib:msvcrt'))

    def test_other_runtime_directives_and_response_options_refuse(self):
        for token in ('/defaultlib:libcmt', '/defaultlib:msvcrtd', '/defaultlib:msvcrt.dll',
                      '/defaultlib:other', '/defaultlib:../msvcrt', '/DEFAULTLIB:msvcrt',
                      '/nodefaultlib', '/defaultlib:msvcrt;other.lib', '/defaultlib:msvcrt&bad'):
            with self.subTest(token=token): self.libraries('native-static-libs: '+token, False)

    def test_missing_empty_duplicate_report_refuses(self):
        for report in ('no report','note: native-static-libs: ',
                       'native-static-libs: a.lib\nnative-static-libs: a.lib'):
            with self.subTest(report=report): self.libraries(report, False)

    def test_native_report_rejects_linker_options_paths_and_shell_tokens(self):
        for token in ('/OUT:elsewhere','@other.rsp','../other.lib','C:/other.lib','a.lib;bad.lib','a.lib&evil','-lm','"a.lib"'):
            with self.subTest(token=token): self.libraries('native-static-libs: '+token, False)

    def test_build_rejects_debug_before_cargo(self):
        with tempfile.TemporaryDirectory(prefix='orchard-crt-contract-') as temp:
            values=dict(CARGO='NONEXISTENT_MUST_NOT_RUN',RUST_TOOLCHAIN='1.91.1',MANIFEST='unused',
                        TARGET_DIR=temp,ARCHIVE=temp+'/absent.lib',LINK_RESPONSE=temp+'/absent.rsp',BUILD_CONFIG='Debug')
            command=[CMAKE]+[f'-D{k}={v}' for k,v in values.items()]+['-P',str(ROOT/'BuildMsvcRust.cmake')]
            result=subprocess.run(command,capture_output=True,text=True,timeout=15)
            self.assertNotEqual(result.returncode,0)
            self.assertIn('release CRT configuration',result.stderr)
            self.assertFalse(Path(values['LINK_RESPONSE']).exists())

    def test_conflicting_runtime_flags_refuse(self):
        for flag in ('/MT', '/MTd', '/MDd', '-MT'):
            with self.subTest(flag=flag): self.platform({'CMAKE_CXX_FLAGS_RELEASE':flag}, False, 'runtime compiler flag')

    def wrapper(self, temp, mode='ok', extra_env=None):
        import json, sys
        root = Path(temp)
        archive = root / 'candidate.lib'
        response = root / 'candidate.rsp'
        # Python consumes the pinned toolchain argument as this inert script.
        # This needs no POSIX shebang, shell, or Windows file association.
        fake = root / '+1.91.1'
        fake.write_text('import os,sys,json\nfrom pathlib import Path\n' +
            'root=Path(__file__).parent\n' +
            '(root/"invocation.json").write_text(json.dumps({"args":[Path(__file__).name]+sys.argv[1:],"flags":os.environ.get("CARGO_TARGET_X86_64_PC_WINDOWS_MSVC_RUSTFLAGS"),"threads":os.environ.get("RAYON_NUM_THREADS")}))\n' +
            'mode=os.environ["ORCHARD_INERT_MODE"]\n' +
            'if mode != "absent": (root/"candidate.lib").write_bytes(b"INERT-NOT-A-REAL-LIBRARY")\n' +
            'if mode != "missing-report": print("note: native-static-libs: kernel32.lib userenv.lib kernel32.lib",file=sys.stderr)\n' +
            'if mode == "duplicate": print("note: native-static-libs: userenv.lib",file=sys.stderr)\n' +
            'sys.exit(7 if mode == "failed" else 0)\n')
        values=dict(CARGO=sys.executable,RUST_TOOLCHAIN='1.91.1',MANIFEST=str(root/'Cargo.toml'),
                    TARGET_DIR=str(root/'target'),ARCHIVE=str(archive),LINK_RESPONSE=str(response),BUILD_CONFIG='Release')
        env=dict(os.environ,ORCHARD_INERT_MODE=mode)
        env.update(extra_env or {})
        result=subprocess.run([CMAKE]+[f'-D{k}={v}' for k,v in values.items()]+['-P',str(ROOT/'BuildMsvcRust.cmake')],
                              env=env,cwd=root,text=True,capture_output=True,timeout=15)
        return result, response, root/'invocation.json'

    def test_wrapper_binds_actual_command_and_publishes_report(self):
        import json
        with tempfile.TemporaryDirectory(prefix='orchard-inert-cargo-') as temp:
            result,response,record=self.wrapper(temp)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertEqual(response.read_text(),'kernel32.lib\nuserenv.lib\nkernel32.lib\n')
            call=json.loads(record.read_text())
            self.assertEqual(call['args'],['+1.91.1','rustc','--locked','--release','--jobs','2','--lib',
                '--target','x86_64-pc-windows-msvc','--manifest-path',str(Path(temp)/'Cargo.toml'),
                '--target-dir',str(Path(temp)/'target'),'--','--print','native-static-libs'])
            self.assertEqual(call['flags'],'-C target-feature=-crt-static')
            self.assertEqual(call['threads'],'2')

    def test_wrapper_failure_preserves_previous_response(self):
        for mode in ('failed','absent','missing-report','duplicate'):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory(prefix='orchard-inert-cargo-') as temp:
                response=Path(temp)/'candidate.rsp';response.write_text('PREVIOUS\n')
                result,_,_=self.wrapper(temp,mode)
                self.assertNotEqual(result.returncode,0)
                self.assertEqual(response.read_text(),'PREVIOUS\n')
                self.assertFalse(Path(str(response)+'.tmp').exists())

    def test_wrapper_refuses_uncontrolled_rust_before_launch(self):
        for name in ('RUSTFLAGS','CARGO_ENCODED_RUSTFLAGS','RUSTC','RUSTC_WRAPPER','CARGO_BUILD_RUSTC'):
            with self.subTest(name=name), tempfile.TemporaryDirectory(prefix='orchard-inert-cargo-') as temp:
                result,response,record=self.wrapper(temp,extra_env={name:'uncontrolled'})
                self.assertNotEqual(result.returncode,0)
                self.assertIn('Uncontrolled '+name,result.stderr)
                self.assertFalse(record.exists())
                self.assertFalse(response.exists())

if __name__ == '__main__': unittest.main(verbosity=2)
