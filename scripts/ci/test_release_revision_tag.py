#!/usr/bin/env python3
"""Execute the three release workflows' version steps against revision tags."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS = ('linux-release.yml', 'windows-release.yml', 'macos-release.yml')


def version_script(workflow: str) -> str:
    lines = (ROOT / '.github' / 'workflows' / workflow).read_text().splitlines()
    step = next(i for i, line in enumerate(lines) if line.strip() == '- name: Resolve version')
    start = next(i for i in range(step + 1, len(lines)) if lines[i].strip() == 'run: |')
    indent = len(lines[start]) - len(lines[start].lstrip())
    body = []
    for line in lines[start + 1:]:
        if line.strip() and len(line) - len(line.lstrip()) <= indent:
            break
        body.append(line[indent + 2:])
    return '\n'.join(body)


def evaluate(workflow: str, *, ref_type: str, ref_name: str, manual_tag: str = '',
             candidate_source: str = ''):
    script = version_script(workflow)
    script = (script.replace('$' + '{{ github.ref_type }}', ref_type)
                    .replace('$' + '{{ inputs.release_tag }}', manual_tag)
                    .replace('$' + '{{ matrix.arch }}', 'arm64'))
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory) / 'output'
        env = dict(os.environ, GITHUB_OUTPUT=str(output), GITHUB_REF_TYPE=ref_type,
                   GITHUB_REF_NAME=ref_name, RELEASE_TAG_INPUT=manual_tag,
                   CANDIDATE_SOURCE=candidate_source)
        process = subprocess.run(['bash', '-e', '-c', script], cwd=ROOT, env=env,
                                 text=True, capture_output=True)
        fields = dict(line.split('=', 1) for line in output.read_text().splitlines()) if output.exists() else {}
        return process, fields


class ReleaseRevisionTagTest(unittest.TestCase):
    def test_revision_tag_preserves_tag_and_numeric_product_version(self):
        for workflow in WORKFLOWS:
            with self.subTest(workflow=workflow):
                process, fields = evaluate(workflow, ref_type='tag', ref_name='v8.2.4-r1')
                self.assertEqual(process.returncode, 0, process.stderr)
                self.assertEqual(fields['version'], '8.2.4')
                self.assertEqual(fields['release_tag'], 'v8.2.4-r1')

    def test_plain_tag_remains_supported(self):
        for workflow in WORKFLOWS:
            with self.subTest(workflow=workflow):
                process, fields = evaluate(workflow, ref_type='tag', ref_name='v8.2.4')
                self.assertEqual(process.returncode, 0, process.stderr)
                self.assertEqual(fields['version'], '8.2.4')
                self.assertEqual(fields['release_tag'], 'v8.2.4')

    def test_bad_revision_and_source_mismatch_refuse(self):
        for workflow in WORKFLOWS:
            for tag in ('v8.2.4-r0', 'v8.2.4-r1-extra', 'v8.2.5-r1'):
                with self.subTest(workflow=workflow, tag=tag):
                    process, _ = evaluate(workflow, ref_type='tag', ref_name=tag)
                    self.assertNotEqual(process.returncode, 0)

    def test_manual_revision_dispatch(self):
        for workflow in WORKFLOWS:
            with self.subTest(workflow=workflow):
                process, fields = evaluate(workflow, ref_type='branch', ref_name='work',
                                           manual_tag='v8.2.4-r1')
                self.assertEqual(process.returncode, 0, process.stderr)
                self.assertEqual(fields['version'], '8.2.4')
                self.assertEqual(fields['release_tag'], 'v8.2.4-r1')

    def test_candidate_source_still_uses_source_version(self):
        head = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
        for workflow in WORKFLOWS[:2]:
            with self.subTest(workflow=workflow):
                process, fields = evaluate(workflow, ref_type='branch', ref_name='work',
                                           candidate_source=head)
                self.assertEqual(process.returncode, 0, process.stderr)
                self.assertEqual(fields['version'], '8.2.4')
                self.assertEqual(fields['release_tag'], '')
                self.assertEqual(fields['source_sha'], head)


if __name__ == '__main__':
    unittest.main()
