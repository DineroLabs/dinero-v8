"""Unit contracts, not SDK or device qualification."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('package', ROOT/'scripts/mobile/package_nodecore_archive.py')
module=importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
elf_spec=importlib.util.spec_from_file_location('android',ROOT/'scripts/mobile/package_nodecore_android.py')
# The CLI scripts share a directory rather than installing a Python package.
import sys
sys.path.insert(0,str(ROOT/'scripts/mobile'))
android=importlib.util.module_from_spec(elf_spec)
elf_spec.loader.exec_module(android)


class Packaging(unittest.TestCase):
    def test_exact_link_command_and_quoted_paths(self):
        tokens=module.link_tokens('clang++ -c smoke.cpp -o CMakeFiles/x/nodecore_link_smoke.cpp.o\n'
                                 ': && clang++ CMakeFiles/x/nodecore_link_smoke.cpp.o -o smoke "with space/a.a" a.a -framework Security && :')
        self.assertIn('with space/a.a', tokens)
        self.assertEqual(module.relink(tokens,Path('/sdk/merged.a'),Path('/sdk/smoke')),
                         ['clang++','CMakeFiles/x/nodecore_link_smoke.cpp.o','-o','/sdk/smoke','/sdk/merged.a','-framework','Security'])

    def test_missing_ambiguous_and_response_file_refuse(self):
        command='clang++ nodecore_link_smoke.cpp.o -o smoke a.a'
        for value in ('',command+'\n'+command, command+' @other.rsp'):
            with self.subTest(value=value), self.assertRaises(ValueError):
                module.link_tokens(value)

    def test_missing_orchard_graph_cannot_package(self):
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp);(path/'old.a').write_bytes(b'not an archive')
            with self.assertRaisesRegex(ValueError,'snapshot store'):
                module.archives(['old.a'],path)

    def test_android_architecture_and_page_alignment(self):
        import struct
        header=bytearray(120);header[:6]=b'\x7fELF\x02\x01'
        struct.pack_into('<HH',header,16,3,183)
        struct.pack_into('<Q',header,32,64)
        struct.pack_into('<HH',header,54,56,1)
        struct.pack_into('<I',header,64,1)
        struct.pack_into('<Q',header,112,16384)
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp)/'test.so';path.write_bytes(header)
            android.check_elf(path,183)
            with self.assertRaisesRegex(ValueError,'architecture'):
                android.check_elf(path,62)
            struct.pack_into('<Q',header,112,4096);path.write_bytes(header)
            with self.assertRaisesRegex(ValueError,'16 KiB'):
                android.check_elf(path,183)
            path.write_bytes(b'not an ELF')
            with self.assertRaises(ValueError):
                android.check_elf(path,183)


if __name__=='__main__':
    unittest.main()
