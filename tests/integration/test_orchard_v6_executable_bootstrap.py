#!/usr/bin/env python3
"""Actual main()/dinerod regression against an explicit private-chain fixture.

The fixture provides an already-buried authentic v6 snapshot and a loopback
peer serving its real selected headers/bodies. It never pre-creates ChainDB or
substitutes DaemonApp/test-only startup for the actual executable. A right tip
obtained by fallback IBD is a failure. Workdirs/evidence are always retained.
Two actual cold restarts remove the snapshot request and must retain the same
authenticated base, selected tip, accumulator and full/root-only storage role.
No fixture is registered by default: this is not an empty/skipped release gate.
"""
import argparse
import base64
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
import traceback
import urllib.error
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dinerod', required=True, type=Path)
    parser.add_argument('--fixture', required=True, type=Path)
    parser.add_argument('--profile', required=True, choices=('full', 'compact'))
    parser.add_argument('--work-parent', required=True, type=Path)
    parser.add_argument('--reopen-copy', type=Path,
                        help='copy an explicitly stopped fixture datadir; never open or modify the original')
    args = parser.parse_args()
    os.umask(0o077)
    fixture = json.loads(args.fixture.read_text())
    assert fixture['network'] in ('testnet', 'regtest'), 'never public/mainnet'
    host, port = fixture['peer'].rsplit(':', 1)
    assert ipaddress.ip_address(host).is_loopback and 1 <= int(port) <= 65535
    binary = args.dinerod.resolve(strict=True)
    snapshot = Path(fixture[args.profile + '_snapshot']).resolve(strict=True)
    assert hashlib.sha256(snapshot.read_bytes()).hexdigest() == fixture[args.profile + '_sha256']
    assert int.from_bytes(snapshot.read_bytes()[4:8], 'little') == 6
    assert args.work_parent.is_dir() and not args.work_parent.is_symlink()
    work = Path(tempfile.mkdtemp(prefix='v6-executable-' + args.profile + '-', dir=args.work_parent)).resolve()
    data = work / 'datadir'
    if args.reopen_copy:
        source_data = args.reopen_copy.resolve(strict=True)
        assert source_data.is_dir() and not args.reopen_copy.is_symlink()
        shutil.copytree(source_data, data, symlinks=True)
    else:
        data.mkdir(mode=0o700)
    reservations = []
    for _ in range(3):
        sock = socket.socket()
        sock.bind(('127.0.0.1', 0))
        reservations.append(sock)
    rpc_port, p2p_port, wallet_port = [sock.getsockname()[1] for sock in reservations]
    config = work / 'dinero.conf'
    config.write_text('wallet.socket.enable=false\nlightning.oracles.enable=false\nrpcbind=127.0.0.1\n'
                      + ('' if args.reopen_copy else 'assumeutxo_snapshot=' + str(snapshot) + '\n'))
    command = [str(binary), '--' + fixture['network'], '--datadir=' + str(data), '--conf=' + str(config),
               '--rpcport=' + str(rpc_port), '--port=' + str(p2p_port), '--wallet-socket-port=' + str(wallet_port),
               '--listen=1', '--dnsseed=0', '--fixedseeds=0', '--discover=0', '--listenonion=0',
               '--connect=' + fixture['peer'], '--utreexo=1', '--utreexo-bridge=1',
               '--sync-profile=' + ('ios_utreexo' if args.profile == 'compact' else 'mac_fullblock')]
    receipt = {'state': 'STARTING', 'scope': 'actual dinerod executable; no hand-created store or header authority',
               'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(), 'command': command,
               'profile': args.profile, 'fixture': fixture, 'workdir': str(work)}
    if args.reopen_copy:
        receipt['reopen_source_copy'] = str(source_data)
    path = work / 'receipt.json'

    def save():
        tmp = path.with_suffix('.tmp')
        tmp.write_text(json.dumps(receipt, indent=2) + '\n')
        os.replace(tmp, path)

    def rpc(method, params=None):
        auth = base64.b64encode((data / '.cookie').read_bytes().strip()).decode()
        request = urllib.request.Request('http://127.0.0.1:' + str(rpc_port) + '/',
            json.dumps({'jsonrpc': '2.0', 'id': 1, 'method': method, 'params': [] if params is None else params}).encode(),
            {'Content-Type': 'application/json', 'Authorization': 'Basic ' + auth})
        try:
            response = urllib.request.urlopen(request, timeout=10)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            reply = json.load(response)
        if reply.get('error'):
            raise RuntimeError(str(reply['error']))
        result = reply['result']
        if isinstance(result, dict) and result.get('error'):
            raise RuntimeError(str(result['error']))
        return result

    process = None
    receipt['cold_restarts'] = []
    began = time.monotonic()
    try:
        for sock in reservations:
            sock.close()
        with (work / 'daemon.log').open('xb') as log:
            process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log,
                                       stderr=log, start_new_session=True)
        receipt['pid'] = process.pid
        save()
        deadline = time.monotonic() + 300
        while time.monotonic() < deadline:
            assert process.poll() is None, 'daemon exited: ' + str(process.returncode)
            text = (work / 'daemon.log').read_text(errors='replace')
            # Deterministic old-binary negative even if the network race happens
            # to let its second Init load correctly: a v6 request must never run
            # preliminary legacy genesis services in the first place.
            assert 'Fresh store genesis records persisted; restarting to upgrade storage' not in text, \
                'configured v6 request ran the preliminary legacy genesis service graph'
            assert '[snapshot] rejected' not in text and '[IBD] Snapshot load failed' not in text, \
                'authenticated v6 bootstrap failed; fallback IBD is not a pass'
            try:
                status = rpc('getsnapshotbootstrapstatus')
                height, tip = rpc('getblockcount'), rpc('getbestblockhash')
                installed = args.reopen_copy or ('SNAPSHOT BOOTSTRAP SUCCESS' in text and '[snapshot] loaded' in text)
                if installed and \
                        status['snapshot_bootstrap'].get('snapshot_base_height') == fixture['base_height'] and \
                        height == fixture['tip_height'] and tip == fixture['tip_hash']:
                    break
            except (OSError, ValueError, RuntimeError):
                pass
            time.sleep(.3)
        else:
            raise TimeoutError('authenticated v6 load did not reach the independently supplied fixture tip')
        assert rpc('getblockhash', [0]) == fixture['genesis']
        assert not status['snapshot_bootstrap'].get('fatal')
        accumulator = rpc('blockchain.getutreexocommitment')
        assert accumulator['verified_height'] == fixture['tip_height'] and accumulator['verified_block_hash'] == fixture['tip_hash']
        assert accumulator['retains_full_state'] == (args.profile == 'full')
        assert accumulator['commitment'] == fixture['tip_accumulator']
        if args.profile == 'compact':
            assert 'ValidationQueue started (canonical-only compact Orchard)' in text
            assert accumulator['forest_memory_bytes_estimate'] == 0 and accumulator['validation_role'] == 'compact_validator'
            if args.reopen_copy:
                assert 'Compact snapshot reopen routed to retained-proof authentication' in text
                assert '[snapshot] loaded' not in text
        assert not list((data / 'blockchain').glob('chaindb.legacy-*')), 'unexpected preliminary legacy layout/migration'
        receipt.update(bootstrap=status, accumulator=accumulator)
        save()
        for index in range(2):
            rpc('stop')
            process.wait(timeout=90)
            assert process.returncode == 0, 'pre-restart daemon did not exit cleanly'
            if receipt['cold_restarts']:
                receipt['cold_restarts'][-1]['exit'] = process.returncode
            else:
                receipt['initial_daemon_exit'] = process.returncode
            process = None
            # Existing storage must reopen without a snapshot file or IBD.
            # Ports are reused exactly as the real daemon does (SO_REUSEADDR).
            config.write_text('wallet.socket.enable=false\nlightning.oracles.enable=false\nrpcbind=127.0.0.1\n')
            log_path = work / ('cold-restart-' + str(index) + '.log')
            with log_path.open('xb') as log:
                process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log,
                                           stderr=log, start_new_session=True)
            restart = {'pid': process.pid, 'log': str(log_path)}
            receipt['cold_restarts'].append(restart)
            save()
            deadline = time.monotonic() + 180
            while time.monotonic() < deadline:
                assert process.poll() is None, 'cold restart exited: ' + str(process.returncode)
                try:
                    reopened = rpc('blockchain.getutreexocommitment')
                    reopened_status = rpc('getsnapshotbootstrapstatus')
                    if reopened['verified_height'] == fixture['tip_height'] and \
                            rpc('getbestblockhash') == fixture['tip_hash']:
                        break
                except (OSError, ValueError, RuntimeError, KeyError):
                    pass
                time.sleep(.3)
            else:
                raise TimeoutError('cold restart did not authenticate the retained selection')
            assert reopened['verified_block_hash'] == fixture['tip_hash']
            assert reopened['commitment'] == accumulator['commitment']
            assert reopened['retains_full_state'] == accumulator['retains_full_state']
            assert reopened_status['snapshot_bootstrap'].get('snapshot_base_height') == fixture['base_height']
            assert not reopened_status['snapshot_bootstrap'].get('fatal')
            text = log_path.read_text(errors='replace')
            assert '[snapshot] loaded' not in text, 'cold restart reimported a snapshot'
            assert 'Height index audit: fixed' not in text
            if args.profile == 'compact':
                assert 'Compact snapshot reopen routed to retained-proof authentication' in text
                assert 'ValidationQueue started (canonical-only compact Orchard)' in text
                assert reopened['forest_memory_bytes_estimate'] == 0
                assert reopened['validation_role'] == 'compact_validator'
                assert 'Seeded HeaderChainSelector with' not in text, 'legacy header backfill ran on compact storage'
            restart.update(accumulator=reopened, bootstrap=reopened_status)
            save()
        receipt.update(state='PASS_ACTUAL_EXECUTABLE_AUTHENTICATED_V6_BOOTSTRAP_AND_TWO_COLD_RESTARTS')
    except BaseException as error:
        receipt.update(state='FAIL', error=repr(error), traceback=traceback.format_exc())
    finally:
        if process is not None:
            if process.poll() is None:
                try:
                    receipt['stop_rpc'] = rpc('stop')
                except Exception as error:
                    receipt['stop_rpc_error'] = repr(error)
                    process.terminate()  # only this newly spawned test child
                try:
                    process.wait(timeout=90)
                except subprocess.TimeoutExpired:
                    receipt.update(state='FAIL', cleanup='child did not stop; no force-kill or automatic retry')
            receipt['daemon_exit'] = process.poll()
            if receipt['cold_restarts']:
                receipt['cold_restarts'][-1]['exit'] = process.poll()
            if receipt['state'].startswith('PASS') and process.poll() != 0:
                receipt.update(state='FAIL', error='successful import did not exit cleanly')
        receipt['elapsed_s'] = round(time.monotonic() - began, 3)
        save()
        print(json.dumps({'state': receipt['state'], 'receipt': str(path), 'error': receipt.get('error')}), flush=True)
    return 0 if receipt['state'].startswith('PASS') else 1


if __name__ == '__main__':
    raise SystemExit(main())
