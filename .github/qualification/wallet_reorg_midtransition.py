#!/usr/bin/env python3
"""Real-process regression for chainstate stealing the wallet transaction owner."""
import base64
import concurrent.futures
import json
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

ROOT = Path(tempfile.mkdtemp(prefix='wallet-reorg-delivery-')).resolve()
DATA = ROOT / 'node'
LOG = ROOT / 'daemon.log'
PAUSE = ROOT / 'after-disconnect'
WALLET_PAUSE = ROOT / 'before-wallet-connect'
DAEMON = Path(os.environ['DINEROD']).resolve()
CRASH = '--crash-pending-connect' in sys.argv
PROCESS = None
PORT = None


def require(value, message):
    if not value:
        raise AssertionError(message)


def wait(check, message, seconds=30):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if PROCESS is not None and PROCESS.poll() is not None:
            raise RuntimeError(f'daemon exited: {PROCESS.returncode}')
        if check():
            return
        time.sleep(.02)
    raise TimeoutError(message)


def rpc(method, params=None, timeout=10):
    cookie = (DATA / '.cookie').read_bytes().strip()
    request = urllib.request.Request(f'http://127.0.0.1:{PORT}/',
        json.dumps({'jsonrpc':'2.0','id':1,'method':method,
                    'params':[] if params is None else params}).encode(),
        {'Content-Type':'application/json', 'Authorization':'Basic '+base64.b64encode(cookie).decode()})
    try:
        response = urllib.request.urlopen(request, timeout=timeout)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        result = json.load(response)
    if result.get('error'):
        raise RuntimeError(f'{method}: {result["error"]}')
    return result['result']


def ports():
    sockets = [socket.socket() for _ in range(3)]
    try:
        for item in sockets:
            item.bind(('127.0.0.1', 0))
        return [item.getsockname()[1] for item in sockets]
    finally:
        for item in sockets:
            item.close()


def start():
    global PROCESS, PORT
    PORT, p2p, wallet = ports()
    env = dict(os.environ, DINERO_TEST_REORG_AFTER_DISCONNECT_PAUSE_FILE=str(PAUSE))
    if CRASH:
        env.update(DINERO_TEST_WALLET_CONNECT_PAUSE_HEIGHT='1',
                   DINERO_TEST_WALLET_CONNECT_PAUSE_FILE=str(WALLET_PAUSE))
    with LOG.open('a') as log:
        PROCESS = subprocess.Popen([str(DAEMON),'--regtest',f'--datadir={DATA}',
            f'--rpcport={PORT}',f'--port={p2p}',f'--wallet-socket-port={wallet}',
            '--listen=0','--utreexo=1','--p2p.offline=1'], env=env,
            stdout=log,stderr=subprocess.STDOUT)
    def ready():
        try:
            return isinstance(rpc('getblockcount',timeout=1),int)
        except (OSError,ValueError,RuntimeError):
            return False
    wait(ready,'RPC startup',120)


def stop():
    if PROCESS is None or PROCESS.poll() is not None:
        return
    try:
        rpc('stop')
    except (OSError,ValueError,RuntimeError):
        PROCESS.terminate()
    try:
        PROCESS.wait(timeout=45)
    except subprocess.TimeoutExpired:
        PROCESS.kill();PROCESS.wait(timeout=10)
        raise RuntimeError('daemon forced shutdown')
    require(PROCESS.returncode == 0,'unclean daemon shutdown')


def rows(txid):
    registry, = DATA.rglob('wallet_registry.db')
    db = sqlite3.connect(f'file:{registry}?mode=ro',uri=True,timeout=1)
    try:
        row = db.execute("SELECT path FROM wallets WHERE name='default'").fetchone()
    finally:
        db.close()
    require(row is not None,'named test wallet absent')
    wallet = Path(row[0]).resolve()
    require(wallet.is_relative_to(DATA.resolve()),'wallet path escaped isolated node')
    result = []
    for path, table in ((DATA/'blockchain/utxo','wallet_utxos'),(wallet,'utxos'),(wallet,'transactions')):
        db = sqlite3.connect(f'file:{path}?mode=ro',uri=True,timeout=1)
        try:
            result.append(db.execute(f'SELECT count(*) FROM {table} WHERE lower(txid)=lower(?)',(txid,)).fetchone()[0])
        finally:
            db.close()
    return result


def foreign_address(hrp):
    # A fixed foreign P2TR point (generator G), never registered in this wallet.
    data = bytes.fromhex('79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798')
    words = [1];acc=bits=0
    for byte in data:
        acc=(acc<<8)|byte;bits+=8
        while bits>=5:
            bits-=5;words.append((acc>>bits)&31)
    if bits:
        words.append((acc<<(5-bits))&31)
    expanded=[ord(c)>>5 for c in hrp]+[0]+[ord(c)&31 for c in hrp]
    checksum=1
    for value in expanded+words+[0]*6:
        high=checksum>>25;checksum=((checksum&0x1ffffff)<<5)^value
        for i,generator in enumerate((0x3b6a57b2,0x26508e6d,0x1ea119fa,0x3d4233dd,0x2a1462b3)):
            if (high>>i)&1:
                checksum^=generator
    checksum^=0x2bc830a3
    alphabet='qpzry9x8gf2tvdw0s3jn54khce6mua7l'
    return hrp+'1'+''.join(alphabet[x] for x in words+[(checksum>>(5*(5-i)))&31 for i in range(6)])


def mine(address):
    result=rpc('generatetoaddress',[1,address])
    block=(result['blocks'] if isinstance(result,dict) else result)[0]
    return block,rpc('getblock',[block,1])['tx'][0]


def marker(path, contents):
    try:
        return path.read_text().strip()==contents
    except FileNotFoundError:
        return False


def main():
    start()
    owned=rpc('wallet.getnewaddress')['address']
    sibling=rpc('wallet.getnewaddress')['address']
    winning,replacement_txid=mine(owned)
    extension,extension_txid=mine(owned)
    wait(lambda:rows(replacement_txid)==[1,1,1] and rows(extension_txid)==[1,1,1],
         'initial owned branch delivery')
    rpc('invalidateblock',[winning])
    wait(lambda:rows(replacement_txid)==[0,0,0] and rows(extension_txid)==[0,0,0],
         'initial branch removed')
    orphan,orphan_txid=mine(sibling)
    wait(lambda:rows(orphan_txid)==[1,1,1],'sibling reward delivery')
    offset=LOG.stat().st_size
    Path(str(PAUSE)+'.entered').unlink(missing_ok=True)
    Path(str(PAUSE)+'.release').unlink(missing_ok=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
        pending=executor.submit(rpc,'reconsiderblock',[winning],60)
        try:
            wait(lambda:marker(Path(str(PAUSE)+'.entered'),'1'),'post-disconnect barrier')
            wait(lambda:'Processed block disconnect 1 in ' in LOG.read_bytes()[offset:].decode(errors='replace'),
                 'wallet rollback completed before chain transition resumes')
            require(rows(orphan_txid)==[0,0,0],'sibling not removed before kill')
            db=sqlite3.connect(f'file:{DATA / "blockchain/utxo"}?mode=ro',uri=True)
            try:
                require(db.execute("SELECT value FROM utxo_metadata WHERE key='reorg_in_progress'").fetchone()
                    is not None,'advisory reorg intent was not durable before kill')
            finally:
                db.close()
            PROCESS.kill();PROCESS.wait(timeout=10)
        finally:
            Path(str(PAUSE)+'.release').touch()
        try:
            pending.result(timeout=10)
        except (OSError, ValueError, RuntimeError) as error:
            print('Interrupted RPC as expected: '+type(error).__name__,flush=True)
    start()
    wait(lambda:rpc('getblockcount')==2 and rpc('getbestblockhash')==extension,
         'restart selects the intended heavier chain',60)
    wait(lambda:rows(replacement_txid)==[1,1,1] and rows(extension_txid)==[1,1,1],
         'both replacement rewards restored before any new block',60)
    require(rows(orphan_txid)==[0,0,0],'orphan returned after mid-transition restart')
    balance=rpc('wallet.getbalance')
    require(balance.get('utxo_count')==2 and balance.get('total')==200 and balance.get('spendable')==0,
        f'incorrect restarted wallet balance: {balance}')
    stop();start()
    require(rpc('getbestblockhash')==extension,'tip changed on second restart')
    require(rows(orphan_txid)==[0,0,0] and rows(replacement_txid)==[1,1,1] and rows(extension_txid)==[1,1,1],
        'wallet stores changed on second restart')
    require(rpc('wallet.getbalance').get('total')==200,'balance changed on second restart')
    stop()
    receipt={'pass':True,'crash_after_disconnect':True,'replacement_rows':[1,1,1],
        'orphan_rows':[0,0,0],'total_din':200,'spendable_din':0,'evidence':str(ROOT)}
    (ROOT/'result.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps(receipt),flush=True)


try:
    main()
except BaseException:
    print(f'FAILED evidence preserved at {ROOT}',file=sys.stderr,flush=True)
    try:
        Path(str(PAUSE)+'.release').touch();Path(str(WALLET_PAUSE)+'.release').touch();stop()
    except Exception as error:
        print(f'cleanup: {error}',file=sys.stderr)
    raise
