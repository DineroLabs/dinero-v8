"""Real loopback catalog ingress and SDK automatic handoff, source-ready parent.

This is not original mobile assumed-history or queued-payment acceptance. Every
root, child log, reply and untrusted packet is retained, including failures.
"""
import hashlib,json,os,socket,struct,subprocess,tempfile,threading,time,traceback
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
fixture=json.loads(manifest.read_text());datadir=Path(fixture['datadir'])
proof=Path(fixture['proof']).read_bytes();assert 0<len(proof)<=1<<30
magic=fixture['magic'];headers=[bytes.fromhex(h) for h in fixture['headers']]
assert len(headers)==104 and all(len(h)==128 for h in headers)
stop=threading.Event();ready=threading.Event();packets=[];failures=[];connections=[]
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
            connections.append(conn);conn.settimeout(.5);transfer=None
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
                    # Bodies are deliberately withheld until automatic parent
                    # handoff is proved. No peer-controlled current tip is an
                    # ownership or wallet-history certificate.
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
try:
    assert call('sdk_activation_events') is True
    assert call('start',datadir=str(datadir),config=dict(network='regtest',sync_profile='ios_utreexo',
        addnode=f'127.0.0.1:{port}',max_peers=1,verbose_logging=True))==0
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
    assert any(p['command']=='getocat' for p in packets) and any(p['command']=='getocatpg' for p in packets)
    assert call('stop')==0
    success=True
    (root/'receipt.json').write_text(json.dumps(dict(fixture=fixture,packets=packets,
        source_ready_parent_handoff=True,named_wallet_without_phrase=True,paused_queue_no_reply=state['pause_probes'],
        scope='Actual loopback P2P catalog/SDK worker at independently replayed parent; no queued payment, original assumed mobile history, descendant body download or device acceptance.'),indent=2)+'\n')
    print('PASS actual SDK loopback catalog and automatic parent handoff; named wallet retained',flush=True)
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
