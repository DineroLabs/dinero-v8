"""Late SDK recovery and fresh-process offline reauthentication of retained DATA."""
import hashlib,json,os,socket,sqlite3,struct,subprocess,tempfile,threading,time,traceback
from pathlib import Path

root=Path(tempfile.mkdtemp(prefix='sdk-late-phone-history-evidence-')).resolve()
print('SDK late phone history evidence:',root,flush=True)
driver=Path(os.environ['NODECORE_DRIVER']).resolve()
generator=Path(os.environ['ACTIVATION_FIXTURE_GENERATOR']).resolve()
manifest=root/'fixture.json'
with (root/'generator.log').open('xb') as log:
    result=subprocess.run([str(generator),'--export-sdk-activation-fixture'],
        env=dict(os.environ,DINERO_SDK_ACTIVATION_MANIFEST=str(manifest),DINERO_SDK_LATE_HISTORY_FIXTURE='1'),
        stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT,timeout=120)
assert result.returncode==0,root/'generator.log'
fixture=json.loads(manifest.read_text());datadir=Path(fixture['late_datadir'])
proof=Path(fixture['proof']).read_bytes();magic=fixture['magic']
headers=[bytes.fromhex(h) for h in fixture['headers']]
bodies={bytes.fromhex(fixture[name+'_hash'])[::-1]:Path(fixture[name]).read_bytes() for name in ('first','second')}
stop=threading.Event();packets=[];requests=[];failures=[];connections=[];history_phase=False
listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen(4);listener.settimeout(.5)
port=listener.getsockname()[1]
def digest(data):return hashlib.sha256(hashlib.sha256(data).digest()).digest()
def record(direction,command,payload):
    path=root/f'packet-{len(packets):05d}-{direction}-{command}.bin';path.write_bytes(payload)
    packets.append(dict(direction=direction,command=command,file=path.name,size=len(payload),sha256=hashlib.sha256(payload).hexdigest()))
    (root/'packets.json').write_text(json.dumps(packets,indent=2)+'\n')
def exact(conn,size):
    out=bytearray()
    while len(out)<size:
        try:part=conn.recv(size-len(out))
        except socket.timeout:
            if stop.is_set():raise EOFError()
            continue
        if not part:raise EOFError()
        out.extend(part)
    return bytes(out)
def send(conn,command,payload=b''):
    record('out',command,payload)
    conn.sendall(struct.pack('<I12sI4s',magic,command.encode(),len(payload),digest(payload)[:4])+payload)
def version():
    services=1|(1<<29)|(1<<30)|(1<<31)
    address=struct.pack('<Q',services)+b'\0'*10+b'\xff\xff\x7f\0\0\1'+struct.pack('>H',port)
    agent=b'/isolated-late-phone-history-peer/'
    return struct.pack('<iQq',70016,services,int(time.time()))+address+address+struct.pack('<Q',0x842b193a)+bytes([len(agent)])+agent+struct.pack('<iB',103,0)
def serve():
    try:
        while not stop.is_set():
            try:conn,_=listener.accept()
            except socket.timeout:continue
            except OSError:
                if stop.is_set():return
                raise
            connections.append(conn);conn.settimeout(.5);transfer=None
            with conn:
                while not stop.is_set():
                    try:raw=exact(conn,24)
                    except (EOFError,ConnectionResetError):break
                    received,command,size,checksum=struct.unpack('<I12sI4s',raw)
                    assert received==magic and size<=2*1024*1024
                    payload=exact(conn,size);assert digest(payload)[:4]==checksum
                    command=command.rstrip(b'\0').decode('ascii');record('in',command,payload)
                    if command=='version':send(conn,'version',version());send(conn,'verack')
                    elif command=='ping':send(conn,'pong',payload)
                    elif command=='getheaders':send(conn,'headers',bytes([104])+b''.join(h+b'\0' for h in headers))
                    elif command=='getocat':
                        assert history_phase,'history must be explicitly requested by a bound recovered wallet'
                        assert len(payload)==151 and payload[:6]==b'DNOCT\x01'
                        assert payload[87:119]==bytes.fromhex(fixture['parent'])[::-1]
                        description=payload+struct.pack('<QI',len(proof),65536)+hashlib.sha256(proof).digest()
                        transfer=hashlib.sha256(description).digest();send(conn,'ocatmeta',description)
                    elif command=='getocatpg':
                        assert transfer and len(payload)==42 and payload[:6]==b'DNOPG\x01' and payload[6:38]==transfer
                        index=struct.unpack_from('<I',payload,38)[0];page=proof[index*65536:(index+1)*65536]
                        assert page;send(conn,'ocatpage',transfer+struct.pack('<II',index,len(page))+page)
                    elif command=='getdata':
                        assert payload and payload[0]<253 and len(payload)==1+36*payload[0]
                        for offset in range(1,len(payload),36):
                            kind,key=struct.unpack_from('<I32s',payload,offset)
                            requests.append(dict(kind=kind,hash=key[::-1].hex(),history=history_phase))
                            if key in bodies:
                                assert kind==2,'Orchard history requires MSG_BLOCK'
                                send(conn,'block',bodies[key])
    except BaseException:
        if not stop.is_set():failures.append(traceback.format_exc())
thread=threading.Thread(target=serve);thread.start()
(datadir/'dinero.conf').write_text('listen=0\nrpc.port=0\nwallet.socket.enable=0\nassumeutxo_bg_stall_timeout=3600\n')
replies=root/'replies.jsonl';replies.touch();reply=replies.open();output=(root/'nodecore.log').open('xb')
process=subprocess.Popen([str(driver),str(replies)],stdin=subprocess.PIPE,text=True,stdout=output,stderr=subprocess.STDOUT)
original_pid=process.pid;cold_restarts=[]
def finish_driver():
    process.stdin.close();process.wait(timeout=30)
    assert process.returncode==0,process.returncode
    output.close();reply.close()
def cache_inventory():
    cache=datadir/'.orchard-phone-history-v1'
    files=sorted(cache.glob('*.raw'));assert len(files)==3,files
    result={p.name:dict(size=p.stat().st_size,sha256=hashlib.sha256(p.read_bytes()).hexdigest(),
        mode=p.stat().st_mode&0o777,links=p.stat().st_nlink) for p in files}
    assert all(v['mode']==0o400 and v['links']==1 for v in result.values()),result
    return result
def call(op,**args):
    process.stdin.write(json.dumps(dict(op=op,**args))+'\n');process.stdin.flush();deadline=time.monotonic()+90
    while time.monotonic()<deadline:
        line=reply.readline()
        if line:
            value=json.loads(line);assert not isinstance(value,dict) or 'driver_error' not in value,value;return value
        assert process.poll() is None,(op,process.returncode);time.sleep(.05)
    raise AssertionError(('SDK call timed out',op))
def inventory():
    with sqlite3.connect(f'file:{datadir}/wallets/wallet_late-phone-owner.db?mode=ro',uri=True) as db:
        catalog=db.execute("SELECT value FROM settings WHERE key='orchard_account_catalog_v1'").fetchall()
        tables=db.execute("SELECT name,sql FROM sqlite_master WHERE name LIKE 'orchard_%' ORDER BY name").fetchall()
        return dict(catalog=catalog,tables=tables)
success=False
try:
    assert call('start',datadir=str(datadir),config=dict(network='regtest',sync_profile='ios_utreexo',
        addnode=f'127.0.0.1:{port}',max_peers=1,verbose_logging=True))==0
    deadline=time.monotonic()+70
    while time.monotonic()<deadline:
        assert not failures,failures
        if call('rpc',method='getblockcount',params=[])==103:break
        time.sleep(.25)
    else:raise AssertionError('fresh snapshot-floor SDK did not synchronize to103')
    restored=call('rpc',method='wallet.restore',params=dict(name='late-phone-owner',
        mnemonic='abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about',rescan=False))
    assert restored.get('success') is True,restored
    binding=call('rpc',method='wallet.orchard.getwalletbinding',params=dict(wallet_name='late-phone-owner'))
    bound=dict(wallet_binding=binding['wallet_binding']);before=inventory();history_phase=True
    (root/'wallet-before.json').write_text(json.dumps(before,indent=2)+'\n')
    canonical_before=call('sdk_activation_state')['canonical_outbox']
    assert canonical_before['head']==1 and [e['height'] for e in canonical_before['events']]==[103],canonical_before
    (root/'canonical-before.json').write_text(json.dumps(canonical_before,indent=2)+'\n')
    deadline=time.monotonic()+70
    while time.monotonic()<deadline:
        assert not failures,failures
        progress=call('rpc',method='wallet.orchard.recoveraccount0',params=bound)
        (root/'progress.json').write_text(json.dumps(progress,indent=2)+'\n')
        state=call('sdk_activation_state');(root/'sdk-state.json').write_text(json.dumps(state,indent=2)+'\n')
        assert isinstance(progress,dict) and 'error' not in progress,progress
        if progress.get('complete'):break
        assert not progress['scan_complete_to_current_tip'] and progress['applied_sequence']==0,progress
        time.sleep(.25)
    else:raise AssertionError('fresh late-phone account-0 recovery did not complete')
    assert progress['complete'] and progress['generated'] is False and progress['coverage']=='account_0_only',progress
    assert progress['applied_sequence']==1 and progress['checkpoint_height']==103,progress
    for name in ('first','second'):
        assert dict(kind=2,hash=fixture[name+'_hash'],history=True) in requests,requests
    after=inventory();assert before!=after,'Only final authenticated recovery may adopt wallet state'
    balance=call('rpc',method='wallet.orchard.getbalance',params=dict(bound,account=0))
    received=call('rpc',method='wallet.orchard.listreceived',params=dict(bound,account=0))
    assert balance['confirmed_una']==500000,balance
    assert received['history_complete'] and received['total_count']==1,received
    note=received['received'][0]
    assert note['amount_una']==500000 and note['height']==102 and note['block_hash']==fixture['first_hash'],note
    assert bytes.fromhex(note['memo_hex'])==b'authenticated phone history'.ljust(512,b'\0'),note
    assert call('rpc',method='getblockcount',params=[])==103
    canonical_after=call('sdk_activation_state')['canonical_outbox'];assert canonical_after==canonical_before
    assert call('rpc',method='wallet.encrypt',params=['retained-history-test-password'])['success'] is True
    assert call('stop')==0
    cache_before=cache_inventory();finish_driver()
    # No process-local corpus/key, peer or phrase may stand in for retention.
    stop.set();listener.close()
    for conn in connections:
        try:conn.shutdown(socket.SHUT_RDWR)
        except OSError:pass
    thread.join(timeout=5);assert not thread.is_alive() and not failures,failures
    packet_count=len(packets)
    (datadir/'dinero.conf').write_text('p2p.offline=1\nlisten=0\nrpc.port=0\nwallet.socket.enable=0\nassumeutxo_bg_stall_timeout=3600\n')
    for restart in range(2):
        replies=root/f'cold-{restart}-replies.jsonl';replies.touch();reply=replies.open()
        output=(root/f'cold-{restart}-nodecore.log').open('xb')
        process=subprocess.Popen([str(driver),str(replies)],stdin=subprocess.PIPE,text=True,stdout=output,stderr=subprocess.STDOUT)
        assert process.pid!=original_pid
        assert call('start',datadir=str(datadir),config=dict(network='regtest',sync_profile='ios_utreexo',
            max_peers=1,verbose_logging=True))==0
        deadline=time.monotonic()+70
        while time.monotonic()<deadline:
            state=call('sdk_activation_state')
            if state.get('phone_history',{}).get('ready'):break
            time.sleep(.1)
        else:raise AssertionError(('cold process did not reauthenticate retained history',state))
        assert call('rpc',method='wallet.open',params=['late-phone-owner'])['success'] is True
        assert call('rpc',method='wallet.unlock',params=['retained-history-test-password',0])['success'] is True
        binding=call('rpc',method='wallet.orchard.getwalletbinding',params=dict(wallet_name='late-phone-owner'))
        bound=dict(wallet_binding=binding['wallet_binding'])
        recovered=call('rpc',method='wallet.orchard.recoveraccount0',params=bound)
        reopened_balance=call('rpc',method='wallet.orchard.getbalance',params=dict(bound,account=0))
        reopened_received=call('rpc',method='wallet.orchard.listreceived',params=dict(bound,account=0))
        assert recovered.get('complete') and recovered['generated'] is False,recovered
        assert reopened_balance['confirmed_una']==500000,reopened_balance
        assert reopened_received['history_complete'] and reopened_received['received']==received['received'],reopened_received
        assert call('sdk_activation_state')['canonical_outbox']==canonical_before
        assert cache_inventory()==cache_before,'Cold reauthentication must not rewrite retained DATA'
        cold_restarts.append(dict(pid=process.pid,state=state,recovered=recovered,
            balance=reopened_balance,received=reopened_received))
        assert call('stop')==0;finish_driver()
    assert len(packets)==packet_count,'Offline retention must not require a peer'
    success=True
    (root/'receipt.json').write_text(json.dumps(dict(fixture=fixture,packets=packets,requests=requests,
        progress=progress,wallet_before=before,wallet_after=after,balance=balance,received=received,
        canonical_before=canonical_before,canonical_after=canonical_after,
        original_pid=original_pid,cold_restarts=cold_restarts,cache=cache_before,
        scope='Fresh authenticated v6 floor102, actual SDK canonical103, untrusted full102/103 download, recovered account0 funds/memo adoption, then two fresh-process offline encrypted named-wallet reopens with genuine proof/body reauthentication. Not original assumed SQL, nonempty queued crossing or devices.'),indent=2)+'\n')
    print('PASS late-phone funds/memo recovery and two fresh-process offline authenticated history reopens',flush=True)
finally:
    stop.set();listener.close()
    for conn in connections:
        try:conn.shutdown(socket.SHUT_RDWR)
        except OSError:pass
    thread.join(timeout=5)
    if not process.stdin.closed:process.stdin.close()
    try:process.wait(timeout=30)
    except subprocess.TimeoutExpired:process.kill();process.wait(timeout=5)
    output.close();reply.close();(root/'peer-failures.json').write_text(json.dumps(failures,indent=2)+'\n')
    if success:assert process.returncode==0 and not thread.is_alive() and not failures
