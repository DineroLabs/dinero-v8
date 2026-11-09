#!/usr/bin/env python3
"""Promote one exact compact v6 generation after both native phone qualifications.

Run on EU1. Mirrors pull independently; trigger their existing mirror services
from the operator host while this command verifies the three public endpoints.
A mirror failure AFTER the atomic local swap is reported as incomplete rollout;
the verified generation remains installed. Rerunning safely retries verification.
"""
import argparse
import base64
import datetime
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time
import urllib.request

from snapshot_transport import inspect

PUBLIC_KEY = base64.b64decode('lFOdW71hOC7Q5Y0bYRLDvyFtv8ddU9Tf4a8dbr1Y4Yw=')
CORE_SHA = '87ef0f46c5ad90f35b7c83b4a91b3edf7cc679fa'
# These are the exact arm64 inputs used by the updated physical-phone builds,
# independently hashed from the distributed SDK/APK. The build wrappers inject
# a version header after the CMake "nodecore" default; captures of the actual
# device VERSION rows are retained in phone_core_identity_v822.json.
CORE_ARTIFACTS = {
    'ios': {'kind': 'ios-arm64-nodecore-archive',
            'sha256': '38cba8889ec1ca1058ebeed3ec5d165d5be8eccda7a830afb5ec96e2bf81e86a'},
    'android': {'kind': 'android-arm64-v8a-nodecore-shared-library',
                'sha256': '7f99699f3c657345538849053cc0be9e40a7f16e99a68631979bec2c491b49a9'},
}
MIRRORS = ('https://seed3.dinerolabs.org/snapshot',
           'https://seed.dinerolabs.org/snapshot',
           'https://seed2.dinerolabs.org/snapshot')
HEX = re.compile(r'^[0-9a-f]{64}$')
CHECKS = ('fleet_signature_verified', 'payload_hash_verified', 'transport_profile_verified',
          'native_import_verified', 'forward_selected_tip_and_accumulator_verified',
          'cold_restart_verified', 'clean_shutdown_verified',
          'altered_payload_rejected', 'full_profile_rejected')


def require(ok, message):
    if not ok:
        raise ValueError(message)


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def read_json(path):
    return json.loads(Path(path).read_bytes())


def verify_signature(manifest, signature):
    require(len(signature) == 64, 'manifest.sig must be a raw 64-byte Ed25519 signature')
    with tempfile.TemporaryDirectory(prefix='phone-signature-') as tmp:
        root = Path(tmp)
        (root/'key.der').write_bytes(bytes.fromhex('302a300506032b6570032100') + PUBLIC_KEY)
        (root/'manifest').write_bytes(manifest)
        (root/'signature').write_bytes(signature)
        result = subprocess.run(['openssl', 'pkeyutl', '-verify', '-pubin', '-keyform', 'DER',
            '-inkey', str(root/'key.der'), '-rawin', '-in', str(root/'manifest'),
            '-sigfile', str(root/'signature')], capture_output=True, timeout=20)
        require(result.returncode == 0, 'manifest signature does not verify against the pinned fleet key')


def validate_manifest(manifest):
    snap = manifest.get('snapshot', {})
    require(manifest.get('network') == 'mainnet' and type(manifest.get('format_version')) is int
            and manifest['format_version'] == 6 and manifest.get('snapshot_profile') == 'compact',
            'expected a mainnet compact v6 manifest')
    require(type(snap.get('height')) is int and snap['height'] >= 150000,
            'v6 base must be at or above 150000')
    require(snap.get('snapshot_file') == 'snapshot.dat', 'phone filename must be snapshot.dat')
    require(type(snap.get('bytes')) is int and 1048576 <= snap['bytes'] <= 536870912,
            'payload exceeds the updated phone download size bounds')
    require(isinstance(snap.get('sha256'), str) and HEX.fullmatch(snap['sha256']), 'invalid payload digest')
    require(isinstance(snap.get('block_hash'), str) and HEX.fullmatch(snap['block_hash']), 'invalid base hash')
    return snap


def verify_generation(generation, staging):
    for name in ('snapshot.dat', 'manifest.json', 'manifest.sig'):
        path = generation/name
        require(path.is_file() and not path.is_symlink(), 'missing or symlinked artifact: ' + name)
    manifest_bytes = (generation/'manifest.json').read_bytes()
    require(hashlib.sha256(manifest_bytes).hexdigest() == staging['manifest_sha256'],
            'manifest digest differs from staging receipt')
    snap = validate_manifest(json.loads(manifest_bytes))
    require(snap == staging['snapshot'], 'snapshot identity differs from staging receipt')
    require((generation/'snapshot.dat').stat().st_size == snap['bytes'] and
            digest(generation/'snapshot.dat') == snap['sha256'], 'payload differs from staging receipt')
    version, height, block, profile = inspect(generation/'snapshot.dat')
    require((version, height, block, profile) == (6, snap['height'], snap['block_hash'], 'compact'),
            'payload header/profile differs from signed manifest')
    verify_signature(manifest_bytes, (generation/'manifest.sig').read_bytes())
    return snap


def verify_phone(receipt_path, platform, staging):
    path = Path(receipt_path)
    receipt = read_json(path)
    require(receipt.get('schema') == 'dinero.phone-snapshot-qualification.v1' and
            receipt.get('state') == 'PASS_NATIVE_COMPACT_V6' and receipt.get('platform') == platform,
            platform + ': missing native qualification receipt')
    snap = staging['snapshot']
    require(receipt.get('network') == 'mainnet' and receipt.get('payload_sha256') == snap['sha256']
            and receipt.get('manifest_sha256') == staging['manifest_sha256'] and
            receipt.get('base_height') == snap['height'] and receipt.get('base_hash') == snap['block_hash'],
            platform + ': qualification is for different snapshot bytes or base')
    require(receipt.get('native_source_sha') == CORE_SHA, platform + ': not the updated qualified Core')
    artifact = receipt.get('native_artifact', {})
    expected = CORE_ARTIFACTS[platform]
    require(artifact.get('kind') == expected['kind'] and artifact.get('sha256') == expected['sha256'],
            platform + ': native artifact is not the exact updated app input')
    relative = Path(artifact.get('path', ''))
    require(artifact.get('path') and not relative.is_absolute() and '..' not in relative.parts,
            platform + ': invalid native artifact path')
    native_binary = path.parent/relative
    require(native_binary.is_file() and not native_binary.is_symlink() and
            digest(native_binary) == expected['sha256'], platform + ': native artifact bytes differ from the pin')
    require(isinstance(receipt.get('app_version'), str) and receipt['app_version'].strip(),
            platform + ': missing app identity')
    require(all(receipt.get('checks', {}).get(check) is True for check in CHECKS),
            platform + ': incomplete import/restart/negative-control qualification')
    require(type(receipt.get('forward_height')) is int and receipt['forward_height'] > snap['height'],
            platform + ': no post-base validation')
    require(receipt.get('validation_role') == 'compact_validator' and
            receipt.get('retains_full_state') is False and
            type(receipt.get('forest_memory_bytes_estimate')) is int and receipt['forest_memory_bytes_estimate'] == 0,
            platform + ': native runtime did not retain the compact role')
    observers = receipt.get('independent_observers', [])
    require(isinstance(observers, list) and all(isinstance(x, str) and x for x in observers)
            and len(set(observers)) >= 2, platform + ': independent fleet agreement missing')
    # Receipts bind the retained execution evidence, rather than a free-standing
    # PASS string. Never copy these private device logs into the web archive.
    evidence = receipt.get('evidence', {})
    evidence_paths = {}
    for name in ('initial_import', 'cold_restart', 'transport', 'negative_controls', 'fleet_agreement'):
        item = evidence.get(name, {})
        relative = Path(item.get('path', ''))
        require(item.get('path') and not relative.is_absolute() and '..' not in relative.parts,
                platform + ': invalid evidence path for ' + name)
        target = path.parent/relative
        require(target.is_file() and not target.is_symlink() and digest(target) == item.get('sha256'),
                platform + ': evidence missing or changed: ' + name)
        evidence_paths[name] = target
    initial = verify_native_log(evidence_paths['initial_import'], snap, cold=False)
    cold = verify_native_log(evidence_paths['cold_restart'], snap, cold=True)
    require(receipt['forward_height'] == initial['verified_height'], platform + ': forward receipt/log mismatch')
    agreement = read_json(evidence_paths['fleet_agreement'])
    require(agreement.get('network') == 'mainnet', platform + ': wrong fleet reference network')
    for sample in (initial, cold):
        matching = [row for row in agreement.get('checkpoints', [])
                    if row.get('height') == sample['verified_height'] and
                    row.get('block_hash') == sample['verified_block_hash'] and
                    row.get('commitment') == sample['commitment'] and
                    isinstance(row.get('observers'), list) and
                    all(isinstance(x, str) for x in row['observers']) and
                    len(set(row['observers']) & set(observers)) >= 2]
        require(matching, platform + ': native tip/accumulator lacks recorded independent agreement')
    return digest(path)


def verify_native_log(path, snap, cold):
    text = Path(path).read_text(errors='replace')
    require('[snapshot] rejected' not in text and '[IBD] Snapshot load failed' not in text,
            'native evidence contains snapshot refusal/fallback')
    records = [line.split('\t', 2) for line in text.splitlines() if line.startswith('NCPROBE\t')]
    require(any(row[1:] == ['START', '0'] for row in records) and
            any(row[1:] == ['STOP', '0'] for row in records), 'native evidence lacks clean start/stop')
    verify_native_version(text)
    bootstraps, accumulators = [], []
    for row in records:
        if len(row) != 3: continue
        if row[1] == 'getsnapshotbootstrapstatus':
            response = json.loads(row[2])
            if not isinstance(response, dict): continue  # a busy native RPC may return null
            state = response.get('snapshot_bootstrap', {})
            if not isinstance(state, dict): continue
            require(state.get('fatal') is not True, 'native evidence contains fatal snapshot state')
            if state.get('snapshot_base_height') == snap['height'] and state.get('snapshot_base_block') == snap['block_hash']:
                bootstraps.append(state)
        elif row[1] == 'blockchain.getutreexocommitment':
            state = json.loads(row[2])
            if not isinstance(state, dict): continue
            if (state.get('validation_role') == 'compact_validator' and state.get('retains_full_state') is False
                and state.get('forest_memory_bytes_estimate') == 0 and
                type(state.get('verified_height')) is int and state['verified_height'] > snap['height'] and
                HEX.fullmatch(str(state.get('verified_block_hash', ''))) and HEX.fullmatch(str(state.get('commitment', '')))):
                accumulators.append(state)
    require(bootstraps and accumulators, 'native evidence does not prove exact-base compact import and forward validation')
    if cold:
        require(any(state.get('configured_snapshot') is False for state in bootstraps),
                'cold restart still had a configured import snapshot')
        require('[snapshot] loaded' not in text, 'cold restart reimported the snapshot')
    else:
        require(any(state.get('manifest_required') is True for state in bootstraps),
                'native import did not require the exact manifest')
    return accumulators[-1]


def verify_native_version(text):
    versions = [row[2] for line in text.splitlines()
                if len(row := line.split('\t', 2)) == 3 and row[:2] == ['NCPROBE', 'VERSION']]
    require(versions and all(value == 'dinerod 8.2.2-' + CORE_SHA for value in versions),
            'native evidence has wrong Core version')


def verify_chain(cli, datadir, snap):
    def rpc(*args):
        result = subprocess.run([cli, '-datadir=' + str(datadir), *args],
                                capture_output=True, text=True, timeout=30, check=True)
        return json.loads(result.stdout)
    tip = rpc('getblockcount')
    require(type(tip) is int and tip >= snap['height'] + 288, 'candidate lacks 288 selected descendants')
    require(rpc('getblockhash', str(snap['height'])) == snap['block_hash'],
            'candidate base is no longer on the selected chain')


def sync_directory(path):
    fd = os.open(path, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def write_report(path, report):
    path = Path(path)
    temporary = path.with_name(path.name + '.tmp')
    with temporary.open('w') as stream:
        json.dump(report, stream, indent=2); stream.write('\n'); stream.flush(); os.fsync(stream.fileno())
    os.replace(temporary, path)
    sync_directory(path.parent)


def install_generation(root, generation, staging, phone_hashes, chain_check, on_swap=lambda *_: None):
    require(root.is_dir(), 'public root must already exist')
    with (root/'.publisher.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        marker = root/'.snapshot-profile'
        if marker.exists():
            require(not marker.is_symlink() and marker.read_text() == 'compact\n',
                    'public root belongs to another snapshot profile')
        if (root/'latest').exists() or (root/'latest').is_symlink():
            current = read_json(root/'latest/manifest.json')
            require(current.get('network') == 'mainnet' and
                    ((current.get('format_version') in (4, 5) and current.get('snapshot_profile') in (None, 'legacy'))
                     or (current.get('format_version') == 6 and current.get('snapshot_profile') == 'compact')),
                    'refusing to replace a non-phone endpoint')
            require(current['snapshot']['height'] <= staging['snapshot']['height'], 'refusing snapshot height rollback')
        archive = root/'archive'; archive.mkdir(exist_ok=True)
        name = 'compact-v6-h%d-%s' % (staging['snapshot']['height'], staging['manifest_sha256'])
        destination = archive/name
        if destination.exists():
            require(not destination.is_symlink(), 'archive generation is a symlink')
            verify_generation(destination, staging)
        else:
            with tempfile.TemporaryDirectory(prefix='.phone-v6-', dir=archive) as temporary:
                copied = Path(temporary)/'generation'; copied.mkdir()
                for filename in ('snapshot.dat', 'manifest.json', 'manifest.sig'):
                    shutil.copyfile(generation/filename, copied/filename)
                    with (copied/filename).open('rb') as stream: os.fsync(stream.fileno())
                    (copied/filename).chmod(0o644)
                verify_generation(copied, staging)
                summary = {'schema': 'dinero.phone-snapshot-promotion.v1',
                           'payload_sha256': staging['snapshot']['sha256'],
                           'manifest_sha256': staging['manifest_sha256'], 'phone_receipts_sha256': phone_hashes,
                           'core_source_sha': CORE_SHA, 'phone_native_artifacts': CORE_ARTIFACTS}
                (copied/'qualification-summary.json').write_text(json.dumps(summary, indent=2) + '\n')
                (copied/'qualification-summary.json').chmod(0o644)
                with (copied/'qualification-summary.json').open('rb') as stream: os.fsync(stream.fileno())
                copied.chmod(0o755); sync_directory(copied); os.rename(copied, destination)
            sync_directory(archive)
        # Catch source reorgs during phone qualification or the copy. No key
        # rotation, boundary pin rewrite or archive retention occurs here.
        chain_check()
        if not marker.exists():
            with marker.open('x') as stream:
                stream.write('compact\n'); stream.flush(); os.fsync(stream.fileno())
        previous = os.readlink(root/'latest') if (root/'latest').is_symlink() else None
        require(not (root/'latest').exists() or (root/'latest').is_symlink(), 'latest is not a symlink')
        temporary = root/('.latest-phone-v6-' + str(os.getpid()))
        try:
            os.symlink('archive/' + name, temporary)
            os.replace(temporary, root/'latest')
            on_swap(str(destination), previous)
            sync_directory(root)
        finally:
            if temporary.is_symlink(): temporary.unlink()
        return str(destination), previous


def fetch_small(url, maximum):
    request = urllib.request.Request(url, headers={'Cache-Control': 'no-cache'})
    with urllib.request.urlopen(request, timeout=30) as stream:
        data = stream.read(maximum + 1)
    require(len(data) <= maximum, 'oversized mirror metadata')
    return data


def check_mirror(url, staging):
    manifest = fetch_small(url + '/manifest.json', 65536)
    require(hashlib.sha256(manifest).hexdigest() == staging['manifest_sha256'], 'mirror manifest differs')
    verify_signature(manifest, fetch_small(url + '/manifest.sig', 64))
    expected = staging['snapshot']; hasher = hashlib.sha256(); size = 0
    with urllib.request.urlopen(urllib.request.Request(url + '/snapshot.dat',
            headers={'Cache-Control': 'no-cache'}), timeout=60) as stream:
        while True:
            chunk = stream.read(1024 * 1024)
            if not chunk: break
            size += len(chunk)
            require(size <= expected['bytes'], 'mirror payload too large')
            hasher.update(chunk)
    require(size == expected['bytes'] and hasher.hexdigest() == expected['sha256'], 'mirror payload differs')
    # A concurrent set switch must not produce a mixed-generation success.
    require(fetch_small(url + '/manifest.json', 65536) == manifest, 'mirror changed during verification')
    return {'state': 'VERIFIED', 'payload_sha256': hasher.hexdigest(),
            'manifest_sha256': hashlib.sha256(manifest).hexdigest(), 'bytes': size}


def verify_mirrors(staging, report, report_path, timeout, interval, mirrors=MIRRORS):
    require(len(mirrors) == 3 and len(set(mirrors)) == 3, 'all three distinct mirrors must be checked')
    deadline = time.monotonic() + timeout
    while True:
        results = {}
        for mirror in mirrors:
            try:
                results[mirror] = check_mirror(mirror, staging)
            except Exception as error:
                results[mirror] = {'state': 'NOT_VERIFIED', 'error': str(error)}
        report['mirrors'] = results
        if all(result['state'] == 'VERIFIED' for result in results.values()):
            report['state'] = 'PROMOTED_ALL_MIRRORS_VERIFIED'; write_report(report_path, report); return
        write_report(report_path, report)
        if time.monotonic() >= deadline:
            raise ValueError('not all three mirrors serve the exact qualified generation')
        time.sleep(min(interval, max(0, deadline-time.monotonic())))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--staging-receipt', type=Path, required=True)
    parser.add_argument('--ios-receipt', type=Path, required=True)
    parser.add_argument('--android-receipt', type=Path, required=True)
    parser.add_argument('--pubroot', type=Path, default=Path('/var/www/dinero-snapshots'))
    parser.add_argument('--datadir', type=Path, default=Path('/var/lib/dinero'))
    parser.add_argument('--cli', default='/usr/bin/dinero-cli')
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--mirror-timeout', type=int, default=900)
    parser.add_argument('--mirror-interval', type=int, default=15)
    parser.add_argument('--dry-run', action='store_true')
    args = parser.parse_args()
    require(args.mirror_timeout >= 0 and args.mirror_interval > 0, 'invalid mirror polling limits')
    report = {'schema': 'dinero.phone-snapshot-promotion.v1', 'state': 'PRECHECKS_RUNNING',
              'started_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'public_promotion_performed': False}
    try:
        staging = read_json(args.staging_receipt)
        require(staging.get('state') == 'SERVER_NATIVE_IMPORT_PASSED_PHONE_QUALIFICATION_REQUIRED',
                'missing server native import staging receipt')
        generation = Path(staging['generation']).resolve(strict=True)
        snap = verify_generation(generation, staging)
        phones = {platform: verify_phone(path, platform, staging)
                  for platform, path in (('ios', args.ios_receipt), ('android', args.android_receipt))}
        require(phones['ios'] != phones['android'], 'phone receipts must be distinct')
        chain_check = lambda: verify_chain(args.cli, args.datadir, snap)
        chain_check()
        report.update(payload_sha256=snap['sha256'], manifest_sha256=staging['manifest_sha256'],
                      base_height=snap['height'], base_hash=snap['block_hash'], phone_receipts_sha256=phones)
        if args.dry_run:
            report['state'] = 'DRY_RUN_PRECHECKS_PASSED'; write_report(args.report, report)
            print(json.dumps(report, indent=2)); return 0
        report['state'] = 'PRECHECKS_PASSED'; write_report(args.report, report)
        def on_swap(installed, previous):
            # Mark the local change before any later fsync/network operation
            # can fail, so an incomplete rollout cannot be called preflight refusal.
            report.update(state='LOCAL_PROMOTED_VERIFYING_MIRRORS', public_promotion_performed=True,
                          installed_generation=installed, previous_latest=previous)
            write_report(args.report, report)
        install_generation(args.pubroot, generation, staging, phones, chain_check, on_swap)
        verify_mirrors(staging, report, args.report, args.mirror_timeout, args.mirror_interval)
        print(json.dumps(report, indent=2)); return 0
    except Exception as error:
        report['state'] = ('LOCAL_PROMOTED_MIRROR_VERIFICATION_FAILED' if report['public_promotion_performed']
                           else 'REFUSED_BEFORE_PROMOTION')
        report['error'] = str(error); write_report(args.report, report)
        print(json.dumps(report, indent=2)); return 1


if __name__ == '__main__':
    raise SystemExit(main())
