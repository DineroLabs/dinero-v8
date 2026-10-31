"""Pinned test vectors must check out byte-for-byte on Windows (B2).

Tests such as BIP119CTVVectors pin the sha256 of the raw vector file. Windows
git defaults to core.autocrlf=true, so without a .gitattributes rule a checkout
rewrites LF to CRLF and the pinned digest no longer matches (PR #860 run 3:
bip119_ctvhash.json read a3c587d5..., pinned 3cff1abe...). `git archive` applies
the same checkout conversion from the committed tree's own .gitattributes, so
this reproduces a Windows checkout of HEAD on any host.
"""
import io
import subprocess
import tarfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def git(*args):
    return subprocess.run(["git", "-C", str(ROOT), *args], check=True, capture_output=True).stdout


class VectorCheckoutBytes(unittest.TestCase):
    def test_autocrlf_checkout_of_every_vector_matches_its_blob(self):
        names = git("ls-tree", "-r", "--name-only", "HEAD", "--", "tests/vectors").decode().split()
        self.assertIn("tests/vectors/bip119_ctvhash.json", names)
        tar = tarfile.open(fileobj=io.BytesIO(
            git("-c", "core.autocrlf=true", "archive", "--format=tar", "HEAD", "--", "tests/vectors")))
        changed = [n for n in names
                   if tar.extractfile(n).read() != git("cat-file", "blob", "HEAD:" + n)]
        self.assertEqual(changed, [])


if __name__ == "__main__":
    unittest.main()
