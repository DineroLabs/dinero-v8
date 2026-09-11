#!/usr/bin/env python3
"""Read-only canonical-chain lock audit. Cookie stays local; outputs are public IDs.

Scans from a pinned tip, resolves every input creation height, and reports
transactions that the proposed contextual rules would reject. No wallet RPCs
other than public raw-transaction decoding are used. Incomplete scans fail.
"""
import argparse, base64, http.client, json, pathlib, statistics, time
p = argparse.ArgumentParser()
p.add_argument('--cookie', required=True)
p.add_argument('--port', type=int, default=20998)
p.add_argument('--output', required=True)
a = p.parse_args()
auth = base64.b64encode(pathlib.Path(a.cookie).read_bytes().strip()).decode()
conn = http.client.HTTPConnection('127.0.0.1', a.port, timeout=60)
def rpc(method, params):
    time.sleep(0.05)  # Stay below the node's 50 requests/second shared RPC budget.
    conn.request('POST', '/', json.dumps({'jsonrpc':'2.0','id':1,'method':method,'params':params}), {'Authorization':'Basic '+auth,'Content-Type':'application/json'})
    response = conn.getresponse()
    data = json.loads(response.read())
    if response.status != 200 or data.get('error'): raise RuntimeError(method+' failed: '+str(data.get('error')))
    value = data.get('result', data)
    if isinstance(value, dict) and value.get('error'): raise RuntimeError(method+' failed')
    return value
start = rpc('getblockchaininfo', [])
height = start['blocks']; tip = start['bestblockhash']; current = tip
times = {}; creation = {}; noncoinbase = []
for expected in range(height, -1, -1):
    try: block = rpc('getblock', [current, 1])
    except Exception as error: raise RuntimeError('audit incomplete at height '+str(expected)+': '+str(error)) from error
    if block['hash'] != current or block['height'] != expected: raise RuntimeError('inconsistent ancestry')
    times[expected] = block['time']
    for index, txid in enumerate(block['tx']):
        creation[txid] = expected
        if index: noncoinbase.append((expected, txid))
    current = block['previousblockhash']
    if expected % 2000 == 0: print('scanned_to', expected, flush=True)
def mtp(h):
    samples = [times[x] for x in range(max(0,h-10), h+1)]
    return sorted(samples)[len(samples)//2]
violations = []; constrained = 0
for h, txid in noncoinbase:
    tx = rpc('wallet.getrawtransaction', [txid, True])
    inputs = tx['vin']; lock = tx.get('locktime', 0)
    reasons = []
    if lock and any(x['sequence'] != 0xffffffff for x in inputs):
        constrained += 1
        if lock >= (h if lock < 500000000 else mtp(h-1)): reasons.append('absolute')
    if tx['version'] >= 2:
        for vin in inputs:
            seq = vin['sequence']
            if seq & 0x80000000: continue
            constrained += 1
            origin = creation[vin['txid']]
            delay = seq & 0xffff
            if seq & 0x400000:
                if mtp(h-1) < mtp(max(0, origin-1)) + delay*512: reasons.append('relative-time')
            elif h < origin+delay: reasons.append('relative-height')
    if reasons: violations.append({'height':h,'txid':txid,'reasons':sorted(set(reasons))})
# Tip may advance, but the scanned base must still be canonical.
if rpc('getblockhash', [height]) != tip: raise RuntimeError('pinned tip reorganized; repeat audit')
result = {'complete':True,'tip_height':height,'tip_hash':tip,'blocks':height+1,
          'noncoinbase_transactions':len(noncoinbase),'constraints_examined':constrained,'violations':violations}
pathlib.Path(a.output).write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result),flush=True)
