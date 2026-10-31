#!/usr/bin/env python3
import pathlib
import tempfile
import unittest
from unittest.mock import patch
from check_macos_desktop_payload import APP_TOOLS, check


class MacPayload(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.stage = pathlib.Path(temp.name)
        for tool in APP_TOOLS:
            self.write(self.stage / 'dinero-qt.app/Contents/MacOS' / tool)
        self.wallet_cli = self.stage / 'dinero-wallet-cli'
        self.write(self.wallet_cli)
        mock = patch('check_macos_desktop_payload.subprocess.check_output', return_value='x86_64\n')
        self.lipo = mock.start()
        self.addCleanup(mock.stop)

    def write(self, path):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('test fixture')
        path.chmod(0o755)

    def test_sidecar_wallet_cli_is_valid_without_app_copy(self):
        self.assertFalse((self.stage / 'dinero-qt.app/Contents/MacOS/dinero-wallet-cli').exists())
        check(self.stage, 'x86_64')
        self.assertEqual(self.lipo.call_count, len(APP_TOOLS) + 1)

    def test_missing_nonexecutable_or_wrong_arch_refused(self):
        self.wallet_cli.chmod(0o644)
        with self.assertRaisesRegex(ValueError, 'missing executable'):
            check(self.stage, 'x86_64')
        self.wallet_cli.unlink()
        with self.assertRaisesRegex(ValueError, 'missing executable'):
            check(self.stage, 'x86_64')
        self.write(self.wallet_cli)
        self.lipo.return_value = 'arm64\n'
        with self.assertRaisesRegex(ValueError, 'expected native'):
            check(self.stage, 'x86_64')

    def test_missing_bundled_miner_refused(self):
        (self.stage / 'dinero-qt.app/Contents/MacOS/dinero-sv2-gpu-miner').unlink()
        with self.assertRaisesRegex(ValueError, 'dinero-sv2-gpu-miner'):
            check(self.stage, 'x86_64')


if __name__ == '__main__':
    unittest.main()
