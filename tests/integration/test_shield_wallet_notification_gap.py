#!/usr/bin/env python3
"""Keep WalletWorker paused after a real shield confirms, then shield again.
The stale wallet row and empty mempool are asserted while the barrier is held.
No sleeps decide when selection occurs: explicit worker entry/release files do.
"""
import base64
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
DAEMON = Path(os.environ.get('DINEROD', ROOT / 'build/dinerod')).resolve()
WORK = Path(tempfile.mkdtemp(prefix='dinero_shield_selection_')).resolve()
BARRIER = WORK / 'wallet-connect'
process = None
print(f'Evidence: {WORK}', flush=True)


def wait(check, seconds=60):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError('daemon exited; see ' + str(WORK / 'daemon.log'))
        if check():
            return
        time.sleep(0.02)
    raise TimeoutError('condition did not become true')


def rpc(method, params=None):
    cookie = (WORK / 'node/.cookie').read_bytes().strip()
    request = urllib.request.Request(
        f'http://127.0.0.1:{rpcport}/',
        json.dumps({'jsonrpc': '2.0', 'id': 1, 'method': method,
                    'params': [] if params is None else params}).encode(),
        {'Content-Type': 'application/json',
         'Authorization': 'Basic ' + base64.b64encode(cookie).decode()})
    with urllib.request.urlopen(request, timeout=180 if method in ('wallet.shield', 'wallet.unshield') else 20) as response:
        reply = json.load(response)
    if reply.get('error'):
        raise RuntimeError(f'{method}: {reply["error"]}')
    return reply['result']


def ready():
    try:
        return rpc('getblockcount') == 0
    except (OSError, ValueError, RuntimeError):
        return False


def inputs(tx):
    return {(i['prevout_txid'], i['prevout_vout']) for i in tx['inputs']}


def main():
    global process, rpcport
    socks = [socket.socket() for _ in range(3)]
    for sock in socks:
        sock.bind(('127.0.0.1', 0))
    rpcport, p2pport, walletport = [sock.getsockname()[1] for sock in socks]
    for sock in socks:
        sock.close()
    env = dict(os.environ, DINERO_TEST_WALLET_CONNECT_PAUSE_HEIGHT='106',
               DINERO_TEST_WALLET_CONNECT_PAUSE_FILE=str(BARRIER))
    with (WORK / 'daemon.log').open('w') as log:
        process = subprocess.Popen([
            str(DAEMON), '--regtest', f'--datadir={WORK / "node"}',
            f'--rpcport={rpcport}', f'--port={p2pport}',
            f'--wallet-socket-port={walletport}', '--listen=0', '--utreexo=1',
            '--p2p.offline=1', '--consensus-shielded-epoch-reset-height=1',
            '--consensus-shielded-spend-auth-height=2', '--consensus-state-commitment-height=3',
        ], env=env, stdout=log, stderr=subprocess.STDOUT)
    wait(ready)
    addr = rpc('wallet.getnewaddress')
    if isinstance(addr, dict):
        addr = addr['address']
    rpc('generatetoaddress', [105, addr])
    wait(lambda: len(rpc('wallet.listunspent')) >= 3)
    first = rpc('wallet.shield', {'amount_una': 10000000, 'fee_una': 1000000})['txid']
    rpc('generatetoaddress', [1, addr])
    block = rpc('getbestblockhash')
    wait(lambda: Path(str(BARRIER) + '.entered').exists())
    assert first in rpc('getblock', [block, 1])['tx']
    assert first not in rpc('getrawmempool')
    first_inputs = inputs(rpc('gettransaction', [first]))
    stale = {(u['txid'], u['vout']) for u in rpc('wallet.listunspent')}
    assert first_inputs <= stale, 'the actual wallet index must still be stale'
    assert not Path(str(BARRIER) + '.exited').exists()
    print('PASS confirmed spend removed from mempool while wallet row remains unspent', flush=True)

    params = {'amount_una': 20000000}
    if '--auto-fee' not in sys.argv:
        params['fee_una'] = 1000000
    second = rpc('wallet.shield', params)['txid']
    assert second in rpc('getrawmempool')
    assert not Path(str(BARRIER) + '.exited').exists(), 'selection must succeed before wallet catches up'
    rpc('generatetoaddress', [1, addr])
    block = rpc('getbestblockhash')
    assert second in rpc('getblock', [block, 1])['tx']
    assert inputs(rpc('gettransaction', [second])).isdisjoint(first_inputs)
    print('PASS second shield uses live coins, admits and mines before wallet notification resumes', flush=True)
    Path(str(BARRIER) + '.release').touch()
    wait(lambda: len([n for n in rpc('wallet.listshielded')['notes'] if n['confirmed'] and not n['spent']]) == 2)
    unshield = rpc('wallet.unshield', {'amount_una': 10000000, 'fee_una': 1000000})['txid']
    rpc('generatetoaddress', [1, addr])
    block = rpc('getbestblockhash')
    assert unshield in rpc('getblock', [block, 1])['tx']
    proofs = rpc('blockchain.getutxoproofs_batch', [[{'txid': unshield, 'vout': 0}]])
    assert proofs['successful'] == 1 and proofs['failed'] == 0
    proof = proofs['proofs'][0]
    verified = rpc('blockchain.verifyutxoproofs_batch', [[{k: proof[k] for k in ['txid', 'vout', 'proof']}]])
    assert verified['valid'] == 1 and verified['invalid'] == 0
    print('PASS unshield output Utreexo proof verifies', flush=True)
    rpc('stop')
    process.wait(timeout=45)
    assert process.returncode == 0


try:
    main()
finally:
    Path(str(BARRIER) + '.release').touch()
    if process is not None and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=45)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=10)
