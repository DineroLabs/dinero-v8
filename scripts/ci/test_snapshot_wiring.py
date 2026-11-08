"""Cross-platform package defaults must agree with the pinned input catalog."""
import json
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SnapshotWiring(unittest.TestCase):
    def setUp(self):
        catalog = json.loads((ROOT / 'packaging/snapshot/desktop-inputs-candidate.json').read_text())
        self.names = [catalog['primary']['filename'], *(e['filename'] for e in catalog['fallbacks'])]

    def test_gui_selection_and_windows_derivation_cover_all_three(self):
        source = (ROOT / 'qt/src/main.cpp').read_text()
        builder = (ROOT / 'packaging/windows/build-installer.ps1').read_text()
        for name, constant in zip(self.names, ['Primary', 'Fallback', 'LegacyFallback']):
            self.assertEqual(re.search(r'kBundledSnapshot' + constant + r'\[\]\s*=\s*"([^"]+)"', source)[1], name)
            self.assertIn('kBundledSnapshot' + constant + r'\[\]', builder)
        self.assertIn('SelectBundledSnapshots({', source)
        self.assertIn("fallbacks.join(';')", source)
        self.assertIn("throw 'Required older exact-lifecycle snapshot fallback missing.'", builder)

    def test_linux_stages_all_three(self):
        source = (ROOT / 'packaging/linux/stage-snapshot-pair.sh').read_text()
        for name, variable in zip(self.names, ['PRIMARY_NAME', 'FALLBACK_NAME', 'LEGACY_FALLBACK_NAME']):
            self.assertEqual(re.search(r'^' + variable + r'="([^"]+)"', source, re.M)[1], name)
        self.assertIn('verify_and_stage "$LEGACY_FALLBACK_DATA"', source)

    def test_macos_defaults_and_copy_destinations_agree(self):
        source = (ROOT / 'packaging/mac/build-installer.sh').read_text()
        defaults = re.findall(r'packaging/mac/snapshot/([^"}]+\.dat)', source)
        self.assertEqual(defaults, self.names)
        for name in self.names:
            self.assertIn('Contents/Resources/' + name, source)

    def test_windows_server_defaults_agree(self):
        builder = (ROOT / 'packaging/windows/build-server-installer.ps1').read_text()
        nsis = (ROOT / 'packaging/windows/dinero-server-installer.nsi').read_text()
        self.assertEqual(re.search(r"\$SnapshotFileName\s*=\s*'([^']+)'", builder)[1], self.names[0])
        self.assertEqual(re.search(r'!define SNAPSHOT_FILE "([^"]+)"', nsis)[1], self.names[0])

    def test_release_workflows_fetch_and_pass_the_new_primary(self):
        for path in ['windows-release.yml', 'windows-openssl-release-smoke.yml',
                     'desktop-release.yml', 'macos-desktop-candidate.yml']:
            source = (ROOT / '.github/workflows' / path).read_text()
            with self.subTest(path=path):
                self.assertIn('--pattern ' + ("'" if path.startswith('windows') else '') + self.names[0], source)
                self.assertIn(self.names[0] + '.manifest.json', source)
                if not path.startswith('windows'):
                    self.assertIn('DINERO_SNAPSHOT_LEGACY_FALLBACK_DAT=', source)
                    self.assertIn('DINERO_SNAPSHOT_LEGACY_FALLBACK_MANIFEST=', source)


if __name__ == '__main__':
    unittest.main()
