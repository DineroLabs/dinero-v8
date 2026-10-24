#!/usr/bin/env python3
"""Actual dinerod: datadir aliases migrate/reopen; inner DB aliases still refuse.

Only fresh, isolated offline stores are used. No activation override, mocked
database, installed node, or existing peer is needed. Logs and receipts are
always retained, including for the old-binary negative control.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
import traceback
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dinerod', required=True)
    parser.add_argument('--work-parent')
    args = parser.parse_args()
    binary = Path(args.dinerod).resolve(strict=True)
    work = Path(tempfile.mkdtemp(prefix='orchard-datadir-alias-', dir=args.work_parent)).resolve()
    print('Retained executable regression evidence: ' + str(work), flush=True)
    real = work / 'real'
    real.mkdir()
    alias = work / 'alias'
    alias.symlink_to(real, target_is_directory=True)
    receipt = {'state': 'RUNNING', 'binary': str(binary),
               'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
               'scope': 'actual main.cpp offline mainnet-parameter genesis storage migration/reopen, no peers; not activation or power loss',
               'checks': [], 'processes': []}
    nodes = []

    def save():
        (work / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')

    class Node:
        def __init__(self, name, datadir):
            self.datadir = datadir  # Deliberately do NOT resolve the tested alias.
            self.datadir.mkdir(parents=True, exist_ok=True)
            self.process = None
            self.starts = 0
            self.config = work / (name + '.conf')
            self.config.write_text('p2p.offline=true\nwallet.socket.enable=false\n'
                                   'lightning.oracles.enable=false\nrpcbind=127.0.0.1\n')
            self.name = name
            nodes.append(self)

        def start(self, path=None):
            self.starts += 1
            sockets = [socket.socket() for _ in range(3)]
            try:
                for sock in sockets:
                    sock.bind(('127.0.0.1', 0))
                self.ports = [sock.getsockname()[1] for sock in sockets]
            finally:
                for sock in sockets:
                    sock.close()
            # Unmodified mainnet parameters schedule Orchard, while ordinary
            # regtest intentionally leaves it dormant. Never enable public
            # networking: these isolated genesis-only stores need no peers.
            self.command = [str(binary), '--datadir=' + str(path or self.datadir),
                            '--conf=' + str(self.config), '--rpcport=' + str(self.ports[0]),
                            '--port=' + str(self.ports[1]), '--wallet-socket-port=' + str(self.ports[2]),
                            '--listen=0', '--p2p.offline=1', '--dnsseed=0', '--fixedseeds=0',
                            '--discover=0', '--listenonion=0', '--utreexo=1', '--utreexo-bridge=1']
            self.log = work / (self.name + '-' + str(self.starts) + '.log')
            with self.log.open('xb') as output:
                self.process = subprocess.Popen(self.command, stdin=subprocess.DEVNULL,
                                                stdout=output, stderr=output, start_new_session=True)
            self.observation = {'pid': self.process.pid, 'command': self.command, 'log': str(self.log)}
            receipt['processes'].append(self.observation)
            save()
            deadline = time.monotonic() + 90
            while time.monotonic() < deadline:
                assert self.process.poll() is None, 'startup exited; see ' + str(self.log)
                # The preliminary genesis bootstrap can briefly open RPC.
                # Only the second Init's flushed Start completion is the real
                # instance. main's final banner is buffered when stdout is a
                # file, so it cannot be used as a readiness condition.
                text = self.log.read_text(errors='replace')
                inits = text.split('[DaemonApp] Initializing services...')
                if len(inits) < 3 or '[DaemonApp] All services started successfully' not in inits[-1]:
                    time.sleep(.1)
                    continue
                try:
                    return self.rpc('getblockcount')
                except (OSError, ValueError):
                    time.sleep(.1)
            raise TimeoutError('RPC readiness: ' + str(self.log))

        def rpc(self, method, params=None):
            cookie = (self.datadir / '.cookie').read_bytes().strip()
            request = urllib.request.Request('http://127.0.0.1:' + str(self.ports[0]) + '/',
                json.dumps({'jsonrpc': '2.0', 'id': 1, 'method': method,
                            'params': [] if params is None else params}).encode(),
                {'Content-Type': 'application/json',
                 'Authorization': 'Basic ' + base64.b64encode(cookie).decode()})
            with urllib.request.urlopen(request, timeout=10) as response:
                reply = json.load(response)
            assert not reply.get('error'), reply.get('error')
            result = reply['result']
            assert not isinstance(result, dict) or not result.get('error'), result
            return result

        def stop(self):
            self.rpc('stop')
            self.observation['exit'] = self.process.wait(timeout=60)
            save()
            assert self.observation['exit'] == 0, 'unclean shutdown: ' + str(self.log)

        def state(self):
            return {'height': self.rpc('getblockcount'), 'tip': self.rpc('getbestblockhash'),
                    'shielded': self.rpc('daemon.shieldedstatehash')['state_hash']}

    def separated(datadir):
        options = sorted((datadir / 'blockchain' / 'chaindb').glob('OPTIONS-*'))
        return bool(options) and 'shielded_state_v1' in options[-1].read_text()

    def check(name, **fields):
        receipt['checks'].append({'name': name, **fields})
        save()
        print('PASS ' + name, flush=True)

    save()
    try:
        # The resolved control executes first, so a pre-fix binary's retained
        # receipt proves that its later alias failure is not generic startup.
        for mode in ('resolved', 'alias'):
            datadir = (real if mode == 'resolved' else alias) / mode / 'node'
            node = Node(mode, datadir)
            assert node.start() == 0
            assert separated(datadir), mode + ': actual executable did not migrate the legacy genesis store'
            assert not node.rpc('getnetworkinfo')['storage_upgrade_refused']
            assert 'Shielded-state storage upgraded' in node.log.read_text()
            legacy = datadir / 'blockchain' / 'chaindb.legacy-v1'
            assert legacy.is_dir(), 'rollback store missing'
            rollback_current = hashlib.sha256((legacy / 'CURRENT').read_bytes()).hexdigest()
            before = node.state()
            # A second *real executable* through the other spelling must hit
            # the same datadir lock, without disturbing the active node.
            other = alias / mode / 'node' if mode == 'resolved' else datadir.resolve()
            contender_log = work / (mode + '-lock-contender.log')
            command = [arg if not arg.startswith('--datadir=') else '--datadir=' + str(other)
                       for arg in node.command]
            with contender_log.open('xb') as output:
                contender = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=output,
                                           stderr=output, timeout=30)
            assert contender.returncode != 0 and 'already in use' in contender_log.read_text()
            assert node.state() == before
            node.stop()
            for spelling in (other, datadir):
                assert node.start(spelling) == before['height']
                assert node.state() == before and separated(datadir)
                assert not node.rpc('getnetworkinfo')['storage_upgrade_refused']
                assert 'Shielded-state storage upgraded' not in node.log.read_text()
                node.stop()
            assert hashlib.sha256((legacy / 'CURRENT').read_bytes()).hexdigest() == rollback_current
            assert not list((datadir / 'blockchain').glob('chaindb.upgrade-*'))
            check(mode + '-migration-lock-and-two-cold-reopens', state=before, rollback_current=rollback_current)

        # Normalize ONLY the operator's root. A symlink to the database, or
        # to blockchain inside that root, must still reach the unchanged
        # strict migration identity guard and leave the node unupgraded.
        source = real / 'resolved' / 'node' / 'blockchain' / 'chaindb.legacy-v1'
        for component in ('chaindb', 'blockchain'):
            node = Node('refused-' + component, alias / ('refused-' + component) / 'node')
            if component == 'chaindb':
                target = node.datadir / 'db-target'
                shutil.copytree(source, target)
                (node.datadir / 'blockchain').mkdir()
                (node.datadir / 'blockchain' / 'chaindb').symlink_to(target.resolve(), target_is_directory=True)
            else:
                target = node.datadir / 'blockchain-target'
                shutil.copytree(source, target / 'chaindb')
                (node.datadir / 'blockchain').symlink_to(target.resolve(), target_is_directory=True)
            assert node.start() == 0
            assert node.rpc('getnetworkinfo')['storage_upgrade_refused'] is True
            assert not separated(node.datadir)
            assert 'symlink in database path' in node.log.read_text()
            node.stop()
            assert not list((node.datadir / 'blockchain').glob('chaindb.upgrade-*'))
            assert not (node.datadir / 'blockchain' / 'chaindb.legacy-v1').exists()
            check(component + '-symlink-still-refused')
        receipt['state'] = 'PASS'
    except BaseException as error:
        receipt.update(state='FAIL', error=repr(error), traceback=traceback.format_exc())
    finally:
        for node in nodes:
            if node.process is not None and node.process.poll() is None:
                node.process.terminate()  # Only this test's freshly spawned child.
                try:
                    node.process.wait(timeout=60)
                except subprocess.TimeoutExpired:
                    receipt.update(state='FAIL', cleanup='test child did not stop; no automatic retry')
                node.observation['cleanup_exit'] = node.process.poll()
        save()
    return 0 if receipt['state'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
