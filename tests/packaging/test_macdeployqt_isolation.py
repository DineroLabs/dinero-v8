#!/usr/bin/env python3
"""Offline checks for deployment isolation; no node, GUI, socket or compiler."""
import hashlib
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

MODULE = Path(__file__).with_name('run_macdeployqt_sanitized.py')
if not MODULE.exists():
    MODULE = Path(__file__).resolve().parents[2] / 'qt/cmake/run_macdeployqt_sanitized.py'
spec = importlib.util.spec_from_file_location('deployment', MODULE)
deployment = importlib.util.module_from_spec(spec)
spec.loader.exec_module(deployment)


def snapshot(root):
    return {str(p.relative_to(root)): (p.lstat().st_mode, p.lstat().st_mtime_ns,
            os.readlink(p) if p.is_symlink() else hashlib.sha256(p.read_bytes()).hexdigest())
            for p in root.rglob('*') if p.is_file() or p.is_symlink()}


class DeploymentIsolation(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='qt fixture, ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.toolkit = self.root / 'custom toolkit, with spaces'
        self.bin = self.toolkit / 'bin'
        self.bin.mkdir(parents=True)
        self.tool = self.bin / 'macdeployqt'
        self.tool.write_bytes(b'fixture deployment executable')
        self.tool.chmod(0o755)
        (self.bin / 'qtpaths').write_bytes(b'fixture query executable')
        self.plugins = self.toolkit / 'plugins'
        for name in ['platforms/libqcocoa.dylib', 'imageformats/libqjpeg.dylib',
                     'imageformats/libqpdf.dylib', 'imageformats/libqsvg.dylib',
                     'iconengines/libqsvgicon.dylib',
                     'platforminputcontexts/libqtvirtualkeyboardplugin.dylib',
                     'iconengines.dineroqt-hidden/preserved.dylib']:
            p = self.plugins / name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(name.encode())
        (self.plugins / 'imageformats/libqalias.dylib').symlink_to('libqjpeg.dylib')
        self.paths = {key: str(self.toolkit / value) for key, value in deployment.QT_PATH_KEYS.items()}
        self.paths.update(QT_INSTALL_PREFIX=str(self.toolkit), QT_INSTALL_BINS=str(self.bin),
                          QT_INSTALL_PLUGINS=str(self.plugins))
        Path(self.paths['QT_INSTALL_LIBS']).mkdir()
        self.app = self.root / 'Application, with spaces.app'
        self.app.mkdir()
        self.before = snapshot(self.toolkit)
        self.isolated = None
        self.invoked = False
        self.supports_sign_option = True
        self.exitcode = 0
        self.bad_query = False
        self.bad_help = False

    def fake_query(self, qtpaths):
        if qtpaths == self.bin / 'qtpaths':
            return dict(self.paths)
        self.isolated = qtpaths.parent.parent
        self.assertEqual(snapshot(self.toolkit), self.before)
        self.assertEqual((self.isolated/'bin/macdeployqt').read_bytes(), self.tool.read_bytes())
        self.assertFalse((self.isolated/'bin/macdeployqt').is_symlink())
        self.assertEqual(os.readlink(self.isolated/'lib'), self.paths['QT_INSTALL_LIBS'])
        config = (qtpaths.parent/'qt.conf').read_text()
        for key, value in self.paths.items():
            if key != 'QT_INSTALL_PLUGINS':
                self.assertIn(f'{deployment.QT_PATH_KEYS[key]} = {deployment.ini_path(value)}\n', config)
        expected = dict(self.paths, QT_INSTALL_PLUGINS=str(self.isolated/'plugins'))
        self.assertIn(f'Plugins = {deployment.ini_path(expected["QT_INSTALL_PLUGINS"])}\n', config)
        actual = {str(p.relative_to(self.isolated/'plugins')) for p in (self.isolated/'plugins').rglob('*') if p.is_file()}
        self.assertEqual(actual, {'platforms/libqcocoa.dylib', 'imageformats/libqjpeg.dylib', 'imageformats/libqalias.dylib'})
        self.assertFalse((self.isolated/'plugins/imageformats/libqalias.dylib').is_symlink())
        return self.paths if self.bad_query else expected

    def fake_run(self, args, **kwargs):
        self.assertEqual(snapshot(self.toolkit), self.before)
        self.assertEqual(args[0], str(self.isolated/'bin/macdeployqt'))
        if args[1] == '-help':
            return subprocess.CompletedProcess(args, 1, 'tool failed' if self.bad_help else 'Usage: macdeployqt -always-overwrite ' + ('-no-codesign' if self.supports_sign_option else ''), '')
        self.invoked = True
        self.assertEqual(args[1:3], [str(self.app), '-always-overwrite'])
        self.assertEqual('-no-codesign' in args, self.supports_sign_option)
        return subprocess.CompletedProcess(args, self.exitcode)

    def execute(self):
        try:
            with patch.object(deployment, 'query_paths', side_effect=self.fake_query), patch.object(deployment.subprocess, 'run', side_effect=self.fake_run):
                return deployment.deploy(self.tool, self.app)
        finally:
            self.assertEqual(snapshot(self.toolkit), self.before)
            if self.isolated:
                self.assertFalse(self.isolated.exists())

    def test_success_preserves_toolkit_and_filters_private_copy(self):
        self.assertEqual(self.execute(), 0)
        self.assertTrue(self.invoked)

    def test_failed_deploy_propagates_status_and_cleans_private_copy(self):
        self.exitcode = 37
        self.assertEqual(self.execute(), 37)
        self.assertTrue(self.invoked)

    def test_older_tool_without_no_codesign_option(self):
        self.supports_sign_option = False
        self.assertEqual(self.execute(), 0)

    def test_wrong_resolved_configuration_refuses_before_deploy(self):
        self.bad_query = True
        with self.assertRaisesRegex(ValueError, 'configuration'):
            self.execute()
        self.assertFalse(self.invoked)

    def test_failed_help_refuses_before_deploy(self):
        self.bad_help = True
        with self.assertRaisesRegex(ValueError, 'inspect macdeployqt'):
            self.execute()
        self.assertFalse(self.invoked)

    def test_copy_failure_leaves_originals_and_no_deploy(self):
        with patch.object(deployment.shutil, 'copytree', side_effect=OSError('fixture copy refused')):
            with self.assertRaisesRegex(OSError, 'copy refused'):
                self.execute()
        self.assertFalse(self.invoked)

    def test_query_requires_complete_unique_absolute_paths(self):
        valid = ''.join(f'{key}:{value}\n' for key, value in self.paths.items())
        for output in [valid.replace('QT_INSTALL_PLUGINS:', 'UNRELATED:'), valid+'QT_INSTALL_LIBS:/duplicate\n', valid.replace(self.paths['QT_INSTALL_PREFIX'], 'relative', 1)]:
            with self.subTest(output=output), patch.object(deployment.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, output, '')):
                with self.assertRaises(ValueError):
                    deployment.query_paths(self.bin/'qtpaths')
        with patch.object(deployment.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, valid, '')):
            self.assertEqual(deployment.query_paths(self.bin/'qtpaths'), self.paths)

    def test_ini_path_quotes_commas_and_rejects_control_characters(self):
        self.assertEqual(deployment.ini_path('/a, b/"c"/d\\e'), '"/a, b/\\"c\\"/d\\\\e"')
        for value in ['/a\nb', '/a\rb', '/a\x00b', '/a\tb']:
            with self.assertRaises(ValueError):
                deployment.ini_path(value)


if __name__ == '__main__':
    unittest.main()
