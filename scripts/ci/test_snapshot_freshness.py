#!/usr/bin/env python3
import copy
import datetime as dt
import unittest

from check_snapshot_freshness import validate


class SnapshotFreshness(unittest.TestCase):
    def setUp(self):
        self.now = dt.datetime(2026, 11, 7, 12, tzinfo=dt.timezone.utc)
        self.tip = 148663
        self.manifest = {'network': 'mainnet', 'snapshot': {'height': 148375},
                         'generated_at': '20261107T100000Z'}

    def check(self, manifest=None, tip=None):
        return validate(self.manifest if manifest is None else manifest,
                        self.tip if tip is None else tip, self.now)

    def test_recent_buried_primary(self):
        self.assertEqual(self.check()['blocks_behind'], 288)
        self.assertEqual(self.check(tip=149375)['blocks_behind'], 1000)

    def test_offline_check_does_not_claim_depth_qualification(self):
        result = validate(self.manifest, now=self.now)
        self.assertEqual(result['check'], 'age-only')
        self.assertIsNone(result['blocks_behind'])
        self.manifest['generated_at'] = '20261012T043837Z'
        with self.assertRaisesRegex(ValueError, 'seven days'):
            validate(self.manifest, now=self.now)

    def test_old_release_height_refused_even_with_recent_timestamp(self):
        self.manifest['snapshot']['height'] = 99677
        with self.assertRaisesRegex(ValueError, 'blocks behind'):
            self.check()

    def test_old_mirror_height_refused(self):
        self.manifest['snapshot']['height'] = 128475
        with self.assertRaisesRegex(ValueError, 'blocks behind'):
            self.check()

    def test_unburied_or_ahead_snapshot_refused(self):
        for tip in [148662, 148375, 148374]:
            with self.subTest(tip=tip), self.assertRaisesRegex(ValueError, 'burial'):
                self.check(tip=tip)

    def test_stale_generation_time_refused_even_at_recent_height(self):
        self.manifest['generated_at'] = '20261012T043837Z'
        with self.assertRaisesRegex(ValueError, 'seven days'):
            self.check()

    def test_future_generation_time_refused(self):
        self.manifest['generated_at'] = '20261107T120501Z'
        with self.assertRaisesRegex(ValueError, 'future'):
            self.check()

    def test_malformed_identity_or_time_refused(self):
        for field, value in [('network', 'regtest'), ('snapshot', None),
                             ('snapshot', {'height': True}), ('snapshot', {'height': -1}),
                             ('generated_at', None), ('generated_at', 'yesterday')]:
            altered = copy.deepcopy(self.manifest)
            altered[field] = value
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                self.check(altered)
        for tip in [True, -1, '148663']:
            with self.subTest(tip=tip), self.assertRaises(ValueError):
                self.check(tip=tip)


if __name__ == '__main__':
    unittest.main()
