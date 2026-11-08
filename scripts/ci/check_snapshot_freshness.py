#!/usr/bin/env python3
"""Check a primary desktop snapshot against a separately observed chain tip.

This is a release admission check, not snapshot authentication. The native
loader must still verify the payload, selected-chain binding and burial.
Older snapshots retained for interrupted lifecycles are not primary inputs.
"""
import argparse
import datetime as dt
import json
import pathlib


MAX_BLOCKS_BEHIND = 1000
MAX_AGE = dt.timedelta(days=7)
MIN_BURIAL = 288


def validate(manifest, tip_height=None, now=None):
    now = now or dt.datetime.now(dt.timezone.utc)
    if now.tzinfo is None:
        raise ValueError('current time must include a timezone')
    if tip_height is not None and (type(tip_height) is not int or tip_height < 0):
        raise ValueError('chain tip must be a nonnegative integer')
    if not isinstance(manifest, dict) or manifest.get('network') != 'mainnet':
        raise ValueError('expected a mainnet snapshot manifest')
    snapshot = manifest.get('snapshot')
    if not isinstance(snapshot, dict):
        raise ValueError('missing snapshot identity')
    height = snapshot.get('height')
    if type(height) is not int or height <= 0:
        raise ValueError('snapshot height must be a positive integer')
    behind = None if tip_height is None else tip_height - height
    if behind is not None and behind < MIN_BURIAL:
        raise ValueError(f'primary snapshot requires {MIN_BURIAL} blocks of burial; depth={behind}')
    if behind is not None and behind > MAX_BLOCKS_BEHIND:
        raise ValueError(f'primary snapshot is {behind} blocks behind tip; maximum={MAX_BLOCKS_BEHIND}')
    generated = manifest.get('generated_at')
    if not isinstance(generated, str):
        raise ValueError('missing snapshot generation time')
    try:
        created = dt.datetime.strptime(generated, '%Y%m%dT%H%M%SZ').replace(tzinfo=dt.timezone.utc)
    except ValueError as exc:
        raise ValueError('invalid snapshot generation time') from exc
    age = now - created
    if age < -dt.timedelta(minutes=5):
        raise ValueError('snapshot generation time is in the future')
    if age > MAX_AGE:
        raise ValueError('primary snapshot is older than seven days')
    return {'check': 'age-only' if tip_height is None else 'age-and-depth',
            'snapshot_height': height, 'chain_tip_height': tip_height,
            'blocks_behind': behind, 'age_seconds': int(age.total_seconds())}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', type=pathlib.Path, required=True)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--tip-height', type=int,
                      help='current tip independently observed from a synced node')
    mode.add_argument('--age-only', action='store_true',
                      help='offline package check; does not establish burial or distance to tip')
    args = parser.parse_args()
    try:
        result = validate(json.loads(args.manifest.read_text()), args.tip_height)
    except (OSError, ValueError) as exc:
        parser.exit(1, f'snapshot freshness refused: {exc}\n')
    print(json.dumps(result, sort_keys=True))


if __name__ == '__main__':
    main()
