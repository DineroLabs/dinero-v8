"""Stage D of 2026-11-11-native-v6-wallet-recovery-design.md: native transparent
wallet recovery on a late v6 floor (composed V5 base + verified typed tail).

A genuinely NEW SDK root with only an authenticated v6 floor (102) syncs to the
fixture tip, restores the funded named wallet, completes the typed phone-history
download through the real account-0 recovery, then recovers the transparent
wallet through wallet.recovertransparentfromsnapshot against the preserved V5
activation-parent container. The recovered balance must equal the funded ground
truth exactly, the success scope must be the compact-v6 scope the apps pin, and
a fresh offline process must reopen the same wallet state (no rewind, no peer).
"""
import hashlib,json,os,socket,sqlite3,struct,subprocess,tempfile,threading,time,traceback
from pathlib import Path

root=Path(tempfile.mkdtemp(prefix='sdk-late-phone-wallet-recovery-evidence-')).resolve()
print('SDK late phone wallet recovery evidence:',root,flush=True)
driver=Path(os.environ['NODECORE_DRIVER']).resolve()
generator=Path(os.environ['ACTIVATION_FIXTURE_GENERATOR']).resolve()
manifest=root/'fixture.json'
native_replies=root/'native-replies.jsonl';native_replies.touch()
native_read=native_replies.open();native_log=(root/'generator.log').open('xb')
native=subprocess.Popen([str(generator),'--export-sdk-activation-fixture'],
    env=dict(os.environ,DINERO_SDK_ACTIVATION_MANIFEST=str(manifest),
        DINERO_SDK_LATE_HISTORY_FIXTURE='1',DINERO_SDK_FUNDED_CROSSING_FIXTURE='1',
        DINERO_SDK_ASSUMED_MOBILE_FIXTURE='1',DINERO_SDK_V5_ACTIVATION_FIXTURE='1',
        DINERO_SDK_FUNDED_REPLIES=str(native_replies)),
    stdin=subprocess.PIPE,text=True,stdout=native_log,stderr=subprocess.STDOUT)
deadline=time.monotonic()+180
while not manifest.exists():
    assert native.poll() is None,('native fixture failed',native.returncode,root)
    assert time.monotonic()<deadline,('native fixture timed out',root)
    time.sleep(.05)
fixture=json.loads(manifest.read_text())
datadir=Path(fixture['late_datadir'])
v5_source=Path(fixture['assumed_snapshot'])
assert fixture['assumed_snapshot_version']==5 and fixture['assumed_base']==101,fixture
assert v5_source.is_file(),v5_source
proof=Path(fixture['proof']).read_bytes();magic=fixture['magic']
headers=[bytes.fromhex(h) for h in fixture['headers']]
bodies={bytes.fromhex(fixture[name+'_hash'])[::-1]:Path(fixture[name]).read_bytes() for name in ('first','second')}
def double_sha(data):return hashlib.sha256(hashlib.sha256(data).digest()).digest()
# Historical creation bodies for owned legacy leaves (keyed by header hash).
legacy_bodies={double_sha(Path(path).read_bytes()[:128])[::-1]:Path(path).read_bytes()
    for path in fixture.get('funding_bodies',{}).values()}
stop=threading.Event();packets=[];requests=[];failures=[];connections=[];history_phase=False
announced=set();initial_headers=None
listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen(4);listener.settimeout(.5)
port=listener.getsockname()[1]
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
    conn.sendall(struct.pack('<I12sI4s',magic,command.encode(),len(payload),double_sha(payload)[:4])+payload)
def version():
    services=1|(1<<29)|(1<<30)|(1<<31)
    address=struct.pack('<Q',services)+b'\0'*10+b'\xff\xff\x7f\0\0\1'+struct.pack('>H',port)
    agent=b'/isolated-late-phone-wallet-recovery-peer/'
    return struct.pack('<iQq',70016,services,int(time.time()))+address+address+struct.pack('<Q',0x19d47ab2)+bytes([len(agent)])+agent+struct.pack('<iB',103,0)
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
                    payload=exact(conn,size);assert double_sha(payload)[:4]==checksum
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
                            elif key in legacy_bodies:
                                send(conn,'block',legacy_bodies[key])
                    # Announce tail extensions: the SDK polls headers only once,
                    # so push the grown chain unsolicited (deduped per peer).
                    if initial_headers and len(headers)>initial_headers and (id(conn),len(headers)) not in announced:
                        announced.add((id(conn),len(headers)))
                        send(conn,'headers',bytes([len(headers)])+b''.join(h+b'\0' for h in headers))
    except BaseException:
        if not stop.is_set():failures.append(traceback.format_exc())
initial_headers=len(headers)
thread=threading.Thread(target=serve);thread.start()
# The first start forwards the V5 source through the nodecore_start JSON
# whitelist (the path the apps use); the key is deliberately NOT in this conf,
# so a broken forwarding surfaces as the permanent capability refusal below.
(datadir/'dinero.conf').write_text('listen=0\nrpc.port=0\nwallet.socket.enable=0\nassumeutxo_bg_stall_timeout=3600\n')
replies=root/'replies.jsonl';replies.touch();reply=replies.open();output=(root/'nodecore.log').open('xb')
process=subprocess.Popen([str(driver),str(replies)],stdin=subprocess.PIPE,text=True,stdout=output,stderr=subprocess.STDOUT)
original_pid=process.pid
def finish_driver():
    process.stdin.close();process.wait(timeout=30)
    assert process.returncode==0,process.returncode
    output.close();reply.close()
def call(op,**args):
    process.stdin.write(json.dumps(dict(op=op,**args))+'\n');process.stdin.flush();deadline=time.monotonic()+90
    while time.monotonic()<deadline:
        line=reply.readline()
        if line:
            value=json.loads(line);assert not isinstance(value,dict) or 'driver_error' not in value,value;return value
        assert process.poll() is None,(op,process.returncode);time.sleep(.05)
    raise AssertionError(('SDK call timed out',op))
FUNDED_UNA=fixture['funding_amount_una'];assert FUNDED_UNA==1000000
native_lock=threading.Lock()
def native_call(op,**args):
    with native_lock:
        native.stdin.write(json.dumps(dict(op=op,**args))+'\n');native.stdin.flush()
        deadline=time.monotonic()+60
        while time.monotonic()<deadline:
            line=native_read.readline()
            if line:return json.loads(line)
            assert native.poll() is None,('native source died',native.returncode)
            time.sleep(.05)
        raise AssertionError(('native call timed out',op))
def extend_chain(mined):
    headers.append(bytes.fromhex(mined['header']))
    bodies[bytes.fromhex(mined['hash'])[::-1]]=bytes.fromhex(mined['body'])
owner=dict(walletname='stage-d-owner',expected_first_address=fixture['funding_address'])
def wallet_utxos():
    with sqlite3.connect(f'file:{datadir}/wallets/wallet_stage-d-owner.db?mode=ro',uri=True) as db:
        live=db.execute('SELECT txid,vout,amount,height,is_coinbase FROM utxos WHERE is_spent=0 ORDER BY txid,vout').fetchall()
        spent=db.execute('SELECT txid,vout,spent_height FROM utxos WHERE is_spent=1 ORDER BY txid,vout').fetchall()
        history=db.execute('SELECT txid,category,amount,height FROM transactions ORDER BY height,txid').fetchall()
        return dict(live=live,spent=spent,history=history)
success=False
try:
    assert call('start',datadir=str(datadir),config=dict(network='regtest',sync_profile='ios_utreexo',
        addnode=f'127.0.0.1:{port}',max_peers=1,verbose_logging=True,
        wallet_recovery_v5_source=str(v5_source)))==0
    deadline=time.monotonic()+70
    while time.monotonic()<deadline:
        assert not failures,failures
        if call('rpc',method='getblockcount',params=[])==103:break
        time.sleep(.25)
    else:raise AssertionError('fresh snapshot-floor SDK did not synchronize to 103')
    restored=call('rpc',method='wallet.restore',params=dict(name='stage-d-owner',
        mnemonic='abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about',rescan=False))
    assert restored.get('success') is True,restored
    # The composed arm refuses (retryable) until the verified typed history is
    # downloaded; the permanent capability refusal must NOT appear, because a
    # V5 source is configured. The permanent message may never ride on a
    # retryable reply.
    early=call('rpc',method='wallet.recovertransparentfromsnapshot',params=owner)
    (root/'early-recovery.json').write_text(json.dumps(early,indent=2)+'\n')
    assert early.get('error') and early.get('retryable') is True,early
    assert early['error']!='transparent snapshot recovery requires a pre-Orchard selected source',early
    def check_refusal(reply):
        assert reply.get('retryable') is True,reply
        assert reply['error']!='transparent snapshot recovery requires a pre-Orchard selected source',reply
    # Complete the typed phone-history download through the real account-0
    # recovery, exactly as the phones do.
    binding=call('rpc',method='wallet.orchard.getwalletbinding',params=dict(wallet_name='stage-d-owner'))
    bound=dict(wallet_binding=binding['wallet_binding']);history_phase=True
    deadline=time.monotonic()+70
    while time.monotonic()<deadline:
        assert not failures,failures
        progress=call('rpc',method='wallet.orchard.recoveraccount0',params=bound)
        if isinstance(progress,dict) and progress.get('complete'):break
        assert isinstance(progress,dict) and 'error' not in progress,progress
        time.sleep(.25)
    else:raise AssertionError('late-phone account-0 recovery did not complete')
    # Native transparent recovery: composed V5 base + verified typed tail.
    recoveries=[];deadline=time.monotonic()+90
    while time.monotonic()<deadline:
        assert not failures,failures
        recovered=call('rpc',method='wallet.recovertransparentfromsnapshot',params=owner)
        recoveries.append(recovered)
        (root/'recoveries.json').write_text(json.dumps(recoveries,indent=2)+'\n')
        if recovered.get('success'):break
        check_refusal(recovered)
        time.sleep(.5)
    else:raise AssertionError(('native v6 transparent recovery did not complete',recoveries))
    assert recovered['success'] is True and recovered['recovered_height']==103,recovered
    assert recovered['scope']=='transparent-current-coins-and-post-snapshot-history-v6',recovered
    assert recovered['whole_wallet_certified'] is False,recovered
    # Full-balance equality: the funded wallet owns exactly the height-101
    # funding coin; the typed tail (102..103) has no owned effects.
    balance=call('rpc',method='wallet.getbalance',params=[])
    (root/'balance.json').write_text(json.dumps(balance,indent=2)+'\n')
    assert 'error' not in balance,balance
    assert balance['utxo_count']==1 and balance['confirmed']==FUNDED_UNA/1e8,balance
    rows=wallet_utxos();(root/'wallet-rows.json').write_text(json.dumps(rows,indent=2)+'\n')
    assert len(rows['live'])==1 and not rows['spent'],rows
    txid,vout,amount,height,coinbase=rows['live'][0]
    assert txid==fixture['funding_txid'] and vout==0 and amount==FUNDED_UNA and height==101 and not coinbase,rows
    assert call('rpc',method='getblockcount',params=[])==103
    # Recovery is idempotent and keeps the same scope.
    again=call('rpc',method='wallet.recovertransparentfromsnapshot',params=owner)
    assert again.get('success') is True and again['recovered_height']==103 and again['scope']==recovered['scope'],again
    assert wallet_utxos()==rows,'repeat recovery must not change wallet rows'
    # Tail activity T1: the chain advances past the download target with a
    # block-104 coinbase paying the funded wallet (an owned IMMATURE
    # tail-created coin). Recovery must follow the longer verified tail.
    with sqlite3.connect(f'file:{datadir}/wallets/wallet_stage-d-owner.db?mode=ro',uri=True) as db:
        funding_script=db.execute('SELECT script_pubkey FROM utxos WHERE txid=? AND vout=0',(fixture['funding_txid'],)).fetchone()[0]
    assert native_call('advance')['height']==103
    mined5=native_call('mine_next',address=fixture['funding_address'],script=funding_script)
    assert mined5['height']==104 and mined5['amount_una']>0,mined5
    extend_chain(mined5)
    deadline=time.monotonic()+60
    while time.monotonic()<deadline:
        assert not failures,failures
        if call('rpc',method='getblockcount',params=[])==104:break
        time.sleep(.25)
    else:raise AssertionError('late phone did not follow the extended tail to 104')
    tail_recoveries=[];deadline=time.monotonic()+90
    while time.monotonic()<deadline:
        assert not failures,failures
        tail_recovered=call('rpc',method='wallet.recovertransparentfromsnapshot',params=owner)
        tail_recoveries.append(tail_recovered)
        (root/'tail-recoveries.json').write_text(json.dumps(tail_recoveries,indent=2)+'\n')
        if tail_recovered.get('success'):break
        check_refusal(tail_recovered)
        time.sleep(.5)
    else:raise AssertionError(('tail recovery did not complete',tail_recoveries))
    assert tail_recovered['recovered_height']==104 and tail_recovered['scope']==recovered['scope'],tail_recovered
    tail_balance=call('rpc',method='wallet.getbalance',params=[])
    (root/'tail-balance.json').write_text(json.dumps(tail_balance,indent=2)+'\n')
    assert 'error' not in tail_balance,tail_balance
    assert tail_balance['confirmed']==FUNDED_UNA/1e8,tail_balance
    assert tail_balance['immature']==mined5['amount_una']/1e8,tail_balance
    rows=wallet_utxos();(root/'tail-wallet-rows.json').write_text(json.dumps(rows,indent=2)+'\n')
    assert len(rows['live'])==2 and not rows['spent'],rows
    coinbase_rows=[r for r in rows['live'] if r[4]]
    assert len(coinbase_rows)==1 and coinbase_rows[0][0]==mined5['coinbase_txid'] and coinbase_rows[0][3]==104,rows
    # Tail activity T2: the native source spends the funded BASE coin inside a
    # real shield at 105, with change to the wallet's own first change script.
    # Recovery must mark the base coin spent (never overstate) and account the
    # owned change — the exact overstatement the partial base set would cause.
    spend=native_call('shield_funding')
    assert spend['height']==105 and spend['change_una']>0,spend
    extend_chain(spend)
    deadline=time.monotonic()+60
    while time.monotonic()<deadline:
        assert not failures,failures
        if call('rpc',method='getblockcount',params=[])==105:break
        time.sleep(.25)
    else:raise AssertionError('late phone did not follow the spend tail to 105')
    spend_recoveries=[];deadline=time.monotonic()+90
    while time.monotonic()<deadline:
        assert not failures,failures
        spend_recovered=call('rpc',method='wallet.recovertransparentfromsnapshot',params=owner)
        spend_recoveries.append(spend_recovered)
        (root/'spend-recoveries.json').write_text(json.dumps(spend_recoveries,indent=2)+'\n')
        if spend_recovered.get('success'):break
        check_refusal(spend_recovered)
        time.sleep(.5)
    else:raise AssertionError(('spend-tail recovery did not complete',spend_recoveries))
    assert spend_recovered['recovered_height']==105 and spend_recovered['scope']==recovered['scope'],spend_recovered
    final_confirmed=spend['change_una']/1e8;final_immature=mined5['amount_una']/1e8
    spend_balance=call('rpc',method='wallet.getbalance',params=[])
    (root/'spend-balance.json').write_text(json.dumps(spend_balance,indent=2)+'\n')
    assert 'error' not in spend_balance,spend_balance
    assert spend_balance['confirmed']==final_confirmed,spend_balance
    assert spend_balance['immature']==final_immature,spend_balance
    rows=wallet_utxos();(root/'spend-wallet-rows.json').write_text(json.dumps(rows,indent=2)+'\n')
    assert len(rows['live'])==2,rows
    spent_rows=[r for r in rows['spent'] if r[0]==fixture['funding_txid']]
    assert len(spent_rows)==1 and spent_rows[0][2]==105,rows
    change_rows=[r for r in rows['live'] if r[0]==spend['spend_txid']]
    assert len(change_rows)==1 and change_rows[0][2]==spend['change_una'] and change_rows[0][3]==105 and not change_rows[0][4],rows
    assert call('rpc',method='wallet.encrypt',params=['stage-d-retention-password'])['success'] is True
    assert call('stop')==0;finish_driver()
    # Fresh offline process: the recovered wallet state must survive a cold
    # restart with no peer, no rewind and no re-recovery.
    stop.set();listener.close()
    for conn in connections:
        try:conn.shutdown(socket.SHUT_RDWR)
        except OSError:pass
    thread.join(timeout=5);assert not thread.is_alive() and not failures,failures
    packet_count=len(packets)
    (datadir/'dinero.conf').write_text('p2p.offline=1\nlisten=0\nrpc.port=0\nwallet.socket.enable=0\nassumeutxo_bg_stall_timeout=3600\n'
        +f'wallet_recovery_v5_source={v5_source}\n')
    replies=root/'cold-replies.jsonl';replies.touch();reply=replies.open()
    output=(root/'cold-nodecore.log').open('xb')
    process=subprocess.Popen([str(driver),str(replies)],stdin=subprocess.PIPE,text=True,stdout=output,stderr=subprocess.STDOUT)
    assert process.pid!=original_pid
    assert call('start',datadir=str(datadir),config=dict(network='regtest',sync_profile='ios_utreexo',
        max_peers=1,verbose_logging=True))==0
    assert call('rpc',method='wallet.open',params=['stage-d-owner'])['success'] is True
    assert call('rpc',method='wallet.unlock',params=['stage-d-retention-password',0])['success'] is True
    cold_balance=call('rpc',method='wallet.getbalance',params=[])
    assert 'error' not in cold_balance,cold_balance
    assert cold_balance['confirmed']==final_confirmed,cold_balance
    assert cold_balance['immature']==final_immature,cold_balance
    assert wallet_utxos()==rows,'cold restart must not rewrite recovered wallet rows'
    assert len(packets)==packet_count,'offline reopen must not require a peer'
    assert call('stop')==0;finish_driver()
    success=True
    (root/'receipt.json').write_text(json.dumps(dict(fixture={k:str(v) for k,v in fixture.items() if k!='headers'},
        early=early,recoveries=recoveries,balance=balance,rows=rows,cold_balance=cold_balance,
        scope='Native late-v6 transparent recovery: composed preserved-V5 base + verified typed tail through wallet.recovertransparentfromsnapshot; v6 scope, exact funded balance, idempotent rerun, offline encrypted cold reopen. Fixture tail carries no owned transparent effects; tail-activity variants extend this harness.'),indent=2)+'\n')
    print('PASS late-phone native transparent wallet recovery (composed V5 base + typed tail) and offline cold reopen',flush=True)
finally:
    stop.set();listener.close()
    for conn in connections:
        try:conn.shutdown(socket.SHUT_RDWR)
        except OSError:pass
    thread.join(timeout=5)
    try:
        if native.poll() is None:
            native.stdin.write(json.dumps(dict(op='stop'))+'\n');native.stdin.flush()
            native.wait(timeout=15)
    except Exception:
        native.kill()
    native_log.close();native_read.close()
    if not process.stdin.closed:process.stdin.close()
    try:process.wait(timeout=30)
    except subprocess.TimeoutExpired:process.kill();process.wait(timeout=5)
    output.close();reply.close();(root/'peer-failures.json').write_text(json.dumps(failures,indent=2)+'\n')
    if success:assert process.returncode==0 and not thread.is_alive() and not failures
