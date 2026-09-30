#!/usr/bin/env python3
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('off_audit', Path(__file__).resolve().parents[1] / 'check_orchard_backend_off.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class BackendOffAudit(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / 'source'
        self.build = self.root / 'build'
        (self.build / 'CMakeFiles').mkdir(parents=True)
        (self.build / 'CMakeCache.txt').write_text('DINERO_BUILD_ORCHARD_BACKEND:BOOL=OFF\n')
        self.entries = [{'directory': str(self.build), 'file': str(self.source / n),
                         'arguments': ['c++', '-c', str(self.source / n)]} for n in (
                             'src/daemon/services/chainstate_service.cpp',
                             'tests/daemon/test_assumeutxo_replay.cpp')]
        (self.build / 'build.ninja').write_text('build dinerod: CXX_EXECUTABLE_LINKER objects.o\n')

    def run_audit(self):
        (self.build / 'compile_commands.json').write_text(json.dumps(self.entries))
        return module.audit(self.build, self.source)

    def test_exact_replay_header_is_allowed_in_both_command_formats(self):
        token = '-I' + str(self.source / 'rust/orchard_backend/include')
        self.entries[1]['arguments'].append(token)
        self.assertEqual(self.run_audit()['backend'], 'OFF')
        import shlex
        self.entries[1]['command'] = shlex.join(self.entries[1].pop('arguments'))
        self.assertEqual(self.run_audit()['required_sources'], 2)

    def test_macros_and_other_backend_dependencies_refuse(self):
        token = '-I' + str(self.source / 'rust/orchard_backend/include')
        for index, value in [(0, token), (1, token + '/other'),
                             (1, '-DDINERO_TEST_ORCHARD_ORIGIN=1'),
                             (0, '-DDINERO_HAS_ORCHARD_RUNTIME_READER=1')]:
            with self.subTest(value=value):
                self.entries[index]['arguments'].append(value)
                with self.assertRaises(module.AuditError): self.run_audit()
                self.entries[index]['arguments'].pop()

    def test_backend_compilation_and_link_targets_refuse(self):
        self.entries.append({'directory': str(self.build), 'file': str(self.source / 'rust/orchard_backend/src/backend.cpp'), 'arguments': ['c++']})
        with self.assertRaises(module.AuditError): self.run_audit()
        self.entries.pop()
        for graph in ['build dinerod: LINK libdinero_orchard_backend.a',
                      'build dinero_orchard_rust_build: phony',
                      'build libdinero_orchard.so: LINK obj.o',
                      'LINK_LIBRARIES = -ldinero_orchard_backend']:
            with self.subTest(graph=graph):
                (self.build / 'build.ninja').write_text(graph)
                with self.assertRaises(module.AuditError): self.run_audit()

    def test_make_links_and_target_inventory_are_checked(self):
        (self.build / 'build.ninja').unlink()
        (self.build / 'CMakeFiles/TargetDirectories.txt').write_text(str(self.build / 'CMakeFiles/dinerod.dir'))
        link = self.build / 'CMakeFiles/link.txt'
        link.write_text('c++ obj.o -o dinerod')
        self.assertEqual(self.run_audit()['graph_files'], 2)
        link.write_text('c++ obj.o libdinero_orchard_backend.a -o dinerod')
        with self.assertRaises(module.AuditError): self.run_audit()
        link.write_text('c++ obj.o -o dinerod')
        (self.build / 'CMakeFiles/TargetDirectories.txt').write_text(str(self.build / 'CMakeFiles/dinero_orchard.dir'))
        with self.assertRaises(module.AuditError): self.run_audit()

    def test_missing_graph_or_required_source_refuses(self):
        self.entries.pop()
        with self.assertRaises(module.AuditError): self.run_audit()
        self.setUp()
        (self.build / 'build.ninja').unlink()
        with self.assertRaises(module.AuditError): self.run_audit()

    def test_backend_on_configuration_refuses(self):
        (self.build / 'CMakeCache.txt').write_text('DINERO_BUILD_ORCHARD_BACKEND:BOOL=ON\n')
        with self.assertRaises(module.AuditError): self.run_audit()


if __name__ == '__main__':
    unittest.main()
