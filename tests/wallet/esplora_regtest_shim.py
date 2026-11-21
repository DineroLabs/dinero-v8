#!/usr/bin/env python3
"""A minimal Esplora-compatible server on top of a regtest bitcoind, for tests.

  esplora_regtest_shim.py <listen port> <bitcoind rpc port> <rpc user> <rpc password> [--hide] [--bad-proofs]

Serves the endpoints the light Bitcoin client uses (btc_esplora.h), answering
from bitcoind over JSON-RPC (bitcoind needs -txindex=1). With --hide it behaves
like a server that hides everything about scripts and spends: an empty history
and "unspent" for every output. With --bad-proofs every Merkle proof it serves is
wrong in one sibling. Regtest only: it scans every block.
"""
import base64, hashlib, http.server, json, sys, urllib.request

LISTEN, RPC_PORT, USER, PASSWORD = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3], sys.argv[4]
HIDE = '--hide' in sys.argv
BAD_PROOFS = '--bad-proofs' in sys.argv
AUTH = 'Basic ' + base64.b64encode(f'{USER}:{PASSWORD}'.encode()).decode()
blocks = {}  # hash -> verbose block (immutable once mined)


class RpcError(Exception):
    pass


def rpc(method, *params):
    body = json.dumps({'jsonrpc': '1.0', 'id': 1, 'method': method, 'params': list(params)}).encode()
    request = urllib.request.Request(f'http://127.0.0.1:{RPC_PORT}/', body, {'Authorization': AUTH, 'Content-Type': 'text/plain'})
    try:
        reply = json.load(urllib.request.urlopen(request, timeout=30))
    except urllib.error.HTTPError as e:
        reply = json.load(e)
    if reply.get('error'):
        raise RpcError(json.dumps(reply['error']))
    return reply['result']


def sha256d(b):
    return hashlib.sha256(hashlib.sha256(b).digest()).digest()


def chain_blocks():
    """Verbose blocks of the best chain, oldest first."""
    out = []
    for height in range(rpc('getblockcount') + 1):
        h = rpc('getblockhash', height)
        if h not in blocks:
            blocks[h] = rpc('getblock', h, 2)
        out.append(blocks[h])
    return out


def all_txs():
    """(tx, status) for every transaction on the best chain and in the mempool."""
    for block in chain_blocks():
        status = {'confirmed': True, 'block_height': block['height'], 'block_hash': block['hash'], 'block_time': block['time']}
        for tx in block['tx']:
            yield tx, status
    for txid in rpc('getrawmempool'):
        try:
            yield rpc('getrawtransaction', txid, True), {'confirmed': False}
        except RpcError:
            pass


def status_of(txid):
    try:
        tx = rpc('getrawtransaction', txid, True)
    except RpcError:
        return {'confirmed': False}  # as the real servers answer for a transaction they do not know
    if not tx.get('blockhash') or tx.get('confirmations', 0) < 1:
        return {'confirmed': False}
    header = rpc('getblockheader', tx['blockhash'])
    return {'confirmed': True, 'block_height': header['height'], 'block_hash': tx['blockhash'], 'block_time': header['time']}


def merkle_proof(txid):
    tx = rpc('getrawtransaction', txid, True)
    block = rpc('getblock', tx['blockhash'], 1)
    level = [bytes.fromhex(t)[::-1] for t in block['tx']]
    index = block['tx'].index(txid)
    pos, siblings = index, []
    while len(level) > 1:
        if len(level) % 2:
            level.append(level[-1])
        siblings.append(level[index ^ 1][::-1].hex())
        level = [sha256d(level[i] + level[i + 1]) for i in range(0, len(level), 2)]
        index //= 2
    if BAD_PROOFS:
        if siblings:
            siblings[0] = ('f' if siblings[0][0] != 'f' else 'e') + siblings[0][1:]
        else:
            pos = 1  # a single-transaction block has no sibling to spoil
    return {'block_height': block['height'], 'merkle': siblings, 'pos': pos}


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, status, body):
        data = body if isinstance(body, bytes) else (body if isinstance(body, str) else json.dumps(body)).encode()
        self.send_response(status)
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        try:
            parts = self.path.strip('/').split('/')
            if self.path == '/blocks/tip/height':
                return self.reply(200, str(rpc('getblockcount')))
            if parts[0] == 'block-height':
                return self.reply(200, rpc('getblockhash', int(parts[1])))
            if parts[0] == 'block' and parts[2] == 'header':
                return self.reply(200, rpc('getblockheader', parts[1], False))
            if parts[0] == 'block' and parts[2] == 'txid':
                return self.reply(200, rpc('getblock', parts[1], 1)['tx'][int(parts[3])])
            if parts[0] == 'fee-estimates':
                return self.reply(200, {'1': 2.0, '6': 1.0, '144': 1.0})
            if parts[0] == 'scripthash' and parts[2] == 'txs':
                found = []
                if not HIDE:
                    for tx, status in all_txs():
                        if any(hashlib.sha256(bytes.fromhex(o['scriptPubKey']['hex'])).hexdigest() == parts[1] for o in tx['vout']):
                            found.append({'txid': tx['txid'], 'status': status})
                return self.reply(200, list(reversed(found)))
            if parts[0] == 'tx' and len(parts) == 3 and parts[2] == 'hex':
                return self.reply(200, rpc('getrawtransaction', parts[1], False))
            if parts[0] == 'tx' and len(parts) == 3 and parts[2] == 'status':
                return self.reply(200, status_of(parts[1]))
            if parts[0] == 'tx' and len(parts) == 3 and parts[2] == 'merkle-proof':
                return self.reply(200, merkle_proof(parts[1]))
            if parts[0] == 'tx' and len(parts) == 4 and parts[2] == 'outspend':
                if not HIDE:
                    for tx, status in all_txs():
                        for n, vin in enumerate(tx['vin']):
                            if vin.get('txid') == parts[1] and vin.get('vout') == int(parts[3]):
                                return self.reply(200, {'spent': True, 'txid': tx['txid'], 'vin': n, 'status': status})
                return self.reply(200, {'spent': False})
            return self.reply(404, 'not found')
        except RpcError as e:
            return self.reply(404, str(e))
        except Exception as e:  # a test server: say what went wrong
            return self.reply(500, repr(e))

    def do_POST(self):
        try:
            if self.path != '/tx':
                return self.reply(404, 'not found')
            raw = self.rfile.read(int(self.headers.get('Content-Length', '0'))).decode().strip()
            return self.reply(200, rpc('sendrawtransaction', raw))
        except RpcError as e:
            return self.reply(400, 'sendrawtransaction RPC error: ' + str(e))
        except Exception as e:
            return self.reply(500, repr(e))


http.server.ThreadingHTTPServer(('127.0.0.1', LISTEN), Handler).serve_forever()
