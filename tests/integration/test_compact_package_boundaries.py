#!/usr/bin/env python3
"""Real signed RPC admission at package limits; canonical state stays unchanged."""
import base64
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
BINARY = Path(os.environ.get('DINEROD', ROOT / 'build/dinerod')).resolve()
BUILDER = Path(os.environ.get('COMPACT_PACKAGE_BUILDER', ROOT / 'build/compact_package_builder')).resolve()
WORK = Path(tempfile.mkdtemp(prefix='compact_packages_'))
RECEIPTS = []
ADMISSIONS = []


def require(ok, message):
    if not ok:
        raise AssertionError(message)


def size(n):
    if n < 253: return bytes([n])
    if n <= 65535: return b'\xfd' + struct.pack('<H', n)
    return b'\xfe' + struct.pack('<I', n)


class Reader:
    def __init__(self, raw): self.raw, self.at = raw, 0
    def take(self, n):
        require(0 <= n <= len(self.raw)-self.at, 'truncated transaction')
        value = self.raw[self.at:self.at+n]; self.at += n; return value
    def count(self):
        first = self.take(1)[0]
        return first if first < 253 else int.from_bytes(self.take({253: 2, 254: 4, 255: 8}[first]), 'little')
    def blob(self): return self.take(self.count())


def decode(encoded):
    raw = bytes.fromhex(encoded); r = Reader(raw); version = r.take(4)
    witness = raw[4:6] == b'\x00\x01'
    if witness: r.take(2)
    start = r.at; inputs = r.count()
    for _ in range(inputs): r.take(36); r.blob(); r.take(4)
    outputs = []
    for index in range(r.count()):
        value = int.from_bytes(r.take(8), 'little'); script = r.blob().hex()
        require(value > 0, 'fixture uses positive transparent output values')
        outputs.append({'vout': index, 'value': value, 'script': script})
    compact = int.from_bytes(version, 'little') == 6
    if compact:
        require(r.take(1) == b'\x01', 'compact fixture requires explicit fee'); r.take(8)
    envelope = raw[start:r.at]
    witness_offsets = []
    if witness:
        for _ in range(inputs):
            for _ in range(r.count()):
                length = r.count(); witness_offsets.append(r.at); r.take(length)
    bundle_offset = None
    if compact:
        length = r.count(); bundle_offset = r.at; bundle = r.take(length)
        from helpers.compact_regtest_oracle import expanded_bundle
        expanded_bundle(bundle)  # Require actual DZE1 proofs, not just v6.
    else: bundle = b''
    locktime = r.take(4); require(r.at == len(raw), 'trailing transaction bytes')
    base = version + envelope + (size(len(bundle)) + bundle if compact else b'') + locktime
    txid = hashlib.sha256(hashlib.sha256(base).digest()).digest()[::-1].hex()
    for output in outputs: output['txid'] = txid
    return {'hex': encoded, 'txid': txid, 'outputs': outputs, 'bytes': len(raw), 'compact': compact,
            'witness_offsets': witness_offsets, 'bundle_offset': bundle_offset}


def envelope(inputs, outputs):
    raw = struct.pack('<I', 2) + size(len(inputs))
    for prev in inputs:
        raw += bytes.fromhex(prev['txid'])[::-1] + struct.pack('<I', prev['vout']) + b'\x00' + b'\xff'*4
    raw += size(len(outputs))
    for value, script in outputs:
        raw += struct.pack('<Q', value) + size(len(script)) + script
    return (raw + bytes(4)).hex()


class Node:
    def __init__(self, name, seed=None):
        self.last_rpc = 0.0
        self.path = WORK / name
        if seed: shutil.copytree(seed, self.path)
        else: self.path.mkdir()
        sockets = [socket.socket() for _ in range(3)]
        try:
            for s in sockets: s.bind(('127.0.0.1', 0))
            self.rpc, p2p, wallet = [s.getsockname()[1] for s in sockets]
        finally:
            for s in sockets: s.close()
        self.log = (self.path / 'package-daemon.log').open('ab')
        self.process = subprocess.Popen([str(BINARY), '--regtest', f'--datadir={self.path}',
            f'--rpcport={self.rpc}', f'--port={p2p}', f'--wallet-socket-port={wallet}',
            '--p2p.offline=1', '--listen=0', '--utreexo=1',
            '--consensus-shielded-epoch-reset-height=1', '--consensus-shielded-spend-auth-height=2',
            '--consensus-state-commitment-height=3', '--consensus-shielded-compact-height=124'],
            stdout=self.log, stderr=self.log)
        try:
            for _ in range(240):
                require(self.process.poll() is None, f'{name}: daemon exited')
                try:
                    self.call('getblockcount'); return
                except (OSError, AssertionError, StopIteration, ValueError): time.sleep(0.25)
            raise AssertionError(f'{name}: RPC did not start')
        except BaseException:
            self.stop(); raise

    def raw(self, method, params=None):
        cookie = next(p for p in (self.path/'.cookie', self.path/'regtest/.cookie') if p.exists()).read_bytes().strip()
        req = urllib.request.Request(f'http://127.0.0.1:{self.rpc}/',
            data=json.dumps({'jsonrpc': '2.0', 'id': 1, 'method': method,
                             'params': [] if params is None else params}).encode(),
            headers={'Authorization': 'Basic ' + base64.b64encode(cookie).decode(),
                     'Content-Type': 'application/json'})
        # A 429 is returned before RPC dispatch, so retrying it cannot repeat
        # a mutation. Never retry validation errors or ambiguous disconnects.
        for attempt in range(100):
            # Stay below the daemon's 50 requests/second transport bucket.
            # Overrunning it can close a socket before urllib sends its body.
            time.sleep(max(0, 0.025 - (time.monotonic() - self.last_rpc)))
            self.last_rpc = time.monotonic()
            try: response = urllib.request.urlopen(req, timeout=600)
            except urllib.error.HTTPError as error: response = error
            with response:
                status, reply = response.status, json.load(response)
            if status != 429 or attempt == 99: return reply
            time.sleep(0.1)

    def call(self, method, params=None):
        reply = self.raw(method, params)
        require(not reply.get('error'), f'{method}: {reply.get("error")}')
        result = reply.get('result')
        require(not isinstance(result, dict) or not result.get('error'), f'{method}: {str(result)[:500]}')
        return result

    def stop(self):
        if self.process.poll() is None:
            try: self.call('stop')
            except (OSError, AssertionError, StopIteration, ValueError): self.process.terminate()
        try: code = self.process.wait(timeout=60)
        except subprocess.TimeoutExpired:
            self.process.kill(); self.process.wait(); raise AssertionError('daemon required SIGKILL')
        finally: self.log.close()
        require(code == 0, f'daemon exited with {code}')


def chain(node):
    return (node.call('getbestblockhash'), node.call('daemon.shieldedstatehash'),
            node.call('blockchain.getutreexocommitment'))


def admission(node, tx, allowed=True, reason='', submit=True):
    before, ids = chain(node), sorted(node.call('getrawmempool'))
    dry = node.call('mempool.testmempoolaccept', [tx['hex']])[0]
    require(dry['txid'] == tx['txid'], 'independent transaction identity differs from daemon')
    require(dry['allowed'] == allowed, f"unexpected admission: {dry}")
    if not allowed: require(reason.lower() in dry['reject-reason'].lower(), f'wrong rejection: {dry}')
    require(sorted(node.call('getrawmempool')) == ids and chain(node) == before, 'dry-run changed state')
    if submit:
        reply = node.raw('wallet.sendrawtransaction', [tx['hex']])
        result = reply.get('result')
        error = reply.get('error') or (result.get('error') if isinstance(result, dict) else None)
        if allowed:
            require(not error, f'real submission rejected: {reply}')
            require(sorted(node.call('getrawmempool')) == sorted(ids + [tx['txid']]), 'wrong mempool insertion')
        else:
            require(error, 'real submission admitted forbidden package')
            require(reason.lower() in str(reply).lower(), f'real rejection differs: {reply}')
            require(sorted(node.call('getrawmempool')) == ids, 'rejected package changed mempool membership')
    require(chain(node) == before, 'admission changed canonical Utreexo/shielded state')
    ADMISSIONS.append({'case': node.path.name, 'txid': tx['txid'], 'wire_bytes': tx['bytes'],
                       'compact': tx['compact'], 'allowed': allowed, 'submitted': submit,
                       'rejection': dry.get('reject-reason'),
                       'canonical_state_hash': before[1]['state_hash'],
                       'mempool_before': ids, 'mempool_after': sorted(node.call('getrawmempool'))})
    return dry


def sign(node, inputs, unsigned):
    def rpc_amount(value):
        # This legacy RPC truncates double(DIN)*1e8. Supply a floating-point
        # value that converts to the exact real prevout amount, never a
        # different integer amount (which produces an invalid Taproot sighash).
        amount = value / 100_000_000
        if int(amount * 100_000_000) != value:
            amount = math.nextafter(amount, math.inf)
        require(int(amount * 100_000_000) == value, 'RPC amount loses integer una')
        return amount
    prevouts = [{'txid': p['txid'], 'vout': p['vout'], 'scriptPubKey': p['script'],
                 'amount': rpc_amount(p['value'])} for p in inputs]
    result = node.call('wallet.signrawtransaction', [unsigned, prevouts])
    require(result['complete'], 'real wallet did not fully sign the fixture')
    return decode(result['hex'])


def spend(node, inputs, script, target=None, fanout=1, fee=1_000_000):
    total = sum(p['value'] for p in inputs) - fee
    def outputs(padding):
        balance = total - len(padding)
        require(balance > fanout, 'fixture exhausted funds')
        values = [balance // fanout] * fanout; values[0] += balance % fanout
        return [(v, script) for v in values] + [(1, b'\x6a' + bytes(n-1)) for n in padding]
    base = sign(node, inputs, envelope(inputs, outputs([])))
    if target is None: return base
    delta = target - base['bytes']; require(delta >= 10, 'fixture target too small')
    # Add bounded OP_RETURN scripts (1..80 bytes), never an invalid giant script.
    count = max(1, math.ceil(delta/89))
    while True:
        payload = delta - (len(size(fanout + count)) - len(size(fanout)))
        if 10*count <= payload <= 89*count: break
        count += 1; require(count < 10000, 'cannot construct exact fixture size')
    padding = [1]*count; extra = payload - 10*count
    for index in range(count):
        add = min(extra, 79); padding[index] += add; extra -= add
    tx = sign(node, inputs, envelope(inputs, outputs(padding)))
    require(tx['bytes'] == target, f"signed size {tx['bytes']} != target {target}")
    return tx


def fixture():
    node = Node('seed')
    try:
        address = node.call('wallet.getnewaddress', ['taproot', 'package-miner'])['address']
        node.call('generatetoaddress', [123, address])
        node.call('wallet.shield', {'amount_una': 1_000_000_000, 'fee_una': 1_000_000})
        node.call('generatetoaddress', [1, address])
        unshield_id = node.call('wallet.unshield', {'amount_una': 900_000_000})['txid']
        raw_unshield = node.call('getrawtransaction', [unshield_id, False])
        unshield = decode(raw_unshield['hex'] if isinstance(raw_unshield, dict) else raw_unshield)
        require(unshield['compact'], 'unshield fixture is not compact')
        coins = node.call('wallet.listunspent', [110, 9999999])
        coin = next(c for c in coins if c['spendable'])
        funding = {'txid': coin['txid'], 'vout': coin['vout'],
                   'value': round(coin['amount']*100_000_000), 'script': coin['scriptPubKey']}
        script = bytes.fromhex(funding['script'])
        # The fanout root itself contains a real compact shield proof, so its
        # direct descendants legitimately receive the Auth package-byte budget.
        shield, fee, count = 100_000_000, 1_000_000, 30
        remainder = funding['value'] - shield - fee
        amounts = [remainder//count]*count; amounts[0] += remainder % count
        unsigned = envelope([funding], [(value, script) for value in amounts])
        request = WORK/'builder-request.json'
        request.write_text(json.dumps({'hex': unsigned, 'shield_una': shield, 'fee_una': fee}))
        generated = json.loads(subprocess.check_output([str(BUILDER), str(request)], text=True))
        fanout = sign(node, [funding], generated['hex'])
        admission(node, fanout, submit=False)
        base_state = chain(node)
        return node.path, unshield, fanout, funding, script, base_state
    finally: node.stop()


def run_case(name, seed, original_state, function):
    node = Node(name, seed)
    try:
        require(chain(node) == original_state, 'restored fixture canonical state changed')
        result = function(node)
        RECEIPTS.append({'case': name, **result})
        print('PASS', name, json.dumps(result, sort_keys=True), flush=True)
    finally: node.stop()


def main():
    success = False
    try:
        seed, unshield, root, funding, script, original_state = fixture()

        def ancestor_bytes(node, legacy=False):
            if legacy:
                parent = spend(node, [funding], script, target=50_000)
                admission(node, parent); budget = 103_424
            else:
                require(unshield['txid'] in node.call('getrawmempool'), 'unshield did not restore canonically')
                parent = unshield; budget = 600_000
                for _ in range(6):
                    tx = spend(node, [parent['outputs'][0]], script, target=90_000)
                    admission(node, tx); parent = tx
            used = 50_000 if legacy else unshield['bytes'] + 6*90_000
            previous = parent['outputs'][0]
            for total, allowed, submit in ((budget-1, True, False), (budget+1, False, True), (budget, True, True)):
                tx = spend(node, [previous], script, target=total-used)
                admission(node, tx, allowed, 'ancestor-size-limit-exceeded', submit)
            return {'package_limit': budget, 'minus_one': 'allowed', 'exact': 'admitted', 'plus_one': 'rejected'}

        def descendant_bytes(node, legacy=False):
            parent = spend(node, [funding], script, fanout=30) if legacy else root
            count, child_size, budget = (1, 50_000, 103_424) if legacy else (6, 90_000, 600_000)
            admission(node, parent)
            for index in range(count):
                admission(node, spend(node, [parent['outputs'][index]], script, target=child_size))
            for total, allowed, submit in ((budget-1, True, False), (budget+1, False, True), (budget, True, True)):
                tx = spend(node, [parent['outputs'][count]], script, target=total-count*child_size)
                admission(node, tx, allowed, 'descendant-size-limit-exceeded', submit)
            return {'descendant_limit': budget, 'root_excluded_from_byte_sum': True, 'plus_one': 'rejected'}

        def shared_ancestor(node):
            admission(node, root)
            tips = []
            for index in range(2):
                previous = root['outputs'][index]
                for _ in range(3):
                    tx = spend(node, [previous], script, target=90_000)
                    admission(node, tx); previous = tx['outputs'][0]
                tips.append(previous)
            used = root['bytes'] + 540_000
            for total, allowed, submit in ((599_999, True, False), (600_001, False, True), (600_000, True, True)):
                tx = spend(node, tips, script, target=total-used)
                admission(node, tx, allowed, 'ancestor-size-limit-exceeded', submit)
            return {'package_limit': 600_000, 'shared_ancestor_counted_once': True, 'plus_one': 'rejected'}

        def ancestor_count(node):
            parent = unshield
            for _ in range(24):
                tx = spend(node, [parent['outputs'][0]], script)
                admission(node, tx); parent = tx
            exact = spend(node, [parent['outputs'][0]], script)
            admission(node, exact)
            admission(node, spend(node, [exact['outputs'][0]], script), False, 'too-many-ancestors')
            return {'ancestors_excluding_candidate': 25, 'plus_one': 'rejected'}

        def descendant_count(node):
            admission(node, root)
            for index in range(25):
                admission(node, spend(node, [root['outputs'][index]], script))
            admission(node, spend(node, [root['outputs'][25]], script), False, 'too-many-descendants')
            return {'descendants_excluding_parent': 25, 'plus_one': 'rejected'}

        def transaction_bytes(node):
            previous = unshield['outputs'][0]
            for total, allowed, submit in ((99_999, True, False), (100_001, False, True), (100_000, True, True)):
                admission(node, spend(node, [previous], script, target=total), allowed, 'transaction-size-limit-exceeded', submit)
            return {'transparent_limit': 100_000, 'shielded_ancestor_does_not_raise_tx_limit': True}

        def invalid_controls(node):
            valid = spend(node, [unshield['outputs'][0]], script)
            admission(node, valid, submit=False)
            bad = bytearray.fromhex(valid['hex'])
            bad[valid['witness_offsets'][0]] ^= 1
            admission(node, decode(bad.hex()), False, 'txn-validation-failed')
            # A rejected signature must not reserve the real input.
            admission(node, valid)

            # Corrupt only a scalar at the end of the shield's output proof,
            # retaining the canonical framing. Re-sign the transparent input
            # so failure reaches shielded validation, not Taproot validation.
            bad = bytearray.fromhex(root['hex'])
            r = Reader(bad); r.at = root['bundle_offset']; r.take(8)
            require(r.count() == 0 and r.count() == 1, 'expected shield-only root')
            r.take(65); r.blob(); proof = r.blob()
            require(proof[:4] == b'DZE1', 'expected compact proof')
            bad[r.at-1] ^= 1
            # Strip the now-stale transparent witness before wallet re-signing.
            unsigned = decode(bad.hex())
            require(len(unsigned['witness_offsets']) == 1, 'expected one transparent signature')
            require(bad[unsigned['witness_offsets'][0]-1] == 64, 'expected a 64-byte signature')
            start = unsigned['witness_offsets'][0]-2
            end = unsigned['witness_offsets'][0]+64
            bad[start:end] = b'\x00'
            invalid = sign(node, [funding], bad.hex())
            admission(node, invalid, False, 'shielded')
            admission(node, root)
            return {'bad_signature_rejected': True, 'bad_compact_proof_rejected': True,
                    'valid_transactions_admitted_after_rejections': True}

        for name, fn in [('auth-ancestor-bytes', ancestor_bytes),
                         ('legacy-ancestor-bytes', lambda n: ancestor_bytes(n, True)),
                         ('auth-descendant-bytes', descendant_bytes),
                         ('legacy-descendant-bytes', lambda n: descendant_bytes(n, True)),
                         ('shared-ancestor-bytes', shared_ancestor),
                         ('ancestor-count', ancestor_count), ('descendant-count', descendant_count),
                         ('transparent-transaction-bytes', transaction_bytes),
                         ('invalid-controls', invalid_controls)]:
            run_case(name, seed, original_state, fn)
        success = True
    finally:
        destination = Path(os.environ.get('COMPACT_PACKAGE_EVIDENCE_DIR', WORK/'evidence'))
        destination.mkdir(parents=True, exist_ok=True)
        (destination/'results.json').write_text(json.dumps(
            {'success': success, 'cases': RECEIPTS, 'admissions': ADMISSIONS}, indent=2)+'\n')
        for log in WORK.glob('*/package-daemon.log'):
            shutil.copyfile(log, destination/(log.parent.name+'.log'))
        print('Package evidence:', destination, flush=True)
        if success and destination != WORK/'evidence': shutil.rmtree(WORK)
        elif not success: print('Retained test data:', WORK, flush=True)


if __name__ == '__main__':
    main()
