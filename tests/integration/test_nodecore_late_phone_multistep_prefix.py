"""Regression: late-phone account-0 recovery over a MULTI-STEP activation prefix.

A genuinely NEW SDK root whose authenticated v6 floor (103) is ABOVE Orchard
activation (102), buried by a real mined 104. The first canonical event is 104,
so PrepareRecoveredAccount0 must scan two uncertified prefix steps (102, 103)
before delivery, as mainnet does (activation 150000, floor 150074). A non-final
prefix step once carried a nonzero parent snapshot revision, which the account
state rejects; wallet.orchard.recoveraccount0 then answered
account0_recovery_unavailable forever. Recovery must complete with no error
reply and adopt the activation-height note.
"""
import hashlib,json,shutil,os,socket,struct,subprocess,tempfile,threading,time,traceback
from pathlib import Path

root=Path(tempfile.mkdtemp(prefix='sdk-late-phone-multistep-prefix-evidence-')).resolve()
print('SDK late phone multi-step prefix evidence:',root,flush=True)
driver=Path(os.environ['NODECORE_DRIVER']).resolve()
generator=Path(os.environ['ACTIVATION_FIXTURE_GENERATOR']).resolve()
manifest=root/'fixture.json'
with (root/'generator.log').open('xb') as log:
    result=subprocess.run([str(generator),'--export-sdk-activation-fixture'],
        env=dict(os.environ,DINERO_SDK_ACTIVATION_MANIFEST=str(manifest),DINERO_SDK_LATE_HISTORY_FIXTURE='1',
            DINERO_SDK_LATE_FLOOR_ABOVE_ACTIVATION='1'),
        stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT,timeout=180)
assert result.returncode==0,root/'generator.log'
fixture=json.loads(manifest.read_text());datadir=Path(fixture['late_datadir'])
assert fixture['late_floor']==103,fixture
proof=Path(fixture['proof']).read_bytes();magic=fixture['magic']
headers=[bytes.fromhex(h) for h in fixture['headers']]+[bytes.fromhex(fixture['late_tip_header'])]
assert len(headers)==105,len(headers)
bodies={bytes.fromhex(fixture[name+'_hash'])[::-1]:Path(fixture[name]).read_bytes()
    for name in ('first','second','late_tip')}
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
    agent=b'/isolated-late-phone-multistep-prefix-peer/'
    return struct.pack('<iQq',70016,services,int(time.time()))+address+address+struct.pack('<Q',0x5be1f00d)+bytes([len(agent)])+agent+struct.pack('<iB',104,0)
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
                    elif command=='getheaders':send(conn,'headers',bytes([len(headers)])+b''.join(h+b'\0' for h in headers))
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
def call(op,**args):
    process.stdin.write(json.dumps(dict(op=op,**args))+'\n');process.stdin.flush();deadline=time.monotonic()+90
    while time.monotonic()<deadline:
        line=reply.readline()
        if line:
            value=json.loads(line);assert not isinstance(value,dict) or 'driver_error' not in value,value;return value
        assert process.poll() is None,(op,process.returncode);time.sleep(.05)
    raise AssertionError(('SDK call timed out',op))
success=False;progresses=[]
try:
    assert call('start',datadir=str(datadir),config=dict(network='regtest',sync_profile='ios_utreexo',
        addnode=f'127.0.0.1:{port}',max_peers=1,verbose_logging=True))==0
    deadline=time.monotonic()+70
    while time.monotonic()<deadline:
        assert not failures,failures
        if call('rpc',method='getblockcount',params=[])==104:break
        time.sleep(.25)
    else:raise AssertionError('fresh floor-103 SDK did not synchronize to 104')
    # The first canonical event is 104, so the activation prefix is 102..103.
    canonical=call('sdk_activation_state')['canonical_outbox']
    (root/'canonical.json').write_text(json.dumps(canonical,indent=2)+'\n')
    assert canonical['head']==1 and [e['height'] for e in canonical['events']]==[104],canonical
    restored=call('rpc',method='wallet.restore',params=dict(name='late-phone-multistep-owner',
        mnemonic='abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about',rescan=False))
    assert restored.get('success') is True,restored
    binding=call('rpc',method='wallet.orchard.getwalletbinding',params=dict(wallet_name='late-phone-multistep-owner'))
    bound=dict(wallet_binding=binding['wallet_binding']);history_phase=True
    deadline=time.monotonic()+70
    while time.monotonic()<deadline:
        assert not failures,failures
        progress=call('rpc',method='wallet.orchard.recoveraccount0',params=bound);progresses.append(progress)
        (root/'progress.json').write_text(json.dumps(progresses,indent=2)+'\n')
        assert isinstance(progress,dict) and 'error' not in progress,('account-0 recovery refused',progress)
        if progress.get('complete'):break
        time.sleep(.25)
    else:raise AssertionError(('multi-step-prefix account-0 recovery did not complete',progresses[-3:]))
    assert progress['generated'] is False and progress['coverage']=='account_0_only',progress
    assert progress['applied_sequence']==1 and progress['checkpoint_height']==104,progress
    for name in ('first','second'):
        assert dict(kind=2,hash=fixture[name+'_hash'],history=True) in requests,requests
    # The activation-height (prefix step 102) note is adopted.
    balance=call('rpc',method='wallet.orchard.getbalance',params=dict(bound,account=0))
    (root/'balance.json').write_text(json.dumps(balance,indent=2)+'\n')
    assert balance['confirmed_una']==500000,balance
    assert call('stop')==0
    process.stdin.close();process.wait(timeout=30);assert process.returncode==0,process.returncode
    # Completed-account reopen while the activation history cannot become
    # ready (offline, public body cache removed): account-0 recovery must
    # report the already-recovered account and history progress, never refuse.
    shutil.rmtree(datadir/'.orchard-phone-history-v1',ignore_errors=True)
    (datadir/'dinero.conf').write_text('p2p.offline=1\nlisten=0\nrpc.port=0\nwallet.socket.enable=0\nassumeutxo_bg_stall_timeout=3600\n')
    reply.close();replies=root/'cold-replies.jsonl';replies.touch();reply=replies.open()
    output.close();output=(root/'cold-nodecore.log').open('xb')
    process=subprocess.Popen([str(driver),str(replies)],stdin=subprocess.PIPE,text=True,stdout=output,stderr=subprocess.STDOUT)
    assert call('start',datadir=str(datadir),config=dict(network='regtest',sync_profile='ios_utreexo',
        max_peers=1,verbose_logging=True))==0
    assert call('rpc',method='wallet.open',params=['late-phone-multistep-owner'])['success'] is True
    binding=call('rpc',method='wallet.orchard.getwalletbinding',params=dict(wallet_name='late-phone-multistep-owner'))
    bound=dict(wallet_binding=binding['wallet_binding'])
    cold=[]
    for _ in range(6):
        progress=call('rpc',method='wallet.orchard.recoveraccount0',params=bound);cold.append(progress)
        (root/'cold-progress.json').write_text(json.dumps(cold,indent=2)+'\n')
        assert isinstance(progress,dict) and 'error' not in progress,('completed account refused on reopen',progress)
        assert progress.get('already_recovered') is True,progress
        time.sleep(.5)
    assert call('stop')==0
    process.stdin.close();process.wait(timeout=30);assert process.returncode==0,process.returncode
    success=True
    print('PASS late-phone account-0 recovery over a two-step activation prefix (102..103, floor 103)',flush=True)
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
