"""Cross-platform package defaults must agree with the pinned input catalog."""
import json
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SnapshotWiring(unittest.TestCase):
    def setUp(self):
        catalog = json.loads((ROOT / 'packaging/snapshot/desktop-inputs-candidate.json').read_text(encoding='utf-8'))
        pinned = [catalog['primary']['filename'], *(e['filename'] for e in catalog['fallbacks'])]
        # Package defaults must follow the exact pinned primary and five
        # older recovery inputs. Native admission is qualified separately.
        self.names = pinned
        self.primary_height = catalog['primary']['height']
        self.assertEqual(len(self.names), 6)
        self.assertEqual(catalog['fallbacks'][0]['height'], 153578)
        self.assertEqual(catalog['fallbacks'][0]['sha256'],
                         '3bb6185106744297a8ae0e6f51c0f734aab8a70eaa7044d8d5dc1880b1cf57cd')
        self.assertEqual(catalog['fallbacks'][1]['height'], 153051)
        self.assertEqual(catalog['fallbacks'][1]['sha256'],
                         '20d87ae691ebc9cd4c42a8800ce7d69af58a4bb97a565f4a8bc1532792fb513f')

    def test_release_notes_name_the_pinned_primary_height(self):
        notes = (ROOT / 'packaging/RELEASE-NOTES-TEMPLATE.md').read_text(encoding='utf-8')
        self.assertIn(f'| {self.primary_height:,} (preferred full-v6) |', notes)
        self.assertIn(self.names[0], notes)
        self.assertIn(f'SHA256SUMS-assumeutxo-{self.primary_height}', notes)
        self.assertIn('SHA256SUMS-assumeutxo-153051', notes)
        self.assertIn(self.names[1] + '.publisher.manifest.sig', notes)

    def test_gui_selection_and_windows_derivation_cover_all_six(self):
        source = (ROOT / 'qt/src/main.cpp').read_text(encoding='utf-8')
        builder = (ROOT / 'packaging/windows/build-installer.ps1').read_text(encoding='utf-8')
        for name, constant in zip(self.names, ['Primary', 'PreviousPrimary', 'RecentFallback', 'Fallback', 'LegacyFallback', 'OldestFallback']):
            self.assertEqual(re.search(r'kBundledSnapshot' + constant + r'\[\]\s*=\s*"([^"]+)"', source)[1], name)
            if constant == 'PreviousPrimary':
                self.assertIn("$previousName = '" + name + "'", builder)
                self.assertIn("throw 'Required H153578 exact-lifecycle snapshot fallback missing.'", builder)
            else:
                self.assertIn('kBundledSnapshot' + constant + r'\[\]', builder)
        self.assertIn('SelectBundledSnapshots({', source)
        self.assertIn("fallbacks.join(';')", source)
        self.assertIn("throw 'Required older exact-lifecycle snapshot fallback missing.'", builder)
        self.assertIn("throw 'Required oldest exact-lifecycle snapshot fallback missing.'", builder)
        self.assertIn("throw 'Required H153051 exact-lifecycle snapshot fallback missing.'", builder)

    def test_linux_stages_all_six(self):
        source = (ROOT / 'packaging/linux/stage-snapshot-pair.sh').read_text(encoding='utf-8')
        for name, variable in zip(self.names, ['PRIMARY_NAME', 'PREVIOUS_PRIMARY_NAME', 'RECENT_FALLBACK_NAME', 'FALLBACK_NAME', 'LEGACY_FALLBACK_NAME', 'OLDEST_FALLBACK_NAME']):
            self.assertEqual(re.search(r'^' + variable + r'="([^"]+)"', source, re.M)[1], name)
        self.assertIn('verify_and_stage "$LEGACY_FALLBACK_DATA"', source)
        self.assertIn('verify_and_stage "$OLDEST_FALLBACK_DATA"', source)
        self.assertIn('verify_and_stage "$PREVIOUS_PRIMARY_DATA"', source)
        self.assertIn('verify_and_stage "$RECENT_FALLBACK_DATA"', source)
        self.assertIn('--recent-fallback', source)
        self.assertIn('check_desktop_primary.py', source)

    def test_macos_defaults_and_copy_destinations_agree(self):
        source = (ROOT / 'packaging/mac/build-installer.sh').read_text(encoding='utf-8')
        defaults = re.findall(r'packaging/mac/snapshot/([^"}]+\.dat)', source)
        self.assertEqual(defaults, [self.names[0], *self.names[2:]])
        self.assertIn('SNAPSHOT_PREVIOUS_PRIMARY_DAT="$(dirname "$SNAPSHOT_DAT")/' + self.names[1], source)
        for name in self.names:
            self.assertIn('Contents/Resources/' + name, source)

    def test_windows_server_defaults_agree(self):
        builder = (ROOT / 'packaging/windows/build-server-installer.ps1').read_text(encoding='utf-8')
        nsis = (ROOT / 'packaging/windows/dinero-server-installer.nsi').read_text(encoding='utf-8')
        self.assertEqual(re.search(r"\$SnapshotFileName\s*=\s*'([^']+)'", builder)[1], self.names[0])
        self.assertEqual(re.search(r'!define SNAPSHOT_FILE "([^"]+)"', nsis)[1], self.names[0])
        self.assertIn('check_desktop_primary.py', builder)
        self.assertIn("if ($SnapshotFileName -match '-v6-full\\.dat$')", builder)

    def test_release_workflows_fetch_and_pass_the_new_primary(self):
        for path in ['windows-release.yml', 'desktop-release.yml', 'macos-desktop-candidate.yml']:
            source = (ROOT / '.github/workflows' / path).read_text(encoding='utf-8')
            with self.subTest(path=path):
                self.assertIn('fetch_desktop_primary.py', source)
                if path == 'macos-desktop-candidate.yml':
                    self.assertIn('--payload-url "$SNAPSHOT_PAYLOAD_URL"', source)
                    self.assertIn('--manifest-url "$SNAPSHOT_MANIFEST_URL"', source)
                    self.assertIn('cp release-inputs/*publisher.* packaging/mac/dist/', source)
                else:
                    self.assertIn('--pattern ' + ("'" if path.startswith('windows') else '') + self.names[0], source)
                    expected_count = 2 if path == 'desktop-release.yml' else 1
                    self.assertGreaterEqual(source.count(self.names[0] + '.publisher.manifest.json'), expected_count)
                    self.assertGreaterEqual(source.count(self.names[0] + '.publisher.manifest.sig'), expected_count)
                self.assertIn(self.names[0] + '.manifest.json', source)
                self.assertIn('--fallback-height 153051', source)
                self.assertIn("--pattern '" + self.names[1] + "*'", source)
                self.assertIn('h153051-f7-05105aa3-20d87ae6/', source)
                self.assertIn(self.names[1], source)
                if path.startswith('windows'):
                    self.assertIn('-SnapshotRecentFallbackPath', source)
                    self.assertIn('-SnapshotRecentFallbackManifestPath', source)
                if path.startswith('windows'):
                    # The hosted Windows runner exposes Python as `python`;
                    # no setup-python step provides a `python3` executable.
                    self.assertNotRegex(source, r'\bpython3\b')
                else:
                    self.assertIn('DINERO_SNAPSHOT_RECENT_FALLBACK_DAT=', source)
                    self.assertIn('DINERO_SNAPSHOT_RECENT_FALLBACK_MANIFEST=', source)
                    self.assertIn('DINERO_SNAPSHOT_LEGACY_FALLBACK_DAT=', source)
                    self.assertIn('DINERO_SNAPSHOT_LEGACY_FALLBACK_MANIFEST=', source)
                    self.assertIn('DINERO_SNAPSHOT_OLDEST_FALLBACK_DAT=', source)
                    self.assertIn('DINERO_SNAPSHOT_OLDEST_FALLBACK_MANIFEST=', source)
        for path in ['desktop-release.yml', 'macos-desktop-candidate.yml']:
            source = (ROOT / '.github/workflows' / path).read_text(encoding='utf-8')
            self.assertIn('cmp -s "${GITHUB_WORKSPACE}/tooling/packaging/mac/build-installer.sh" packaging/mac/build-installer.sh', source)

        smoke = (ROOT / '.github/workflows/windows-openssl-release-smoke.yml').read_text(encoding='utf-8')
        self.assertIn("-SnapshotFileName 'dinero-assumeutxo-155650-v6-full.dat'", smoke)
        self.assertIn('fetch_desktop_primary.py', smoke)
        candidate_url = f'https://seed3.dinerolabs.org/v823-candidate/v824/h{self.primary_height}'
        for name, flag in [('snapshot.dat', '--payload-url'),
                           ('manifest.json', '--manifest-url'),
                           ('manifest.sig', '--signature-url')]:
            self.assertIn(f"{flag} '{candidate_url}/{name}'", smoke)
        self.assertIn('--publisher-signature snapshot-inputs/dinero-assumeutxo-155650-v6-full.dat.publisher.manifest.sig', smoke)
        version = re.search(r"-Version '([^']+)'", smoke)[1]
        filename = 'Dinero-Server-' + version + '-windows-x86_64-Setup.exe'
        self.assertEqual(smoke.count(filename), 2, 'Install and artifact paths must match the built version')
        numeric_version = version.split('-')[0] + '.0'
        self.assertIn("$numericVersion -ne '" + numeric_version + "'", smoke)

    def test_pr_workflows_do_not_fetch_live_full_v6_primary_before_rotation(self):
        for workflow in (ROOT / '.github/workflows').glob('*.yml'):
            source = workflow.read_text(encoding='utf-8')
            if 'pull_request:' not in source:
                continue
            with self.subTest(workflow=workflow.name):
                self.assertNotRegex(source, r'https://[^\s\'"/]+/snapshot/full-v6/(?:latest/)?(?:snapshot\.dat|manifest\.json)')

    def test_h153051_release_builds_use_same_tag_signed_assets(self):
        base = self.names[2]
        signed_names = (base, base + '.manifest.json',
                        base + '.publisher.manifest.json',
                        base + '.publisher.manifest.sig')
        candidate_route = 'h153051-f7-05105aa3-20d87ae6/'
        desktop = (ROOT / '.github/workflows/desktop-release.yml').read_text(encoding='utf-8')
        desktop_steps = desktop.split('- name: Fetch full-v6 primary and exact recovery bases')[1:]
        self.assertEqual(len(desktop_steps), 2)
        for index, fragment in enumerate(desktop_steps):
            step = fragment.split('\n      - name:', 1)[0]
            if index == 0:
                candidate, release = step.split('          else\n', 1)
                self.assertIn(candidate_route, candidate)
                self.assertNotIn(candidate_route, release)
            else:
                release = step
                self.assertNotIn(candidate_route, release)
            self.assertIn('gh release download "${{ inputs.release_tag }}"', release)
            for name in signed_names:
                self.assertIn('--pattern ' + name, release)

        windows = (ROOT / '.github/workflows/windows-release.yml').read_text(encoding='utf-8')
        step = windows.split('- name: Fetch full-v6 primary and exact recovery bases', 1)[1].split('\n      - name:', 1)[0]
        release, candidate = step.split('          else\n', 1)
        self.assertIn(candidate_route, candidate)
        self.assertNotIn(candidate_route, release)
        self.assertIn('gh release download "$PRIMARY_RELEASE"', release)
        for name in signed_names:
            self.assertIn("--pattern '" + name + "'", release)

    def test_full_v6_fallbacks_precede_older_bases_in_every_runtime(self):
        for path in ('qt/src/main.cpp', 'docker-entrypoint.sh'):
            source = (ROOT / path).read_text(encoding='utf-8')
            with self.subTest(path=path):
                self.assertIn(self.names[1], source)
                self.assertLess(source.index(self.names[1]), source.index(self.names[2]))
                self.assertLess(source.index(self.names[2]), source.index(self.names[3]))
        docker = (ROOT / 'Dockerfile').read_text(encoding='utf-8')
        fallback_list = re.search(r'ENV DINERO_SNAPSHOT_FALLBACKS="([^"]+)"', docker)[1]
        self.assertLess(fallback_list.index(self.names[1]), fallback_list.index(self.names[2]))
        self.assertLess(fallback_list.index(self.names[2]), fallback_list.index(self.names[3]))
        self.assertIn('dinero-assumeutxo-153051-v6-full.dat*',
                      (ROOT / 'Dockerfile').read_text(encoding='utf-8'))
        self.assertIn('recent-fallback',
                      (ROOT / 'packaging/mac/build-installer.sh').read_text(encoding='utf-8'))


if __name__ == '__main__':
    unittest.main()
