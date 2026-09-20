#!/usr/bin/env python3
"""Real daemon marker -> offline migration -> READY startup/restart.

Ordinary empty-shielded regtest deliberately isolates storage semantics from
proof generation and activation. Nonempty compact/timing qualification is a
separate gate. Only this test's generated, stopped datadirs are migrated.
"""
import base64
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

DINEROD = Path(os.environ['DINEROD']).resolve()
DRIVER = Path(os.environ['MIGRATION_TEST_DRIVER']).resolve()
WORK = Path(tempfile.mkdtemp(prefix='dinero_marker_daemon_')).resolve()
print(f'Evidence: {WORK}', flush=True)
processes = []


class Node:
    def __init__(self, name, datadir):
        self.datadir = datadir
        self.log = WORK / (name + '.log')
        sockets = [socket.socket() for _ in range(3)]
        for sock in sockets:
            sock.bind(('127.0.0.1', 0))
        ports = [sock.getsockname()[1] for sock in sockets]
        for sock in sockets:
            sock.close()
        self.rpcport = ports[0]
        with self.log.open('w') as output:
            self.process = subprocess.Popen([
                str(DINEROD), '--regtest', f'--datadir={datadir}',
                f'--rpcport={ports[0]}', f'--port={ports[1]}',
                f'--wallet-socket-port={ports[2]}', '--listen=0',
            ], stdout=output, stderr=subprocess.STDOUT)
        processes.append(self)

    def rpc(self, method, params=None):
        cookie = (self.datadir / '.cookie').read_bytes().strip()
        request = urllib.request.Request(
            f'http://127.0.0.1:{self.rpcport}/',
            json.dumps({'jsonrpc': '2.0', 'id': 1, 'method': method,
                        'params': [] if params is None else params}).encode(),
            {'Content-Type': 'application/json',
             'Authorization': 'Basic ' + base64.b64encode(cookie).decode()})
        with urllib.request.urlopen(request, timeout=15) as response:
            reply = json.load(response)
        if reply.get('error'):
            raise RuntimeError(reply['error'])
        return reply['result']

    def ready(self):
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError(f'Unexpected startup failure: {self.log}')
            try:
                return self.rpc('getblockcount')
            except (OSError, ValueError, RuntimeError):
                time.sleep(0.1)
        raise TimeoutError(f'RPC readiness: {self.log}')

    def stop(self):
        if self.process.poll() is None:
            self.rpc('stop')
        self.process.wait(timeout=45)
        assert self.process.returncode == 0, self.log

    def state(self):
        return (self.rpc('getblockcount'), self.rpc('getbestblockhash'),
                self.rpc('daemon.shieldedstatehash')['state_hash'])


def driver(*args):
    result = subprocess.run([str(DRIVER), *map(str, args)], capture_output=True,
                            text=True, timeout=90)
    assert result.returncode == 0, result.stdout + result.stderr
    return result.stdout


def main():
    original, candidate = WORK / 'original', WORK / 'candidate'
    original.mkdir()
    node = Node('original', original)
    assert node.ready() == 0
    address = node.rpc('wallet.getnewaddress')
    if isinstance(address, dict):
        address = address['address']
    node.rpc('generatetoaddress', [10, address])
    before = node.state()
    assert before[0] == 10
    node.stop()
    shutil.copytree(original, candidate)
    assert driver('--daemon-copy', original, candidate).strip() == 'READY'
    print('PASS daemon-produced marker accepted by migration', flush=True)
    node = Node('candidate', candidate)
    assert node.ready() == 10
    assert node.state() == before
    node.rpc('generatetoaddress', [1, address])
    advanced = node.state()
    assert advanced[0] == 11
    node.stop()
    node = Node('restart', candidate)
    assert node.ready() == 11
    assert node.state() == advanced
    node.stop()
    print('PASS READY startup, connect and restart preserve state', flush=True)
    for field in ['root', 'tree_size', 'nullifier_count', 'height',
                  'duplicate_nullifier', 'future_nullifier']:
        damaged = WORK / ('damaged-' + field)
        shutil.copytree(candidate, damaged)
        driver('--damage-daemon-marker', damaged, field)
        node = Node('refused-' + field, damaged)
        node.process.wait(timeout=60)
        assert node.process.returncode != 0, f'Accepted corrupt {field}'
        assert 'READY shielded state refused:' in node.log.read_text(), node.log
        print(f'PASS corrupt {field} refused at READY state check', flush=True)


try:
    main()
finally:
    # Cleanup owns only child processes; never mask the original exception.
    for node in processes:
        if node.process.poll() is None:
            node.process.terminate()
            try:
                node.process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                node.process.kill()
                node.process.wait(timeout=10)
