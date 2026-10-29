"""Unit contracts, not SDK or device qualification."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
import copy
import json
import os
import shutil
import subprocess

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
import sdk_consensus_manifest as consensus


class Packaging(unittest.TestCase):
    def test_reviewed_defaults_are_network_specific_not_globally_dormant(self):
        declaration=consensus.reviewed_defaults(ROOT)
        self.assertEqual(declaration['dnorst02_activation_height'],dict(mainnet=150000,testnet=4294967295,regtest=4294967295))
        self.assertIn('NOT runtime attestation',declaration['evidence'])

    def test_source_change_requires_explicit_declaration_review(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp)
            for name in consensus.REVIEWED:
                (root/name).parent.mkdir(parents=True,exist_ok=True)
                shutil.copy2(ROOT/name,root/name)
            self.assertEqual(consensus.reviewed_defaults(root),consensus.reviewed_defaults(ROOT))
            path=root/'src/consensus/chainparams_impl.cpp';path.write_bytes(path.read_bytes()+b'\n')
            with self.assertRaisesRegex(ValueError,'unreviewed chainparams'):
                consensus.reviewed_defaults(root)

    def test_composition_preserves_input_source_instead_of_current_head(self):
        receipt=consensus.source_receipt(ROOT);receipt.update(source_base='a'*40,source_dirty=False)
        combined=consensus.compose_receipts([receipt,copy.deepcopy(receipt)],ROOT)
        self.assertEqual(combined['source_base'],'a'*40)
        self.assertEqual(combined['source_consensus_declaration'],consensus.reviewed_defaults(ROOT))
        for field,value,error in [('source_base','b'*40,'different source'),('source_dirty',True,'clean'),
                                  ('ffi_header_sha256','0'*64,'header identity'),
                                  ('source_consensus_declaration',{'dnorst02':'dormant'},'consensus declaration')]:
            changed=copy.deepcopy(receipt);changed[field]=value
            with self.subTest(field=field),self.assertRaisesRegex(ValueError,error):
                consensus.compose_receipts([receipt,changed],ROOT)
        with self.assertRaisesRegex(ValueError,'consensus declaration'):
            consensus.compose_receipts([dict(qualification_only=True,dnorst02='dormant')],ROOT)

    def test_xcframework_ships_orchard_wallet_header(self):
        spec=importlib.util.spec_from_file_location('xcframework',ROOT/'scripts/mobile/package_nodecore_xcframework.py')
        xcframework=importlib.util.module_from_spec(spec);spec.loader.exec_module(xcframework)
        with tempfile.TemporaryDirectory() as temp:
            headers=Path(temp)/'Headers';headers.mkdir()
            digests=xcframework.stage_headers(headers)
            sources={'nodecore_ffi.h':ROOT/'include/nodecore/nodecore_ffi.h',
                     'orchard_backend_ffi.h':ROOT/'rust/orchard_backend/include/orchard_backend_ffi.h'}
            self.assertEqual(sorted(p.name for p in headers.iterdir()),sorted(sources))
            for name,source in sources.items():
                self.assertEqual((headers/name).read_bytes(),source.read_bytes())
                self.assertEqual(digests[name],module.digest(source))
            # Swift consumers include both through a bridging header: each must stand alone as C.
            for name in sources:
                subprocess.run(['cc','-fsyntax-only','-x','c','-I',str(headers),str(headers/name)],check=True)

    def test_sdk_slices_never_carry_gpu_mining(self):
        # macOS host builds auto-detect OpenCL; a wallet SDK must not export the
        # mining backend (it leaves _cl* symbols undefined for app links).
        source=(ROOT/'scripts/mobile/build_nodecore_slice.py').read_text()
        self.assertIn("'-DENABLE_GPU_MINING=OFF'",source)

    def test_all_packaging_entrypoints_use_the_reviewed_receipt(self):
        for name in ('build_nodecore_slice.py','package_nodecore_archive.py','package_nodecore_android.py','package_nodecore_xcframework.py'):
            source=(ROOT/'scripts/mobile'/name).read_text()
            self.assertNotIn("dnorst02='dormant'",source)
            self.assertIn('sdk_consensus_manifest',source)

    if os.environ.get('NODECORE_LINK_SMOKE'):
        def test_declaration_matches_actual_compiled_network_parameters(self):
            actual=json.loads(subprocess.check_output([os.environ['NODECORE_LINK_SMOKE'],'--consensus-json'],text=True))
            self.assertEqual(actual,consensus.reviewed_defaults(ROOT)['dnorst02_activation_height'])

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
