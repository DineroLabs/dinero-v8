#!/usr/bin/env python3
import pathlib
import platform
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from check_macos_minimum import audit, minimum

BUILD = 'cmd LC_BUILD_VERSION\nplatform 1\nminos 13.0\nsdk 26.0\ntool LD\nversion 1234.1\ncmd LC_UUID\n'
LEGACY = 'cmd LC_VERSION_MIN_MACOSX\nversion 10.15\nsdk 14.0\n'


class MacMinimum(unittest.TestCase):
    def test_load_commands_not_sdk_or_linker_version(self):
        self.assertEqual(minimum(BUILD), (13, 0, 0))
        self.assertEqual(minimum(LEGACY), (10, 15, 0))

    def test_missing_duplicate_and_non_macos_refused(self):
        for text in ['', BUILD + LEGACY, BUILD.replace('platform 1', 'platform 2')]:
            with self.subTest(text=text), self.assertRaises(ValueError):
                minimum(text)

    def test_real_audit_refuses_newer_runtime_in_nested_bundle(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            helper = root / 'Contents/Frameworks/New.framework/New'
            helper.parent.mkdir(parents=True)
            helper.write_bytes(bytes.fromhex('cffaedfe') + bytes(20))
            with patch('check_macos_minimum.subprocess.run') as arch_check, patch('check_macos_minimum.subprocess.check_output', return_value=BUILD) as tool:
                audit(root, 'x86_64', '13.0')
                arch_check.assert_called_once_with(['lipo', str(helper), '-verify_arch', 'x86_64'], check=True)
                self.assertEqual(tool.call_args.args[0], ['otool', '-arch', 'x86_64', '-l', str(helper)])
            with patch('check_macos_minimum.subprocess.run'), patch('check_macos_minimum.subprocess.check_output', return_value=BUILD.replace('minos 13.0', 'minos 14.0')):
                with self.assertRaisesRegex(ValueError, 'above 13.0'):
                    audit(root, 'x86_64', '13.0')

    @unittest.skipUnless(sys.platform == 'darwin', 'requires Apple lipo/otool/clang')
    def test_actual_apple_tools_on_native_and_wrong_architecture(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            source = root / 'main.c'
            source.write_text('int main(void) { return 0; }\n')
            subprocess.run(['clang', '-mmacosx-version-min=13.0', str(source), '-o', str(root / 'tool')], check=True)
            native = platform.machine()
            audit(root, native, '13.0')
            other = 'x86_64' if native == 'arm64' else 'arm64'
            with self.assertRaises(subprocess.CalledProcessError):
                audit(root, other, '13.0')

    def test_empty_package_refused(self):
        with tempfile.TemporaryDirectory() as temp, self.assertRaisesRegex(ValueError, 'too few Mach-O'):
            audit(pathlib.Path(temp), 'arm64', '13.0')


if __name__ == '__main__':
    unittest.main()
