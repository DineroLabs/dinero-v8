"""Real catalog/handoff/full-body ingress and late account-0 recovery.

The separate assumed-mobile variant uses normal snapshot import with original
SQL lifecycle keys. Neither variant has a nonempty queued payment. Every root,
child log, reply and untrusted packet is retained, including failures.
"""
import hashlib,json,os,socket,sqlite3,struct,subprocess,tempfile,threading,time,traceback
from pathlib import Path

root=Path(tempfile.mkdtemp(prefix='sdk-activation-wire-evidence-')).resolve()
print('SDK activation wire evidence:',root,flush=True)
driver=Path(os.environ['NODECORE_DRIVER']).resolve()
generator=Path(os.environ['ACTIVATION_FIXTURE_GENERATOR']).resolve()
manifest=root/'fixture.json'
with (root/'generator.log').open('xb') as log:
    result=subprocess.run([str(generator),'--export-sdk-activation-fixture'],
        env=dict(os.environ,DINERO_SDK_ACTIVATION_MANIFEST=str(manifest)),
        stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT,timeout=120)
assert result.returncode==0,(root/'generator.log')
fixture=json.loads(manifest.read_text());assumed=bool(os.environ.get('DINERO_SDK_ASSUMED_MOBILE_FIXTURE'))
datadir=Path(fixture['assumed_datadir'] if assumed else fixture['datadir'])
proof=Path(fixture['proof']).read_bytes();assert 0<len(proof)<=1<<30
magic=fixture['magic'];headers=[bytes.fromhex(h) for h in fixture['headers']]
assert len(headers)==104 and all(len(h)==128 for h in headers)
stop=threading.Event();ready=threading.Event();bodies_ready=threading.Event()
packets=[];failures=[];connections=[];pending={};body_requests=[]
bodies={bytes.fromhex(fixture[name+'_hash'])[::-1]:Path(fixture[name]).read_bytes()
        for name in ('first','second')}
listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen(4);listener.settimeout(.5)
port=listener.getsockname()[1]
def double_sha(data):return hashlib.sha256(hashlib.sha256(data).digest()).digest()
def record(direction,command,payload):
    path=root/f'packet-{len(packets):05d}-{direction}-{command}.bin';path.write_bytes(payload)
    packets.append(dict(direction=direction,command=command,size=len(payload),
        file=path.name,sha256=hashlib.sha256(payload).hexdigest()))
    (root/'packets.json').write_text(json.dumps(packets,indent=2)+'\n')
def exact(conn,size):
    out=bytearray()
    while len(out)<size:
        flush_bodies(conn)
        try:part=conn.recv(size-len(out))
        except socket.timeout:
            if stop.is_set():raise EOFError()
            continue
        if not part:raise EOFError()
        out.extend(part)
    return bytes(out)
def send(conn,command,payload=b''):
    record('out',command,payload)
    conn.sendall(struct.pack('<I12sI4s',magic,command.encode(),len(payload),double_sha(payload)[:4])+payload)
def flush_bodies(conn):
    if bodies_ready.is_set():
        for key in list(pending.get(conn,{})):
            send(conn,'block',bodies[key]);del pending[conn][key]
def inventory(payload):
    assert payload,'empty getdata'
    prefix=payload[0]
    width={253:2,254:4,255:8}.get(prefix,0)
    count=int.from_bytes(payload[1:1+width],'little') if width else prefix
    offset=1+width
    assert count<=50000 and len(payload)==offset+36*count
    return [struct.unpack_from('<I32s',payload,offset+36*i) for i in range(count)]
def version():
    services=1|(1<<29)|(1<<30)|(1<<31)
    address=struct.pack('<Q',services)+b'\x00'*10+b'\xff\xff\x7f\x00\x00\x01'+struct.pack('>H',port)
    agent=b'/isolated-sdk-catalog-peer/'
    return struct.pack('<iQq',70016,services,int(time.time()))+address+address+struct.pack('<Q',0x842b193a)+bytes([len(agent)])+agent+struct.pack('<iB',103,0)
def serve():
    try:
        while not stop.is_set():
            try:conn,_=listener.accept()
            except socket.timeout:continue
            except OSError:
                if stop.is_set():return
                raise
            connections.append(conn);pending[conn]={};conn.settimeout(.5);transfer=None
            with conn:
                while not stop.is_set():
                    try:raw=exact(conn,24)
                    except socket.timeout:continue
                    except (EOFError,ConnectionResetError):break
                    received,command,size,checksum=struct.unpack('<I12sI4s',raw)
                    assert received==magic and size<=2*1024*1024
                    payload=exact(conn,size);assert double_sha(payload)[:4]==checksum
                    command=command.rstrip(b'\0').decode('ascii')
                    record('in',command,payload)
                    if command=='version':send(conn,'version',version());send(conn,'verack')
                    elif command=='ping':send(conn,'pong',payload)
                    elif command=='getheaders':send(conn,'headers',bytes([104])+b''.join(h+b'\0' for h in headers))
                    elif command=='getocat':
                        assert ready.wait(30),'test did not open named wallet before proof'
                        assert len(payload)==151 and payload[:6]==b'DNOCT\x01'
                        assert payload[87:119]==bytes.fromhex(fixture['parent'])[::-1]
                        assert payload[119:151]==bytes.fromhex(fixture['first_hash'])[::-1]
                        assert payload[43:75]!=bytes(32)
                        description=payload+struct.pack('<QI',len(proof),65536)+hashlib.sha256(proof).digest()
                        transfer=hashlib.sha256(description).digest();send(conn,'ocatmeta',description)
                    elif command=='getocatpg':
                        assert transfer and len(payload)==42 and payload[:6]==b'DNOPG\x01' and payload[6:38]==transfer
                        index=struct.unpack_from('<I',payload,38)[0];page=proof[index*65536:(index+1)*65536]
                        assert page;send(conn,'ocatpage',transfer+struct.pack('<II',index,len(page))+page)
                    elif command=='getdata':
                        for kind,key in inventory(payload):
                            body_requests.append(dict(kind=kind,hash=key[::-1].hex()))
                            if key in bodies:
                                assert kind==2,'Orchard body must use full MSG_BLOCK, not a legacy proof'
                                pending[conn][key]=True
                        flush_bodies(conn)
                    # Publication is observed independently through the public
                    # SDK. The peer supplies bytes, never a wallet certificate.
    except BaseException:
        if not stop.is_set():failures.append(traceback.format_exc())
thread=threading.Thread(target=serve);thread.start()
(datadir/'dinero.conf').write_text('listen=0\nrpc.port=0\nwallet.socket.enable=0\n'
    'utreexo.checkpoint_interval=1000\nassumeutxo_bg_stall_timeout=3600\n')
replies=root/'replies.jsonl';replies.touch();reply=replies.open();output=(root/'nodecore.log').open('xb')
process=subprocess.Popen([str(driver),str(replies)],stdin=subprocess.PIPE,text=True,stdout=output,stderr=subprocess.STDOUT)
def call(op,**args):
    process.stdin.write(json.dumps(dict(op=op,**args))+'\n');process.stdin.flush()
    deadline=time.monotonic()+90
    while time.monotonic()<deadline:
        line=reply.readline()
        if line:
            value=json.loads(line);assert not isinstance(value,dict) or 'driver_error' not in value,value
            return value
        assert process.poll() is None,(op,process.returncode)
        time.sleep(.05)
    raise AssertionError(('SDK call timed out',op))
success=False
config=dict(network='regtest',sync_profile='ios_utreexo',
    addnode=f'127.0.0.1:{port}',max_peers=1,verbose_logging=True)
if assumed:config['assumeutxo_snapshot']=fixture['assumed_snapshot']
def original_lifecycle():
    with sqlite3.connect('file:'+str(datadir/'blockchain/utxo')+'?mode=ro',uri=True) as db:
        return dict(db.execute("SELECT key,value FROM utxo_metadata WHERE key LIKE 'assumeutxo_%' OR key='wallet_snapshot_recovery_base_height' ORDER BY key"))
try:
    assert call('sdk_activation_events') is True
    assert call('start',datadir=str(datadir),config=config)==0
    if assumed:
        deadline=time.monotonic()+45
        while time.monotonic()<deadline:
            if call('rpc',method='getblockcount',params=[])==101:break
            time.sleep(.25)
        original=original_lifecycle();(root/'original-lifecycle.json').write_text(json.dumps(original,indent=2)+'\n')
        assert original['assumeutxo_active']=='true',original
        assert original['assumeutxo_base_height']=='101',original
        assert original['assumeutxo_base_block']==fixture['parent'],original
        assert original.get('assumeutxo_fully_validated')!='true',original
    assert call('rpc',method='getblockcount',params=[])==101,'source-ready parent not loaded'
    restored=call('rpc',method='wallet.restore',params=dict(name='wire-named-owner',
        mnemonic='abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about',rescan=False))
    assert restored.get('success') is True,restored
    ready.set();deadline=time.monotonic()+70
    completed=datadir/'blockchain/.orchard-handoff-selected-v1.done'
    while time.monotonic()<deadline:
        assert not failures,failures
        state=call('sdk_activation_state')
        (root/'sdk-state.json').write_text(json.dumps(state,indent=2)+'\n')
        assert not state.get('shutdown_requested'),'SDK worker failed closed; inspect retained nodecore.log'
        if completed.exists():
            status=call('status')
            # The public API intentionally has no queryable App during the
            # orderly pause. Do not turn that allowed no-reply into a failure.
            if isinstance(status,dict) and status.get('running'):break
        time.sleep(.5)
    else:raise AssertionError('actual SDK automatic activation handoff did not complete')
    state=call('sdk_activation_state')
    assert state['pause_probes']==1 and state['pause_refusals']==0,state
    wallet=call('rpc',method='wallet.getwalletinfo',params=[])
    assert wallet['walletname']=='wire-named-owner',wallet
    assert call('rpc',method='wallet.open',params=['wire-named-owner'])['success'] is True
    assert call('rpc',method='getblockcount',params=[])==101
    if assumed:
        after=original_lifecycle();assert after==original,(original,after)
        (root/'after-handoff-lifecycle.json').write_text(json.dumps(after,indent=2)+'\n')
    assert any(p['command']=='getocat' for p in packets) and any(p['command']=='getocatpg' for p in packets)
    bodies_ready.set();deadline=time.monotonic()+60
    while time.monotonic()<deadline:
        assert not failures,failures
        height=call('rpc',method='getblockcount',params=[])
        if height==103:break
        time.sleep(.25)
    else:raise AssertionError(('SDK did not canonically validate downloaded first/descendant bodies',height,body_requests))
    for name in ('first','second'):
        assert dict(kind=2,hash=fixture[name+'_hash']) in body_requests,body_requests
    assert not (datadir/'chainstate_recovery.marker').exists(),'completed activation must finish its guarded integrity audit'
    restored=call('rpc',method='wallet.restore',params=dict(name='late-history-owner',
        mnemonic='abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about',rescan=False))
    assert restored.get('success') is True,restored
    binding=call('rpc',method='wallet.orchard.getwalletbinding',params=dict(wallet_name='late-history-owner'))
    bound=dict(wallet_binding=binding['wallet_binding'])
    for _ in range(8):
        recovery=call('rpc',method='wallet.orchard.recoveraccount0',params=bound)
        assert isinstance(recovery,dict) and 'error' not in recovery,recovery
        if recovery.get('complete'):break
    assert recovery.get('complete') is True,recovery
    assert recovery['generated'] is False and recovery['coverage']=='account_0_only',recovery
    balance=call('rpc',method='wallet.orchard.getbalance',params=dict(bound,account=0))
    received=call('rpc',method='wallet.orchard.listreceived',params=dict(bound,account=0))
    assert balance['confirmed_una']==500000,balance
    assert received['history_complete'] is True and received['total_count']==1,received
    note=received['received'][0]
    assert note['amount_una']==500000 and note['height']==102 and note['block_hash']==fixture['first_hash'],note
    assert bytes.fromhex(note['memo_hex'])==b'authenticated phone history'.ljust(512,b'\0'),note
    (root/'recovery.json').write_text(json.dumps(dict(recovery=recovery,balance=balance,received=received),indent=2)+'\n')
    assert call('rpc',method='wallet.encrypt',params=['history-test-password'])['success'] is True
    assert call('stop')==0
    assert call('start',datadir=str(datadir),config=config)==0
    assert call('rpc',method='getblockcount',params=[])==103
    if assumed:assert original_lifecycle()==original
    assert call('rpc',method='wallet.open',params=['late-history-owner'])['success'] is True
    assert call('rpc',method='wallet.unlock',params=['history-test-password',0])['success'] is True
    binding=call('rpc',method='wallet.orchard.getwalletbinding',params=dict(wallet_name='late-history-owner'))
    bound=dict(wallet_binding=binding['wallet_binding'])
    reopened_balance=call('rpc',method='wallet.orchard.getbalance',params=dict(bound,account=0))
    reopened_received=call('rpc',method='wallet.orchard.listreceived',params=dict(bound,account=0))
    assert reopened_balance['confirmed_una']==500000,reopened_balance
    assert reopened_received['history_complete'] and reopened_received['received']==received['received'],reopened_received
    if assumed:assert original_lifecycle()==original
    (root/'reopened-recovery.json').write_text(json.dumps(dict(balance=reopened_balance,received=reopened_received),indent=2)+'\n')
    assert call('stop')==0
    success=True
    (root/'receipt.json').write_text(json.dumps(dict(fixture=fixture,packets=packets,
        source_ready_parent_handoff=True,named_wallet_without_phrase=True,paused_queue_no_reply=state['pause_probes'],
        original_assumed_mobile=assumed,original_lifecycle=original if assumed else None,
        body_requests=body_requests,late_restored_funds_and_memo=True,
        scope=('Actual normal legacy snapshot import with original assumed SQL.' if assumed else 'Actual independently executed full starting parent.')+' Loopback catalog/SDK parent handoff plus full activation/descendant body download and retained authenticated account-0 recovery. Not a fresh late snapshot phone, nonempty queued crossing or device acceptance.'),indent=2)+'\n')
    print('PASS actual SDK full-body activation download and encrypted late account-0 funds/memo cold reopen',flush=True)
finally:
    ready.set();stop.set();listener.close()
    for conn in connections:
        try:conn.shutdown(socket.SHUT_RDWR)
        except OSError:pass
    thread.join(timeout=5)
    process.stdin.close()
    try:process.wait(timeout=30)
    except subprocess.TimeoutExpired:process.kill();process.wait(timeout=5)
    output.close();reply.close()
    (root/'peer-failures.json').write_text(json.dumps(failures,indent=2)+'\n')
    if success:assert process.returncode==0 and not thread.is_alive() and not failures
