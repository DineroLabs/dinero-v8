#!/usr/bin/env python3
import hashlib
from pathlib import Path
import tempfile
import unittest
import zipfile
from package_signed_operator import TOOLS, assemble


class OperatorRuntimeTest(unittest.TestCase):
    def test_runtime_closure_and_signed_bytes(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw); stage = root / 'stage'; stage.mkdir()
            license_file = root / 'LICENSE'; license_file.write_bytes(b'license\n')
            payload = {name: ('signed-' + name).encode() for name in TOOLS}
            for name, data in payload.items():
                (stage / name).write_bytes(data)
            output = root / 'operator.zip'
            with self.assertRaisesRegex(ValueError, 'NVRTC runtime'):
                assemble(stage, license_file, output, '8.2.0')
            self.assertFalse(output.exists())
            for name in ('nvrtc64_120_0.dll', 'nvrtc-builtins64_122.dll'):
                payload[name] = ('signed-' + name).encode()
                (stage / name).write_bytes(payload[name])
            assemble(stage, license_file, output, '8.2.0')
            with zipfile.ZipFile(output) as archive:
                self.assertIsNone(archive.testzip())
                self.assertEqual(len(archive.namelist()), 14)
                for name, data in payload.items():
                    self.assertEqual(archive.read('bin/' + name), data)
                sums = archive.read('SHA256SUMS.txt').decode().splitlines()
                self.assertEqual(len(sums), 13)
                for row in sums:
                    digest, name = row.split('  ', 1)
                    self.assertEqual(hashlib.sha256(archive.read(name)).hexdigest(), digest)

    def test_duplicate_runtime_refuses_before_output(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            for name in TOOLS + ('nvrtc64_120_0.dll', 'nvrtc64_130_0.dll',
                                 'nvrtc-builtins64_122.dll', 'LICENSE'):
                (root / name).write_bytes(b'fixture')
            with self.assertRaisesRegex(ValueError, 'NVRTC runtime'):
                assemble(root, root / 'LICENSE', root / 'output.zip', '8.2.0')
            self.assertFalse((root / 'output.zip').exists())


if __name__ == '__main__':
    unittest.main()
