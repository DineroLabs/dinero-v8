"""Funded named-wallet SDK handoff; all traffic/data is isolated and retained.

The default parent has independently replayed FULL backing. The separate
assumed-mobile variant uses normal snapshot import into a new header-only
root, never copied canonical coins, wallet rows or lifecycle keys. This is
a native C ABI acceptance lane, not a device run.
"""
import hashlib,json,os,socket,sqlite3,struct,subprocess,sys,tempfile,threading,time,traceback
from pathlib import Path

root=Path(tempfile.mkdtemp(prefix='sdk-funded-crossing-evidence-')).resolve()
print('SDK funded crossing evidence:',root,flush=True)
driver=Path(os.environ['NODECORE_DRIVER']).resolve()
generator=Path(os.environ['ACTIVATION_FIXTURE_GENERATOR']).resolve()
manifest=root/'fixture.json';native_replies=root/'native-replies.jsonl';native_replies.touch()
native_read=native_replies.open();native_log=(root/'generator.log').open('xb')
native=subprocess.Popen([str(generator),'--export-sdk-activation-fixture'],
    env=dict(os.environ,DINERO_SDK_ACTIVATION_MANIFEST=str(manifest),
        DINERO_SDK_FUNDED_CROSSING_FIXTURE='1',DINERO_SDK_FUNDED_REPLIES=str(native_replies)),
    stdin=subprocess.PIPE,text=True,stdout=native_log,stderr=subprocess.STDOUT)
deadline=time.monotonic()+120
while not manifest.exists():
    assert native.poll() is None,('native fixture failed',native.returncode,root)
    assert time.monotonic()<deadline,('native fixture timed out',root)
    time.sleep(.05)
fixture=json.loads(manifest.read_text());assumed=bool(os.environ.get('DINERO_SDK_ASSUMED_MOBILE_FIXTURE'))
input_queue=bool(os.environ.get('DINERO_SDK_IMPORTED_SHIELD_QUEUE'))
ready_only=bool(os.environ.get('DINERO_SDK_IMPORTED_SHIELD_READY'))
wrong_input_height=bool(os.environ.get('DINERO_SDK_IMPORTED_SHIELD_WRONG_HEIGHT'))
new_compact_reservation=bool(os.environ.get('DINERO_SDK_COMPACT_NEW_RESERVATION'))
new_compact_ready=bool(os.environ.get('DINERO_SDK_COMPACT_NEW_READY'))
if new_compact_ready:assert assumed and new_compact_reservation
reserved_restart=bool(os.environ.get('DINERO_SDK_COMPACT_RESERVED_RESTART'))
if reserved_restart:assert new_compact_ready
ready_restart=bool(os.environ.get('DINERO_SDK_COMPACT_READY_RESTART'))
if ready_restart:assert new_compact_ready and not reserved_restart
post_base_shield=bool(os.environ.get('DINERO_SDK_POST_BASE_SHIELD'))
post_base_discovery_only=bool(os.environ.get('DINERO_SDK_POST_BASE_DISCOVERY_ONLY'))
post_base_source_refusal=bool(os.environ.get('DINERO_SDK_POST_BASE_SOURCE_REFUSAL'))
mixed_balance_refusal=bool(os.environ.get('DINERO_SDK_MIXED_BALANCE_REFUSAL'))
if mixed_balance_refusal:assert post_base_shield and not post_base_discovery_only
mixed_baseline=bool(os.environ.get('DINERO_SDK_MIXED_BASELINE'))
if mixed_baseline:assert post_base_shield and not post_base_discovery_only and not mixed_balance_refusal
if post_base_shield:assert new_compact_ready and not reserved_restart and not ready_restart
assumed_unshield=bool(os.environ.get('DINERO_SDK_ASSUMED_PRIVATE_UNSHIELD'))
if assumed_unshield:assert assumed and os.environ.get('DINERO_SDK_COMPACT_PRIVATE_SEND')=='1' and not new_compact_reservation
native_lock=threading.Lock()
if wrong_input_height:
    assert assumed and input_queue and fixture['assumed_snapshot_version']==4
    # Deliberately corrupt creation metadata with a recomputed file checksum.
    # Regtest fork20 already binds this height101 output in its v2 leaf; the
    # original complete source audit must refuse before discovery/enrollment.
    original_snapshot=Path(fixture['assumed_snapshot']);snapshot=bytearray(original_snapshot.read_bytes())
    assert struct.unpack_from('<I',snapshot,4)[0]==4 and hashlib.sha256(snapshot[:-32]).digest()==snapshot[-32:]
    count=struct.unpack_from('<Q',snapshot,44)[0];offset=68;changed=0
    for _ in range(count):
        key=bytes(snapshot[offset:offset+32]);output=struct.unpack_from('<I',snapshot,offset+32)[0]
        size=struct.unpack_from('<I',snapshot,offset+44)[0];metadata=offset+48+size
        if key==bytes.fromhex(fixture['funding_txid'])[::-1] and output==0:
            assert struct.unpack_from('<I',snapshot,metadata)[0]==101 and snapshot[metadata+4]==0
            struct.pack_into('<I',snapshot,metadata,100);changed+=1
        offset=metadata+5
    assert changed==1
    snapshot[-32:]=hashlib.sha256(snapshot[:-32]).digest()
    altered=root/'wrong-legacy-height-v4.dat';altered.write_bytes(snapshot);fixture['assumed_snapshot']=str(altered)
datadir=Path(fixture['assumed_datadir'] if assumed else fixture['datadir'])
proof=Path(fixture['proof']).read_bytes();assert 0<len(proof)<=1<<30
magic=fixture['magic'];headers=[bytes.fromhex(h) for h in fixture['headers']]
assert len(headers)==104 and all(len(h)==128 for h in headers)
stop=threading.Event();future=threading.Event();bodies_ready=threading.Event();proof_ready=threading.Event();echo_ready=threading.Event()
funding_release=threading.Event();historical_release=threading.Event();funding_pending={}
funding_bodies={hashlib.sha256(hashlib.sha256(Path(path).read_bytes()[:128]).digest()).digest()[::-1]:Path(path).read_bytes()
    for path in fixture.get('funding_bodies',{}).values()}
historical_only={key for key in funding_bodies if mixed_baseline and key not in
    {bytes.fromhex(fixture['parent'])[::-1],hashlib.sha256(hashlib.sha256(Path(fixture['funding_bodies']['100']).read_bytes()[:128]).digest()).digest()[::-1]}}
packets=[];failures=[];connections=[];pending={};handshaken={};sent_headers=set();sent_proof=set();asked_echo=set();echo=[]
body_requests=[];proof_wire=b'';txid=''
bodies={bytes.fromhex(fixture[name+'_hash'])[::-1]:Path(fixture[name]).read_bytes() for name in ('first','second')}
listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen(4);listener.settimeout(.5)
port=listener.getsockname()[1]
def double_sha(data):return hashlib.sha256(hashlib.sha256(data).digest()).digest()
def record(direction,command,payload):
    path=root/f'packet-{len(packets):05d}-{direction}-{command}.bin';path.write_bytes(payload)
    packets.append(dict(direction=direction,command=command,size=len(payload),file=path.name,sha256=hashlib.sha256(payload).hexdigest()))
    (root/'packets.json').write_text(json.dumps(packets,indent=2)+'\n')
def send(conn,command,payload=b''):
    record('out',command,payload)
    conn.sendall(struct.pack('<I12sI4s',magic,command.encode(),len(payload),double_sha(payload)[:4])+payload)
def flush(conn):
    if not handshaken.get(conn):return
    if funding_release.is_set():
        for key in list(funding_pending.get(conn,{})):
            if key in historical_only and not historical_release.is_set():continue
            send(conn,'block',funding_bodies[key]);del funding_pending[conn][key]
    if future.is_set() and (conn,len(headers)) not in sent_headers:
        send(conn,'headers',bytes([len(headers)])+b''.join(h+b'\0' for h in headers));sent_headers.add((conn,len(headers)))
    if proof_ready.is_set() and conn not in sent_proof:
        send(conn,'utxotx',proof_wire);sent_proof.add(conn)
    if echo_ready.is_set() and conn not in asked_echo:
        send(conn,'getdata',b'\x01'+struct.pack('<I',0x50000001)+bytes.fromhex(txid)[::-1]);asked_echo.add(conn)
    if bodies_ready.is_set():
        for key in list(pending.get(conn,{})):
            send(conn,'block',bodies[key]);del pending[conn][key]
def exact(conn,size):
    out=bytearray()
    while len(out)<size:
        flush(conn)
        try:part=conn.recv(size-len(out))
        except socket.timeout:
            if stop.is_set():raise EOFError()
            continue
        if not part:raise EOFError()
        out.extend(part)
    return bytes(out)
def inventory(payload):
    assert payload;prefix=payload[0];width={253:2,254:4,255:8}.get(prefix,0)
    count=int.from_bytes(payload[1:1+width],'little') if width else prefix;offset=1+width
    assert count<=50000 and len(payload)==offset+36*count
    return [struct.unpack_from('<I32s',payload,offset+36*i) for i in range(count)]
def version():
    services=1|(1<<29)|(1<<30)|(1<<31)
    address=struct.pack('<Q',services)+b'\0'*10+b'\xff\xff\x7f\0\0\x01'+struct.pack('>H',port)
    agent=b'/isolated-funded-sdk-peer/'
    return struct.pack('<iQq',70016,services,int(time.time()))+address+address+struct.pack('<Q',0x842b193a)+bytes([len(agent)])+agent+struct.pack('<iB',103 if future.is_set() else 101,0)
def serve():
    try:
        while not stop.is_set():
            try:conn,_=listener.accept()
            except socket.timeout:continue
            except OSError:
                if stop.is_set():return
                raise
            connections.append(conn);pending[conn]={};funding_pending[conn]={};conn.settimeout(.5);transfer=None
            with conn:
                while not stop.is_set():
                    try:raw=exact(conn,24)
                    except (EOFError,ConnectionResetError):break
                    received,command,size,checksum=struct.unpack('<I12sI4s',raw)
                    assert received==magic and size<=2*1024*1024
                    payload=exact(conn,size);assert double_sha(payload)[:4]==checksum
                    command=command.rstrip(b'\0').decode('ascii');record('in',command,payload)
                    if command=='version':send(conn,'version',version());send(conn,'verack')
                    elif command=='verack':handshaken[conn]=True
                    elif command=='ping':send(conn,'pong',payload)
                    elif command=='getheaders':
                        selected=headers if future.is_set() else headers[:102]
                        send(conn,'headers',bytes([len(selected)])+b''.join(h+b'\0' for h in selected))
                        if future.is_set():sent_headers.add((conn,len(headers)))
                    elif command=='getocat':
                        assert future.is_set() and len(payload)==151 and payload[:6]==b'DNOCT\x01'
                        assert payload[87:119]==bytes.fromhex(fixture['parent'])[::-1]
                        assert payload[119:151]==bytes.fromhex(fixture['first_hash'])[::-1] and payload[43:75]!=bytes(32)
                        description=payload+struct.pack('<QI',len(proof),65536)+hashlib.sha256(proof).digest()
                        transfer=hashlib.sha256(description).digest();send(conn,'ocatmeta',description)
                    elif command=='getocatpg':
                        assert transfer and len(payload)==42 and payload[:6]==b'DNOPG\x01' and payload[6:38]==transfer
                        index=struct.unpack_from('<I',payload,38)[0];page=proof[index*65536:(index+1)*65536]
                        assert page;send(conn,'ocatpage',transfer+struct.pack('<II',index,len(page))+page)
                    elif command=='getwinput':
                        assert new_compact_reservation and future.is_set() and len(payload)==190
                        assert payload[:6]==b'DNWIP\x01' and payload[122:154]!=bytes(32)
                        data=native_call('wallet_input',request=payload.hex())
                        send(conn,'winput',bytes.fromhex(data['reply']))
                    elif command=='getdata':
                        for kind,key in inventory(payload):
                            body_requests.append(dict(kind=kind,hash=key[::-1].hex()))
                            if assumed and key in funding_bodies:
                                assert kind==2
                                # Do not retain scheduler's old pre-base DATA
                                # requests for a later unsolicited burst. Only
                                # the explicit acquisition phase gets bodies.
                                if key not in historical_only or historical_release.is_set():funding_pending[conn][key]=True
                            elif key in bodies:
                                assert kind==2;pending[conn][key]=True
                            elif not future.is_set() and txid and key==bytes.fromhex(txid)[::-1] and kind==0x50000001:send(conn,'utxotx',proof_wire)
                        flush(conn)
                    elif command=='utxotx':echo.append(payload)
    except BaseException:
        if not stop.is_set():failures.append(traceback.format_exc())
thread=threading.Thread(target=serve);thread.start()
(datadir/'dinero.conf').write_text('listen=0\nrpc.port=0\nwallet.socket.enable=0\nutreexo.checkpoint_interval=1000\nassumeutxo_bg_stall_timeout=3600\n')
replies=root/'replies.jsonl';replies.touch();reply=replies.open();output=(root/'nodecore.log').open('xb')
process=subprocess.Popen([str(driver),str(replies)],stdin=subprocess.PIPE,text=True,stdout=output,stderr=subprocess.STDOUT)
def response(reader,child,op,timeout=90):
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        line=reader.readline()
        if line:
            value=json.loads(line);assert not isinstance(value,dict) or 'driver_error' not in value,value
            return value
        assert child.poll() is None,(op,child.returncode,root)
        time.sleep(.05)
    raise AssertionError(('operation timed out',op,root))
def call(op,**args):
    process.stdin.write(json.dumps(dict(op=op,**args))+'\n');process.stdin.flush();return response(reply,process,op)
def restart_sdk_process(stage):
    global process,reply,output,replies
    old_pid=process.pid
    assert call('stop')==0
    process.stdin.close();process.wait(timeout=30)
    assert process.returncode==0
    output.close();reply.close()
    replies=root/(stage+'-cold-replies.jsonl');replies.touch();reply=replies.open()
    output=(root/(stage+'-cold-nodecore.log')).open('xb')
    process=subprocess.Popen([str(driver),str(replies)],stdin=subprocess.PIPE,text=True,stdout=output,stderr=subprocess.STDOUT)
    assert process.pid!=old_pid
    assert call('sdk_activation_events') is True
    assert call('watch_script',script=fixture['funding_change_script'])==0
    # Completed compact startup must consume its real stored original cohort,
    # not re-import the old snapshot or carry any executor/result into the PID.
    cold_config={k:v for k,v in config.items() if k!='assumeutxo_snapshot'}
    assert call('start',datadir=str(datadir),config=cold_config)==0
    return dict(old_pid=old_pid,new_pid=process.pid,old_exit=0,replies=str(replies),
        log=str(root/(stage+'-cold-nodecore.log')),snapshot_reimport=False)
def rpc(method,params):
    value=call('rpc',method=method,params=params)
    assert not isinstance(value,dict) or 'error' not in value,(method,value,root)
    return value
def native_call(op,**args):
    with native_lock:
        native.stdin.write(json.dumps(dict(op=op,**args))+'\n');native.stdin.flush();return response(native_read,native,op)
def wallet_rows(name='wire-named-owner'):
    paths=list(datadir.rglob('wallet_'+name+'.db'));assert len(paths)==1,paths
    with sqlite3.connect('file:'+str(paths[0])+'?mode=ro',uri=True) as db:
        return {name:db.execute('SELECT hex(wallet_id),account,revision,hex(sealed) FROM '+name+' ORDER BY wallet_id,account,revision').fetchall()
                for name in ('orchard_wallet_snapshots','orchard_wallet_retained')}
def bound(name):
    return dict(wallet_binding=rpc('wallet.orchard.getwalletbinding',dict(wallet_name=name))['wallet_binding'])
success=False;config=dict(network='regtest',sync_profile='ios_utreexo',addnode=f'127.0.0.1:{port}',max_peers=1,verbose_logging=True)
if assumed:config['assumeutxo_snapshot']=fixture['assumed_snapshot']
def original_lifecycle():
    with sqlite3.connect('file:'+str(datadir/'blockchain/utxo')+'?mode=ro',uri=True) as db:
        return dict(db.execute("SELECT key,value FROM utxo_metadata WHERE substr(key,1,11)='assumeutxo_' OR key='wallet_snapshot_recovery_base_height' ORDER BY key"))
def mixed_runtime_receipts(name='wire-named-owner'):
    # Read-only distinction: real Orchard delivery may NOT manufacture either
    # ordinary or index baseline, including by adding a receipt column/schema.
    with sqlite3.connect('file:'+str(datadir/'blockchain/utxo')+'?mode=ro',uri=True) as db:
        indexed=db.execute("SELECT key,hex(value) FROM utxo_metadata WHERE key='runtime_delivery:v1:receipt'").fetchall()
    paths=list(datadir.rglob('wallet_'+name+'.db'));assert len(paths)==1
    with sqlite3.connect('file:'+str(paths[0])+'?mode=ro',uri=True) as db:
        columns=[row[1] for row in db.execute('PRAGMA table_info(wallet_meta)')]
        ordinary=db.execute('SELECT hex(runtime_ordinary_receipt),runtime_ordinary_invalid FROM wallet_meta WHERE id=1').fetchall() if 'runtime_ordinary_receipt' in columns else None
    return dict(indexed=indexed,ordinary=ordinary,receipt_columns=[c for c in columns if c.startswith('runtime_ordinary_')])
def candidate_wallet_state():
    paths=list(datadir.rglob('wallet_wire-named-owner.db'));assert len(paths)==1,paths
    with sqlite3.connect('file:'+str(paths[0])+'?mode=ro',uri=True) as db:
        encoded=lambda row:[v.hex() if isinstance(v,bytes) else v for v in row]
        tables={name:[encoded(row) for row in db.execute('SELECT * FROM '+name+' ORDER BY rowid')]
                for name in ('utxos','sync_meta','tip')}
        tables['catalog']=db.execute("SELECT value FROM settings WHERE key='orchard_account_catalog_v1'").fetchall()
        return tables
def snapshot_candidate_rows():
    # Separate DATA inventory: successful discovery legitimately adds these
    # rows, unlike the ordinary coin/progress/tip/catalog invariant above.
    paths=list(datadir.rglob('wallet_wire-named-owner.db'));assert len(paths)==1
    with sqlite3.connect('file:'+str(paths[0])+'?mode=ro',uri=True) as db:
        exists=db.execute("SELECT 1 FROM sqlite_schema WHERE type='table' AND name='wallet_snapshot_candidates'").fetchone()
        return [list(row) for row in db.execute('SELECT * FROM wallet_snapshot_candidates ORDER BY rowid')] if exists else []
try:
    assert call('sdk_activation_events') is True
    assert call('watch_script',script=fixture['funding_change_script'])==0
    assert call('start',datadir=str(datadir),config=config)==0
    if assumed:
        deadline=time.monotonic()+45
        while time.monotonic()<deadline:
            if call('rpc',method='getblockcount',params=[])==101:break
            time.sleep(.25)
        original=original_lifecycle();(root/'original-lifecycle.json').write_text(json.dumps(original,indent=2)+'\n')
        assert original['assumeutxo_active']=='true' and original['assumeutxo_base_height']=='101',original
        assert original['assumeutxo_base_block']==fixture['parent'] and original.get('assumeutxo_fully_validated')!='true',original
    assert rpc('getblockcount',[])==101
    restored=rpc('wallet.restore',dict(name='wire-named-owner',mnemonic='abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about',rescan=assumed))
    assert restored['success'] is True
    (root/'restored.json').write_text(json.dumps(restored,indent=2)+'\n')
    if os.environ.get('DINERO_SDK_ASSUMED_RESCAN_FENCE'):
        # A separate fail-closed gate, NOT the funded crossing. The genuine
        # imported selected parent is101 while independent/durable tip is0.
        # A scan of genesis must never be reported as a102-block history scan.
        assert assumed and restored.get('rescan_success') is False,restored
        assert 'Historical rescan source is not the selected active tip' in restored.get('rescan_error',''),restored
        assert 'rescan' not in restored,'refused scan published a completion result'
        assert original_lifecycle()==original,'rescan refusal changed original assumed lifecycle'
        assert rpc('wallet.listunspent',[])==[],'refused scan invented funding'
        (root/'receipt.json').write_text(json.dumps(dict(original_assumed_mobile=True,
            original_lifecycle=original,restore=restored,false_completion_refused=True,
            funded_crossing=False,scope='Actual normal imported SDK named restore refuses historical scan beyond its durable source. No funding discovery, queue, handoff, app/device or release acceptance.'),indent=2)+'\n')
        assert call('stop')==0;success=True
        print('PASS actual imported SDK refuses false historical rescan completion without assumed lifecycle changes',flush=True)
        sys.exit(0)
    deadline=time.monotonic()+20
    while time.monotonic()<deadline:
        state=call('sdk_activation_state');assert not failures,failures
        if state.get('converged') and not state.get('ibd'):break
        time.sleep(.25)
    assert state.get('converged') and not state.get('ibd'),state
    if assumed:
        # Use only the real restore/unlock discovery path. A snapshot can
        # discover candidate coins, not certify that absent historical bodies
        # were scanned. The later selected-source reservation must still
        # authenticate the exact inputs; a wallet row is not coin authority.
        assert rpc('wallet.encrypt',['funded-crossing-password'])['success'] is True
        source_refusal=os.environ.get('DINERO_SDK_SNAPSHOT_CANDIDATE_REFUSAL')
        if source_refusal:
            assert source_refusal=='content-commitment'
            with sqlite3.connect(datadir/'blockchain/utxo') as db:
                assert db.execute("UPDATE utxo_metadata SET value=? WHERE key='assumeutxo_expected_commitment'",('f'*64,)).rowcount==1
            original=original_lifecycle() # Retain the intentional bad value; no repair/retry.
        before_discovery=candidate_wallet_state()
        unlocked=rpc('wallet.unlock',['funded-crossing-password',0]);assert unlocked['success'] is True
        candidates=rpc('wallet.listunspent',[])
        (root/'snapshot-funding.json').write_text(json.dumps(dict(restored=restored,unlocked=unlocked,candidates=candidates),indent=2)+'\n')
        if source_refusal:
            assert candidates==[],'invalid snapshot cohort was silently accepted/repaired'
            assert 'Snapshot funding candidates discovered' not in unlocked.get('recovery_warning',''),unlocked
        else:
            if wrong_input_height:
                # In this fixed regtest profile, height101 is ALREADY a v2
                # maturity-bound leaf (fork20). The complete source audit must
                # refuse this altered file BEFORE discovery/enrollment. Do not
                # call this a reached creating-body comparison negative.
                assert candidates==[] and 'Snapshot funding candidates discovered' not in unlocked.get('recovery_warning',''),unlocked
                assert candidate_wallet_state()==before_discovery and original_lifecycle()==original
                assert call('stop')==0;success=True
                (root/'receipt.json').write_text(json.dumps(dict(original_assumed_mobile=True,wrong_snapshot_height=100,
                    actual_creation_height=101,original_lifecycle=original,nonempty_queue=False,
                    source_leaf_refusal=True,creation_body_check_reached=False,
                    scope='Correct-checksum altered v4 metadata refuses in the unchanged complete maturity-bound leaf audit before discovery/enrollment. NOT a reached legacy creating-body/maturity-negative, queue, handoff, app/device or release gate.'),indent=2)+'\n')
                print('PASS maturity-bound imported source refuses altered metadata before discovery',flush=True)
                sys.exit(0)
            found=[c for c in candidates if c.get('txid')==fixture['funding_txid']]
            assert len(found)==(2 if new_compact_reservation else 1),'normal snapshot restore/unlock did not discover actual named-wallet funding'
            assert sum(c['amount_una'] for c in found)==1000000,found
            assert all(c['snapshot_candidate'] and not c['spendable'] and not c['safe'] and c['confirmations']==0 for c in found),found
            assert 'canonical history/index recovery remains deferred' in unlocked['recovery_warning'],unlocked
        assert candidate_wallet_state()==before_discovery,'candidate discovery altered ordinary coins/progress/tip/catalog'
        assert original_lifecycle()==original,'funding discovery changed original assumed lifecycle'
        if os.environ.get('DINERO_SDK_SNAPSHOT_CANDIDATES_ONLY'):
            assert call('stop')==0;success=True
            (root/'receipt.json').write_text(json.dumps(dict(original_assumed_mobile=True,original_lifecycle=original,
                candidates=candidates,source_refusal=source_refusal,ordinary_state=before_discovery,unlocked=unlocked,
                funded_crossing=False,scope='Actual normal imported SDK encrypted named-wallet unlock discovers checked source-bound candidate DATA, or refuses a mismatched original content commitment. Ordinary coins/progress/tip/catalog unchanged; no coin authority, queue, history/index baseline, handoff/app/device or release acceptance.'),indent=2)+'\n')
            print('PASS actual snapshot candidate '+('source refusal' if source_refusal else 'discovery without spend/history authority'),flush=True)
            sys.exit(0)
    else:
        scanned=rpc('wallet.rescanblockchain',[0,101]);assert scanned['scanned_blocks']==102,scanned
    addresses=rpc('wallet.listaddresses',[])
    assert fixture['funding_address'] in json.dumps(addresses),addresses
    # Funding was genuinely signed/mined in historical101 and found normally;
    # neither this script nor the source helper writes a wallet coin/HD path.
    binding=bound('wire-named-owner')
    if os.environ.get('DINERO_SDK_IMPORTED_ENROLLMENT_ONLY'):
        assert assumed
        before_enrollment=candidate_wallet_state();before_candidates=rpc('wallet.listunspent',[])
        refusal=os.environ.get('DINERO_SDK_IMPORTED_ENROLLMENT_REFUSAL')
        if refusal=='content-commitment':
            with sqlite3.connect(datadir/'blockchain/utxo') as db:
                assert db.execute("UPDATE utxo_metadata SET value=? WHERE key='assumeutxo_expected_commitment'",('f'*64,)).rowcount==1
            original=original_lifecycle() # Deliberately retain bad input; no repair/retry.
        elif refusal=='missing-catalog':
            paths=list(datadir.rglob('wallet_wire-named-owner.db'));assert len(paths)==1
            with sqlite3.connect(paths[0]) as db:
                assert db.execute("DELETE FROM settings WHERE key='orchard_account_catalog_v1'").rowcount==1
            before_enrollment=candidate_wallet_state()
        else:assert not refusal,refusal
        enrolled=call('rpc',method='wallet.orchard.enrollpreactivation',params=binding)
        account=None;inventory=None;owners=None
        if refusal:
            assert enrolled.get('error_code')=='preactivation_enrollment_unavailable',enrolled
            assert candidate_wallet_state()==before_enrollment,'refusal repaired/changed wallet state or catalog'
        else:
            assert enrolled['enrolled'] and enrolled['checkpoint_height']==101 and enrolled['checkpoint_hash']==fixture['parent'],enrolled
            assert not enrolled['scan_complete_to_current_tip'],enrolled
            account=rpc('wallet.orchard.createaccount',dict(binding,account=0))
            assert account['revision']==1 and account['address'],account
            inventory=rpc('wallet.orchard.listaccounts',binding)
            assert inventory['captured_source_sequence']==0 and inventory['captured_source_digest']=='0'*64,inventory
            assert len(inventory['accounts'])==1 and inventory['accounts'][0]['account']==0,inventory
            assert inventory['accounts'][0]['account_sequence']==0 and inventory['accounts'][0]['checkpoint_height']==101,inventory
            owners=wallet_rows();assert owners['orchard_wallet_snapshots'] and not owners['orchard_wallet_retained'],owners
            assert rpc('wallet.open',['wire-named-owner'])['success'] is True
            assert rpc('wallet.unlock',['funded-crossing-password',0])['success'] is True
            binding=bound('wire-named-owner')
            assert rpc('wallet.orchard.listaccounts',binding)==inventory
            assert wallet_rows()==owners,'normal named open/unlock changed encrypted account owner'
            after_enrollment=candidate_wallet_state()
            for name in ('utxos','sync_meta','tip'):assert after_enrollment[name]==before_enrollment[name],name
            assert after_enrollment['catalog']!=before_enrollment['catalog'],'no actual preactivation catalog was written'
        assert original_lifecycle()==original,'enrollment changed original assumed lifecycle'
        assert rpc('wallet.listunspent',[])==before_candidates,'enrollment promoted/changed candidate DATA'
        assert call('stop')==0;success=True
        (root/'receipt.json').write_text(json.dumps(dict(original_assumed_mobile=True,original_lifecycle=original,
            refusal=refusal,enrolled=enrolled,account=account,inventory=inventory,owners=owners,
            candidates=before_candidates,named_wallet_without_phrase=not refusal,funded_crossing=False,
            scope='Actual imported named recovered wallet enrolls domain/seed/wallet/checkpoint-bound empty preactivation history and creates/reopens encrypted account0 at real zero cursor, or refuses invalid cohort/missing catalog without repair. No independent history, ordinary coin/input, queue/Ready, app/device or release authority.'),indent=2)+'\n')
        print('PASS actual imported account enrollment '+('refusal '+refusal if refusal else 'and named encrypted account0 reopen without coin authority'),flush=True)
        sys.exit(0)
    enrolled=rpc('wallet.orchard.enrollpreactivation',binding)
    assert enrolled['checkpoint_height']==101,enrolled
    account=rpc('wallet.orchard.createaccount',dict(binding,account=0))
    if not assumed:assert rpc('wallet.encrypt',['funded-crossing-password'])['success'] is True
    assert rpc('wallet.unlock',['funded-crossing-password',0])['success'] is True
    binding=bound('wire-named-owner');request_id='71'+'00'*31
    request=dict(binding,account=0,request_id=request_id,expected_revision=account['revision'],fee_una=10000,
        payments=[dict(address=account['address'],amount_una=20000,memo_hex=b'funded queued crossing'.hex())])
    if input_queue:
        assert assumed and not future.is_set()
        before_queue=wallet_rows();before_state=candidate_wallet_state();before_coins=rpc('wallet.listunspent',[])
        before_candidate_rows=snapshot_candidate_rows()
        pending_reply=call('rpc',method='wallet.orchard.queueshield',params=request)
        assert pending_reply.get('error_code')=='funding_source_pending' and pending_reply.get('retryable') is True,pending_reply
        assert wallet_rows()==before_queue and candidate_wallet_state()==before_state,'pending source mutated wallet owner/selection'
        assert snapshot_candidate_rows()==before_candidate_rows
        funding_release.set();deadline=time.monotonic()+30
        while time.monotonic()<deadline:
            queued=call('rpc',method='wallet.orchard.queueshield',params=request)
            if queued.get('error_code')!='funding_source_pending':break
            assert wallet_rows()==before_queue and candidate_wallet_state()==before_state
            time.sleep(.1)
    elif assumed:
        # Real input DATA may need its ordinary asynchronous getdata flight.
        # No other source/owner/refusal is retried or reclassified as success.
        funding_release.set();deadline=time.monotonic()+30
        while time.monotonic()<deadline:
            queued=call('rpc',method='wallet.orchard.queueshield',params=request)
            if queued.get('error_code')!='funding_source_pending':break
            assert queued.get('retryable') is True,queued;time.sleep(.1)
    else:queued=rpc('wallet.orchard.queueshield',request)
    assert queued.get('proof_queued') and queued['input_count']>0,queued
    if input_queue:
        assert queued['input_count']==1 and not queued['existing_request'],queued
        reserved=wallet_rows();assert reserved!=before_queue and reserved['orchard_wallet_retained']
        retry=rpc('wallet.orchard.queueshield',request)
        assert retry['existing_request'] and not retry['proof_queued'] and wallet_rows()==reserved,retry
        finished=None;signed=None;ready_rows=reserved
        if ready_only:
            deadline=time.monotonic()+30
            while time.monotonic()<deadline:
                finished=call('rpc',method='wallet.orchard.finishshield',params=dict(binding,account=0,request_id=request_id))
                if finished.get('error_code')!='proof_not_ready':break
                assert finished.get('proof_state') in ('queued','running'),finished;time.sleep(.1)
            assert finished.get('durable_state')=='signed' and finished.get('admitted'),finished
            signed=rpc('wallet.getrawtransaction',[finished['txid']])['hex'];assert signed
            observed=call('sdk_activation_state')['observed_transaction']
            assert observed['txid']==finished['txid'] and observed['format']=='orchard',observed
            # The ordinary independent producer checks the ACTUAL signed body
            # and builds its untrusted proof; no wallet coins or Ready injected.
            proof_result=native_call('proof',body=signed)
            assert proof_result['txid']==finished['txid'] and proof_result['proof'],proof_result
            ready_rows=wallet_rows();assert ready_rows!=reserved and ready_rows['orchard_wallet_retained']
            refinished=rpc('wallet.orchard.finishshield',dict(binding,account=0,request_id=request_id))
            assert refinished['already_in_mempool'] and refinished['txid']==finished['txid'],refinished
            assert wallet_rows()==ready_rows
        assert rpc('wallet.open',['wire-named-owner'])['success'] is True
        assert rpc('wallet.unlock',['funded-crossing-password',0])['success'] is True
        binding=bound('wire-named-owner');retry=rpc('wallet.orchard.queueshield',dict(request,**binding))
        assert retry['existing_request'] and not retry['proof_queued'] and wallet_rows()==ready_rows,retry
        if ready_only:
            reopened=rpc('wallet.orchard.finishshield',dict(binding,account=0,request_id=request_id))
            assert reopened['already_in_mempool'] and reopened['txid']==finished['txid'] and wallet_rows()==ready_rows,reopened
        after_state=candidate_wallet_state()
        for table in ('utxos','sync_meta','tip'):assert after_state[table]==before_state[table],table
        assert snapshot_candidate_rows()==before_candidate_rows,'Ready/admission changed candidate DATA'
        assert original_lifecycle()==original
        # Candidate DATA stays byte-identical. The ordinary existing mempool
        # input-spent filter must hide it after real admission, not resurrect
        # an already-reserved input merely because it came from a snapshot.
        after_coins=rpc('wallet.listunspent',[])
        assert after_coins==([] if ready_only else before_coins),after_coins
        assert not future.is_set() and not (datadir/'blockchain/.orchard-handoff-selected-v1.done').exists()
        assert any(b['hash']==fixture['parent'] and b['kind']==2 for b in body_requests),body_requests
        assert call('stop')==0;success=True
        (root/'receipt.json').write_text(json.dumps(dict(original_assumed_mobile=True,queued=queued,retry=retry,
            pending=pending_reply,reserved=reserved,finished=finished,signed=signed,ready_rows=ready_rows,original_lifecycle=original,
            nonempty_queue=True,signed_ready_and_pool=ready_only,funded_crossing=False,
            scope='Actual normally imported named wallet acquires historical creation inclusion plus current selected membership and atomically reserves a real funded shield request. Ready variant uses actual prover/key signing, freshly recaptured exact-input transaction authorization, encrypted Ready commit and ordinary pool admission; independent producer checks same signed body. No-phrase reopen and exact queue/finish retries preserve encrypted owners. Original assumed SQL and ordinary coins unchanged; no independent history, first block/Event1, handoff/confirmation, app/device or release claim.'),indent=2)+'\n')
        print('PASS actual authenticated imported '+('signed Ready and pool with exact retries' if ready_only else 'nonempty input queue'),flush=True)
        sys.exit(0)
    deadline=time.monotonic()+30
    while time.monotonic()<deadline:
        finished=call('rpc',method='wallet.orchard.finishshield',params=dict(binding,account=0,request_id=request_id))
        if isinstance(finished,dict) and finished.get('admitted'):break
        # Only normal owned proof work may still be pending. Never drop a Core
        # refusal or manufacture a successful Ready/admission result.
        assert isinstance(finished,dict) and finished.get('error_code')=='proof_not_ready' and finished.get('proof_state') in ('queued','running'),finished
        time.sleep(.25)
    assert finished.get('admitted'),finished
    txid=finished['txid'];signed=rpc('wallet.getrawtransaction',[txid])['hex'];assert isinstance(signed,str) and signed
    notification=call('sdk_activation_state')['observed_transaction']
    assert notification['txid']==txid and notification['format']=='orchard',notification
    change_amount=470000 if new_compact_reservation else 970000
    assert any(o['script']==fixture['funding_change_script'] and o['amount']==change_amount for o in notification['outputs']),notification
    untrusted=native_call('proof',body=signed);assert untrusted['txid']==txid,untrusted
    proof_wire=bytes.fromhex(untrusted['proof']);proof_ready.set()
    deadline=time.monotonic()+15
    while time.monotonic()<deadline:
        state=call('sdk_activation_state')
        if any(p['txid']==txid and p['cached_proof_bytes'] for p in state.get('pool',[])):break
        time.sleep(.1)
    assert any(p['txid']==txid and p['cached_proof_bytes'] for p in state.get('pool',[])),state
    before=wallet_rows();assert before['orchard_wallet_snapshots'] and before['orchard_wallet_retained']
    mixed_before=mixed_runtime_receipts() if assumed else None
    if assumed:assert not mixed_before['indexed'] and mixed_before['ordinary'] is None,mixed_before
    (root/'pending-before.json').write_text(json.dumps(dict(rows=before,queued=queued,finished=finished,signed=signed,state=state),indent=2)+'\n')
    proof_ready.clear();future.set();deadline=time.monotonic()+70
    completed=datadir/'blockchain/.orchard-handoff-selected-v1.done'
    while time.monotonic()<deadline:
        assert not failures,failures;state=call('sdk_activation_state')
        (root/'sdk-state.json').write_text(json.dumps(state,indent=2)+'\n')
        assert not state.get('shutdown_requested'),state
        if completed.exists() and state.get('running'):break
        time.sleep(.25)
    assert completed.exists() and state.get('running'),state
    assert state['pause_probes']==1 and state['pause_refusals']==0,state
    assert rpc('wallet.getwalletinfo',[])['walletname']=='wire-named-owner'
    assert rpc('wallet.open',['wire-named-owner'])['success'] is True
    assert rpc('wallet.unlock',['funded-crossing-password',0])['success'] is True
    assert wallet_rows()==before,'encrypted current/retained pending rows changed during parent handoff'
    if assumed:assert original_lifecycle()==original,'handoff changed original assumed lifecycle'
    assert txid in rpc('mempool.getrawmempool',[]),'actual proof readmission did not preserve nonempty pool'
    assert rpc('wallet.getrawtransaction',[txid])['hex']==signed
    binding=bound('wire-named-owner');operations=rpc('wallet.orchard.listoperations',dict(binding,account=0))
    assert any(o['operation_id']==request_id and o['txid']==txid and o['durable_state']=='signed' for o in operations['operations']),operations
    # A committed request is not new compact coin selection. Retry through the
    # real public API after the automatic handoff, with a fresh wallet binding.
    retry_request=dict(request,**binding)
    retried=rpc('wallet.orchard.queueshield',retry_request)
    assert retried['existing_request'] and not retried['proof_queued'] and retried['durable_state']=='signed',retried
    assert wallet_rows()==before,'exact queue retry changed encrypted owners'
    conflicting=dict(retry_request,payments=[dict(request['payments'][0],memo_hex=b'different request'.hex())])
    refused=call('rpc',method='wallet.orchard.queueshield',params=conflicting)
    assert isinstance(refused,dict) and 'error' in refused,refused
    conflict_refusal=refused
    assert wallet_rows()==before,'conflicting retry changed encrypted owners'
    finish_request=dict(binding,account=0,request_id=request_id)
    proof_refusals=[]
    for name,payload in [('missing',b''),('invalid',proof_wire[:-1]+bytes([proof_wire[-1]^1]))]:
        assert call('sdk_activation_set_proof',txid=txid,payload=payload.hex()) is True
        refused=call('rpc',method='wallet.orchard.finishshield',params=finish_request)
        assert isinstance(refused,dict) and 'error' in refused,(name,refused)
        assert wallet_rows()==before,'unproved Ready retry changed encrypted owners'
        assert txid in rpc('mempool.getrawmempool',[]),'refusal discarded the retained request'
        proof_refusals.append(dict(control=name,result=refused))
    assert call('sdk_activation_set_proof',txid=txid,payload=proof_wire.hex()) is True
    refinished=rpc('wallet.orchard.finishshield',finish_request)
    assert refinished['already_in_mempool'] and refinished['txid']==txid,refinished
    assert rpc('wallet.getrawtransaction',[txid])['hex']==signed
    assert wallet_rows()==before,'proof-backed Ready retry changed encrypted owners'
    (root/'compact-retries.json').write_text(json.dumps(dict(queue=retried,conflict=conflict_refusal,
        proof_controls=proof_refusals,finish=refinished),indent=2)+'\n')
    journals=list(datadir.glob('.core-reopen-candidates-v1-*/candidates.sealed'));assert len(journals)==1,journals
    journal=journals[0].read_bytes();assert hashlib.sha256(journal[:-32]).digest()==journal[-32:]
    assert int.from_bytes(journal[-48:-40],'little')==1 and bytes.fromhex(signed) in journal and proof_wire in journal
    echo_ready.set();deadline=time.monotonic()+10
    while not echo and time.monotonic()<deadline:assert not failures,failures;time.sleep(.1)
    assert proof_wire in echo,'reopened compact peer did not serve its actually readmitted proof/body'
    cohort_refusal=bool(os.environ.get('DINERO_SDK_IMPORTED_DELIVERY_COHORT_REFUSAL'))
    if cohort_refusal:
        assert assumed and not input_queue
        # Normal lock excludes new encrypted account writes before this explicit
        # isolated negative corruption. Never repair or reuse this failed source.
        assert rpc('wallet.lock',[])['success'] is True
        refusal_rows=wallet_rows()
        with sqlite3.connect(datadir/'blockchain/utxo') as db:
            assert db.execute("UPDATE utxo_metadata SET value=? WHERE key='assumeutxo_expected_commitment'",('f'*64,)).rowcount==1
        bad_cohort=original_lifecycle();assert bad_cohort!=original
        assert rpc('wallet.unlock',['funded-crossing-password',0])['success'] is True
    mined=native_call('mine');assert txid in mined['transactions'],mined
    bodies[bytes.fromhex(mined['hash'])[::-1]]=bytes.fromhex(mined['body'])
    headers.append(bytes.fromhex(mined['header']));bodies_ready.set();deadline=time.monotonic()+60
    while time.monotonic()<deadline:
        assert not failures,failures
        if rpc('getblockcount',[])==104:break
        time.sleep(.25)
    assert rpc('getblockcount',[])==104
    if cohort_refusal:
        deadline=time.monotonic()+5
        while time.monotonic()<deadline:
            operations=rpc('wallet.orchard.listoperations',dict(binding,account=0))
            assert not any(o.get('chain_observation') and o['chain_observation']['outcome']=='confirmed' for o in operations['operations']),operations
            assert wallet_rows()==refusal_rows,'invalid imported cohort advanced an encrypted account'
            time.sleep(.25)
        assert original_lifecycle()==bad_cohort,'invalid imported metadata was repaired'
        assert mixed_runtime_receipts()==mixed_before,'invalid source minted mixed receipts'
        (root/'receipt.json').write_text(json.dumps(dict(original_assumed_mobile=True,
            actual_sdk_height=104,invalid_cohort_retained=bad_cohort,account_rows_unchanged=refusal_rows,
            mixed_receipts_unchanged=mixed_before,operations=operations,funded_crossing=False,
            scope='Actual imported nonempty queue/Ready/handoff survives; a deliberately corrupted ORIGINAL cohort after handoff refuses automatic account delivery through real104. No repair, confirmed funds or whole-crossing PASS.'),indent=2)+'\n')
        assert call('stop')==0;success=True
        print('PASS actual imported account delivery refuses changed original cohort after nonempty handoff',flush=True)
        sys.exit(0)
    deadline=time.monotonic()+20
    while time.monotonic()<deadline:
        operations=rpc('wallet.orchard.listoperations',dict(binding,account=0))
        if any(o['operation_id']==request_id and o.get('chain_observation',{} ) and o['chain_observation']['outcome']=='confirmed' for o in operations['operations']):break
        time.sleep(.25)
    assert any(o['operation_id']==request_id and o.get('chain_observation') and o['chain_observation']['outcome']=='confirmed' for o in operations['operations']),operations
    assert rpc('wallet.orchard.getbalance',dict(binding,account=0))['confirmed_una']==520000
    if assumed:
        assert original_lifecycle()==original,'Orchard-only confirmation changed original assumed lifecycle'
        assert mixed_runtime_receipts()==mixed_before,'Orchard-only confirmation minted mixed baseline/schema'
    if new_compact_reservation:
        assert assumed
        original_candidates=snapshot_candidate_rows();before_new=wallet_rows();ordinary_before=candidate_wallet_state()
        current=rpc('wallet.orchard.getbalance',dict(binding,account=0))
        new_request=dict(binding,account=0,request_id='91'+'00'*31,expected_revision=current['account_revision'],fee_una=10000,
            payments=[dict(address=account['address'],amount_una=15000,memo_hex=b'new compact input'.hex())])
        pending_input=call('rpc',method='wallet.orchard.queueshield',params=new_request)
        assert pending_input.get('error_code')=='funding_source_pending' and pending_input['retryable'],pending_input
        assert wallet_rows()==before_new and candidate_wallet_state()==ordinary_before
        deadline=time.monotonic()+35
        while time.monotonic()<deadline:
            assert not failures,failures
            new_queued=call('rpc',method='wallet.orchard.queueshield',params=new_request)
            if new_queued.get('proof_queued'):break
            assert new_queued.get('error_code')=='funding_source_pending' and new_queued['retryable'],new_queued
            time.sleep(.1)
        assert new_queued.get('proof_queued') and new_queued['input_count']==1 and not new_queued['existing_request'],new_queued
        after_new=wallet_rows();new_retry=rpc('wallet.orchard.queueshield',new_request)
        assert new_retry['existing_request'] and not new_retry['proof_queued'] and wallet_rows()==after_new,new_retry
        reopened=None
        if not new_compact_ready:
            # Reservation-only gate preserves its original no-phrase reopen.
            # Ready gate below first signs in the SAME executor/session, then
            # tests signed-Ready reopen. Reserved live-job rebind is separate.
            assert rpc('wallet.open',['wire-named-owner'])['success'] is True
            assert rpc('wallet.unlock',['funded-crossing-password',0])['success'] is True
            binding=bound('wire-named-owner');new_request.update(binding)
            reopened=rpc('wallet.orchard.queueshield',new_request)
            assert reopened['existing_request'] and wallet_rows()==after_new,reopened
        assert original_lifecycle()==original and mixed_runtime_receipts()==mixed_before
        assert snapshot_candidate_rows()==original_candidates
        assert any(p['command']=='getwinput' for p in packets) and any(p['command']=='winput' for p in packets)
        new_ready=None
        if new_compact_ready:
            cold_reserved=None
            if ready_restart:
                premature=call('rpc',method='wallet.orchard.finishshield',params=dict(binding,account=0,
                    request_id=new_request['request_id'],reacquire_input_proofs=True))
                assert 'already signed current shield request' in premature.get('error',''),premature
                assert wallet_rows()==after_new,'Ready-only recovery replaced Reserved work'
            if reserved_restart:
                prior_binding=dict(binding);wire_before=len(packets)
                cold_reserved=restart_sdk_process('reserved')
                assert wallet_rows()==after_new,'orderly restart changed encrypted Reserved/retained rows'
                assert original_lifecycle()==original and mixed_runtime_receipts()==mixed_before
                assert rpc('wallet.open',['wire-named-owner'])['success'] is True
                assert rpc('wallet.unlock',['funded-crossing-password',0])['success'] is True
                binding=bound('wire-named-owner');new_request.update(binding)
                assert wallet_rows()==after_new,'no-phrase Reserved reopen changed encrypted rows'
                assert rpc('getblockcount',[])==104
                recovered_retry=rpc('wallet.orchard.queueshield',new_request)
                assert recovered_retry['existing_request'] and not recovered_retry['proof_queued'],recovered_retry
                assert wallet_rows()==after_new,'queue retry replaced durable randomized intent'
                cold_reserved.update(old_binding=prior_binding,new_binding=binding,queue_retry=recovered_retry,wire_before=wire_before)
            finish_new=dict(binding,account=0,request_id=new_request['request_id']);deadline=time.monotonic()+35
            while time.monotonic()<deadline:
                new_finished=call('rpc',method='wallet.orchard.finishshield',params=finish_new)
                if new_finished.get('error_code') not in ('proof_not_ready','funding_source_pending'):break
                assert wallet_rows()==after_new,'pending signing changed encrypted reservation'
                time.sleep(.1)
            assert new_finished.get('durable_state')=='signed' and new_finished.get('admitted'),new_finished
            if reserved_restart:
                assert any(p['command']=='getwinput' and p['direction']=='in' for p in packets[wire_before:]),'fresh PID reused a volatile input path'
                assert any(p['command']=='winput' and p['direction']=='out' for p in packets[wire_before:]),'fresh PID did not reacquire CURRENT input DATA'
                cold_reserved['encrypted_reserved_rows']=after_new
            new_signed=rpc('wallet.getrawtransaction',[new_finished['txid']])['hex'];assert new_signed
            signed_rows=wallet_rows();assert signed_rows!=after_new and signed_rows['orchard_wallet_retained']
            pool=call('sdk_activation_state')['pool']
            assert any(p['txid']==new_finished['txid'] and p['cached_proof_bytes'] for p in pool),pool
            # No full utxotx refill from the peer: actual per-input DATA was
            # recaptured for signing/Ready and independently verified at ingress.
            assert not proof_ready.is_set()
            txid=new_finished['txid'];asked_echo.clear();echo.clear();deadline=time.monotonic()+10
            while not echo and time.monotonic()<deadline:assert not failures,failures;time.sleep(.1)
            own_proofs=[p for p in echo if len(p)>37 and p[0]==2 and p[1:33]==bytes.fromhex(txid)[::-1]]
            assert len(own_proofs)==1 and bytes.fromhex(new_signed) in own_proofs[0],echo
            for label,bad_proof in [('missing',b''),('invalid',own_proofs[0][:-1]+bytes([own_proofs[0][-1]^1]))]:
                assert call('sdk_activation_set_proof',txid=txid,payload=bad_proof.hex()) is True
                refused_new=call('rpc',method='wallet.orchard.finishshield',params=finish_new)
                assert 'error' in refused_new and wallet_rows()==signed_rows,(label,refused_new)
                assert txid in rpc('mempool.getrawmempool',[])
                if ready_restart:
                    refused_recovery=call('rpc',method='wallet.orchard.finishshield',params=dict(finish_new,reacquire_input_proofs=True))
                    assert 'error' in refused_recovery and wallet_rows()==signed_rows,(label,refused_recovery)
                    assert txid in rpc('mempool.getrawmempool',[]),'explicit recovery dropped an existing poisoned entry'
            assert call('sdk_activation_set_proof',txid=txid,payload=own_proofs[0].hex()) is True
            ready_retry=rpc('wallet.orchard.finishshield',finish_new)
            assert ready_retry['already_in_mempool'] and ready_retry['txid']==txid and wallet_rows()==signed_rows,ready_retry
            assert rpc('wallet.orchard.queueshield',new_request)['existing_request'] and wallet_rows()==signed_rows
            assert rpc('wallet.open',['wire-named-owner'])['success'] is True
            assert rpc('wallet.unlock',['funded-crossing-password',0])['success'] is True
            binding=bound('wire-named-owner');new_request.update(binding);finish_new.update(binding)
            reopened=rpc('wallet.orchard.queueshield',new_request)
            reopened_ready=rpc('wallet.orchard.finishshield',finish_new)
            assert reopened['existing_request'] and reopened_ready['already_in_mempool'] and reopened_ready['txid']==txid
            assert wallet_rows()==signed_rows,'signed-Ready no-phrase reopen changed encrypted rows'
            cold_ready=None
            if ready_restart:
                wire_before_ready=len(packets);cold_ready=restart_sdk_process('ready')
                assert wallet_rows()==signed_rows and original_lifecycle()==original
                assert mixed_runtime_receipts()==mixed_before
                assert rpc('wallet.open',['wire-named-owner'])['success'] is True
                assert rpc('wallet.unlock',['funded-crossing-password',0])['success'] is True
                binding=bound('wire-named-owner');new_request.update(binding);finish_new.update(binding)
                assert rpc('getblockcount',[])==104 and txid not in rpc('mempool.getrawmempool',[])
                assert wallet_rows()==signed_rows,'Ready restart changed the durable signed intent'
                default_refusal=call('rpc',method='wallet.orchard.finishshield',params=finish_new)
                assert 'error' in default_refusal and wallet_rows()==signed_rows,default_refusal
                assert txid not in rpc('mempool.getrawmempool',[]),'default retry silently reacquired an absent pool proof'
                conflicting=dict(new_request,reacquire_input_proofs=True,payments=[dict(new_request['payments'][0],memo_hex='ff')])
                conflict=call('rpc',method='wallet.orchard.finishshield',params=conflicting)
                assert 'error' in conflict and wallet_rows()==signed_rows and txid not in rpc('mempool.getrawmempool',[]),conflict
                recovered_request=dict(finish_new,reacquire_input_proofs=True)
                deadline=time.monotonic()+35
                while time.monotonic()<deadline:
                    recovered_ready=call('rpc',method='wallet.orchard.finishshield',params=recovered_request)
                    assert wallet_rows()==signed_rows,'Ready input recovery changed sealed signed bytes or owners'
                    if recovered_ready.get('error_code')!='funding_source_pending':break
                    assert recovered_ready['retryable'],recovered_ready
                    time.sleep(.1)
                assert recovered_ready.get('admitted') and recovered_ready.get('durable_state')=='signed' and recovered_ready.get('txid')==txid,recovered_ready
                assert rpc('wallet.getrawtransaction',[txid])['hex']==new_signed,'Ready recovery signed a replacement transaction'
                recovered_pool=call('sdk_activation_state')['pool']
                assert any(p['txid']==txid and p['cached_proof_bytes'] for p in recovered_pool),recovered_pool
                assert any(p['direction']=='in' and p['command']=='getwinput' for p in packets[wire_before_ready:])
                assert any(p['direction']=='out' and p['command']=='winput' for p in packets[wire_before_ready:])
                assert not proof_ready.is_set(),'Ready recovery relied on a whole peer utxotx refill'
                cold_ready.update(default_refusal=default_refusal,conflicting_intent_refusal=conflict,recovered=recovered_ready,pool=recovered_pool,
                    encrypted_ready_rows=signed_rows,current_inputs_reacquired=True)
            # Ordinary independent FULL admission/mining of the SAME canonical
            # SDK-signed body, not a manually built proof-backed spend.
            shield_mined=native_call('mine_shield',body=new_signed);assert txid in shield_mined['transactions'],shield_mined
            bodies[bytes.fromhex(shield_mined['hash'])[::-1]]=bytes.fromhex(shield_mined['body'])
            headers.append(bytes.fromhex(shield_mined['header']));deadline=time.monotonic()+45
            while time.monotonic()<deadline:
                assert not failures,failures
                if rpc('getblockcount',[])==105:break
                time.sleep(.1)
            assert rpc('getblockcount',[])==105
            deadline=time.monotonic()+20
            while time.monotonic()<deadline:
                new_operations=rpc('wallet.orchard.listoperations',dict(binding,account=0))
                if any(o['operation_id']==new_request['request_id'] and (o.get('chain_observation') or {}).get('outcome')=='confirmed' for o in new_operations['operations']):break
                time.sleep(.1)
            assert any(o['operation_id']==new_request['request_id'] and (o.get('chain_observation') or {}).get('outcome')=='confirmed' for o in new_operations['operations']),new_operations
            new_balance=rpc('wallet.orchard.getbalance',dict(binding,account=0));assert new_balance['confirmed_una']==535000,new_balance
            assert original_lifecycle()==original and mixed_runtime_receipts()==mixed_before
            new_ready=dict(finished=new_finished,body=new_signed,pool=pool,proof=own_proofs[0].hex(),retry=ready_retry,reopened_ready=reopened_ready,
                mined=shield_mined,operations=new_operations,balance=new_balance,reserved_cold_restart=cold_reserved,ready_cold_restart=cold_ready)
            if post_base_shield:
                # Both historical funding outputs are now genuinely spent.
                # Only actual owned change made AFTER base101 can fund this
                # next request; neither the script nor the native source may
                # inject a wallet coin, caller balance or proof-backed spend.
                change=next(o for o in shield_mined['transparent_outputs'] if o['amount_una']==475000)
                assert change['height']==105
                before_discovery_post=candidate_wallet_state();before_candidates_post=snapshot_candidate_rows()
                live_candidates=rpc('wallet.listunspent',[])
                opened_post=rpc('wallet.open',['wire-named-owner']);assert opened_post['success'] is True
                unlocked_post=rpc('wallet.unlock',['funded-crossing-password',0]);assert unlocked_post['success'] is True
                binding=bound('wire-named-owner')
                discovered_post=rpc('wallet.listunspent',[])
                discovery=dict(live=live_candidates,reopened=discovered_post,opened=opened_post,unlocked=unlocked_post,
                    expected_txid=txid,expected_change=change,ordinary=candidate_wallet_state())
                (root/'post-base-discovery.json').write_text(json.dumps(discovery,indent=2)+'\n')
                found=[c for c in discovered_post if c.get('txid')==txid and c.get('vout')==change['index']]
                assert len(found)==1,'normal named-wallet discovery did not find real post-base105 change'
                assert found[0]['amount_una']==475000 and found[0]['scriptPubKey']==change['script'],found
                assert not found[0].get('snapshot_candidate',False),'post-base change was relabelled as original frozen funding'
                assert found[0]['candidate_only'] and found[0]['funding_source_height']==105 and found[0]['funding_source_hash']==shield_mined['hash'],found
                assert found[0]['confirmations']==0 and not found[0]['safe'] and not found[0]['spendable'],found
                first_change_index=next(o['index'] for o in notification['outputs'] if o['script']==fixture['funding_change_script'] and o['amount']==470000)
                expected_post={(finished['txid'],first_change_index):(470000,fixture['funding_change_script'],104),
                               (txid,change['index']):(475000,change['script'],105)}
                assert {(c['txid'],c['vout']):(c['amount_una'],c['scriptPubKey'],c['candidate_created_height']) for c in discovered_post}==expected_post,'candidate result omitted real post-base change or retained spent historical inputs'
                assert all(c['candidate_only'] and c['confirmations']==0 and not c['safe'] and not c['spendable'] for c in discovered_post)
                assert candidate_wallet_state()==before_discovery_post and snapshot_candidate_rows()==before_candidates_post,'post-base discovery mutated ordinary/frozen DATA or catalog'
                assert original_lifecycle()==original and mixed_runtime_receipts()==mixed_before
                if post_base_discovery_only:
                    refusal=None
                    if post_base_source_refusal:
                        # Deliberately damage ONLY this fresh isolated source.
                        # No repair/retry/adoption of the refused root.
                        assert rpc('wallet.lock',[])['success'] is True
                        with sqlite3.connect(datadir/'blockchain/utxo') as db:
                            assert db.execute("UPDATE utxo_metadata SET value=? WHERE key='assumeutxo_expected_commitment'",('e'*64,)).rowcount==1
                        bad_original=original_lifecycle();assert bad_original!=original
                        assert rpc('wallet.unlock',['funded-crossing-password',0])['success'] is True
                        before_refusal=candidate_wallet_state();refusal_rows=wallet_rows()
                        refusal=call('rpc',method='wallet.listunspent',params=[])
                        assert 'error' in refusal and candidate_wallet_state()==before_refusal and wallet_rows()==refusal_rows,refusal
                        assert original_lifecycle()==bad_original and mixed_runtime_receipts()==mixed_before
                    (root/'receipt.json').write_text(json.dumps(dict(original_assumed_mobile=True,post_base_discovery=discovery,
                        source_refusal=refusal,original_lifecycle=original,mixed_receipts_unchanged=mixed_before,
                        scope='Actual imported compact105 normal named no-phrase reopen/unlock discovers ONLY genuine owned post-base change as unsafe/conf0/unspendable candidate DATA, hides both genuinely spent historical inputs, leaves ordinary/frozen/catalog state and original ten SQL/missing mixed baselines unchanged. Optional separately isolated bad-original-cohort case refuses without repair/fallback. NO post-base new reservation/signing/106 or mixed/index history/app/device/release claim.'),indent=2)+'\n')
                    assert call('stop')==0;success=True
                    print('PASS actual authenticated post-base candidate DATA discovery'+(' and original-cohort refusal' if post_base_source_refusal else ''),flush=True)
                    sys.exit(0)
                before_post=wallet_rows();wire_post=len(packets)
                post_balance=rpc('wallet.orchard.getbalance',dict(binding,account=0))
                post_request=dict(binding,account=0,request_id='b1'+'00'*31,expected_revision=post_balance['account_revision'],fee_una=10000,
                    payments=[dict(address=account['address'],amount_una=15000,memo_hex=b'post-base change'.hex())])
                post_pending=call('rpc',method='wallet.orchard.queueshield',params=post_request)
                assert post_pending.get('error_code')=='funding_source_pending' and post_pending['retryable'],post_pending
                assert wallet_rows()==before_post,'pending post-base input DATA changed encrypted owners'
                deadline=time.monotonic()+35
                while time.monotonic()<deadline:
                    post_queued=call('rpc',method='wallet.orchard.queueshield',params=post_request)
                    if post_queued.get('proof_queued'):break
                    assert post_queued.get('error_code')=='funding_source_pending' and post_queued['retryable'],post_queued
                    assert wallet_rows()==before_post
                    time.sleep(.1)
                assert post_queued.get('proof_queued') and post_queued['input_count']==1 and not post_queued['existing_request'],post_queued
                post_reserved=wallet_rows();assert post_reserved!=before_post
                assert rpc('wallet.orchard.queueshield',post_request)['existing_request'] and wallet_rows()==post_reserved
                finish_post=dict(binding,account=0,request_id=post_request['request_id']);deadline=time.monotonic()+35
                while time.monotonic()<deadline:
                    post_finished=call('rpc',method='wallet.orchard.finishshield',params=finish_post)
                    if post_finished.get('error_code') not in ('proof_not_ready','funding_source_pending'):break
                    assert wallet_rows()==post_reserved
                    time.sleep(.1)
                assert post_finished.get('durable_state')=='signed' and post_finished.get('admitted'),post_finished
                post_signed=rpc('wallet.getrawtransaction',[post_finished['txid']])['hex']
                post_signed_rows=wallet_rows()
                assert rpc('wallet.orchard.finishshield',finish_post)['already_in_mempool'] and wallet_rows()==post_signed_rows
                assert not proof_ready.is_set()
                post_mined=native_call('mine_shield',body=post_signed)
                assert post_finished['txid'] in post_mined['transactions']
                assert post_mined['transparent_inputs']==[dict(txid=txid,index=change['index'],amount_una=475000,
                    script=change['script'],height=105,coinbase=False)],post_mined['transparent_inputs']
                bodies[bytes.fromhex(post_mined['hash'])[::-1]]=bytes.fromhex(post_mined['body']);headers.append(bytes.fromhex(post_mined['header']))
                deadline=time.monotonic()+45
                while time.monotonic()<deadline:
                    assert not failures,failures
                    if rpc('getblockcount',[])==106:break
                    time.sleep(.1)
                assert rpc('getblockcount',[])==106
                deadline=time.monotonic()+20
                while time.monotonic()<deadline:
                    post_operations=rpc('wallet.orchard.listoperations',dict(binding,account=0))
                    if any(o['operation_id']==post_request['request_id'] and (o.get('chain_observation') or {}).get('outcome')=='confirmed' for o in post_operations['operations']):break
                    time.sleep(.1)
                assert any(o['operation_id']==post_request['request_id'] and (o.get('chain_observation') or {}).get('outcome')=='confirmed' for o in post_operations['operations']),post_operations
                post_result=rpc('wallet.orchard.getbalance',dict(binding,account=0));assert post_result['confirmed_una']==550000,post_result
                assert original_lifecycle()==original and mixed_runtime_receipts()==mixed_before
                baseline_result=None
                if mixed_baseline:
                    for forbidden in ({'tip':106},{'reset':True},{'base':101}):
                        bad_baseline=call('rpc',method='wallet.reconcilecoverage',params=dict(binding,**forbidden))
                        assert 'error' in bad_baseline and not bad_baseline.get('complete'),bad_baseline
                    historical_release.set()
                    before_baseline_rows=wallet_rows();before_baseline_candidates=snapshot_candidate_rows()
                    probe_start=len(packets)
                    first_baseline=call('rpc',method='wallet.reconcilecoverage',params=binding)
                    assert first_baseline.get('error_code')=='coverage_source_pending' and first_baseline.get('retryable') is True,first_baseline
                    # One actual RPC, then let its bounded DATA actor finish;
                    # NO second source call can persist/queue a successor here.
                    # Attribute only its first unarchived prefix window1..8.
                    # The ordinary scheduler also backfills other heights; keep
                    # those packets as DATA evidence, not this actor's requests.
                    time.sleep(1)
                    probe_bodies={double_sha((root/p['file']).read_bytes()[:128])[::-1] for p in list(packets)[probe_start:]
                        if p['direction']=='out' and p['command']=='block'} & historical_only
                    prefix_window={double_sha(Path(fixture['funding_bodies'][str(h)]).read_bytes()[:128])[::-1] for h in range(1,9)}
                    prefix_bodies=probe_bodies & prefix_window
                    probe=dict(reply=first_baseline,packet_start=probe_start,legacy_bodies=[h.hex() for h in sorted(prefix_bodies)],
                        other_legacy_bodies=[h.hex() for h in sorted(probe_bodies-prefix_window)],scope='First unarchived prefix window1..8 only; unrelated scheduler backfill packets retained separately.')
                    (root/'history-probe.json').write_text(json.dumps(probe,indent=2)+'\n')
                    assert len(prefix_bodies)==1,probe
                    assert wallet_rows()==before_baseline_rows and original_lifecycle()==original and mixed_runtime_receipts()==mixed_before
                    baseline_replies=[];deadline=time.monotonic()+120
                    while time.monotonic()<deadline:
                        baseline_reply=call('rpc',method='wallet.reconcilecoverage',params=binding)
                        baseline_replies.append(baseline_reply)
                        (root/'baseline-replies.json').write_text(json.dumps(baseline_replies,indent=2)+'\n')
                        if baseline_reply.get('complete'):break
                        assert baseline_reply.get('error_code')=='coverage_source_pending' and baseline_reply.get('retryable') is True,baseline_reply
                        assert wallet_rows()==before_baseline_rows and original_lifecycle()==original
                        time.sleep(.1)
                    assert baseline_reply.get('complete') and baseline_reply['checkpoint_height']==106,baseline_reply
                    assert baseline_reply['checkpoint_hash']==post_mined['hash'] and baseline_reply['applied_sequence']==5,baseline_reply
                    assert baseline_reply['historical_replay']=='genesis_to_activation_parent' and not baseline_reply['whole_wallet_ready'],baseline_reply
                    receipts=mixed_runtime_receipts()
                    assert len(receipts['indexed'])==1 and receipts['ordinary'] and receipts['ordinary'][0][0] and receipts['ordinary'][0][1]==0,receipts
                    assert sorted(receipts['receipt_columns'])==['runtime_ordinary_invalid','runtime_ordinary_receipt']
                    wallet_paths=list(datadir.rglob('wallet_wire-named-owner.db'));assert len(wallet_paths)==1
                    with sqlite3.connect('file:'+str(wallet_paths[0])+'?mode=ro',uri=True) as db:
                        ordinary_live=db.execute('SELECT txid,vout,amount,height FROM utxos WHERE is_spent=0 ORDER BY txid,vout').fetchall()
                    with sqlite3.connect('file:'+str(datadir/'blockchain/utxo')+'?mode=ro',uri=True) as db:
                        index_live=db.execute('SELECT txid,vout,value,height FROM wallet_utxos WHERE spend_height IS NULL ORDER BY txid,vout').fetchall()
                    assert ordinary_live==index_live and sum(r[2] for r in ordinary_live)==920000,(ordinary_live,index_live)
                    assert sorted((r[2],r[3]) for r in ordinary_live)==[(450000,106),(470000,104)]
                    assert wallet_rows()==before_baseline_rows and snapshot_candidate_rows()==before_baseline_candidates
                    assert original_lifecycle()==original,'coverage moved original assumed lifecycle'
                    repeated_baseline=rpc('wallet.reconcilecoverage',binding)
                    assert repeated_baseline==baseline_reply and mixed_runtime_receipts()==receipts,repeated_baseline
                    assert wallet_rows()==before_baseline_rows and original_lifecycle()==original
                    # Earning actual baseline effects is separate from the
                    # later owner-bound balance-read publication gate.
                    refused_balance=call('rpc',method='wallet.getbalance',params=[])
                    assert refused_balance.get('error_code')=='wallet_balance_not_current',refused_balance
                    baseline_result=dict(result=baseline_reply,repeated=repeated_baseline,history_probe=probe,replies=baseline_replies,receipts=receipts,
                        ordinary_live=ordinary_live,index_live=index_live,balance_refusal=refused_balance,
                        scope='Genuine independent genesis-to-parent replay of public legacy body DATA plus actual canonical102..106 events, original audited coverage writer/index-first and FULL ordinary commits. Original imported cohort/anchor/lifecycle and encrypted owners unchanged. Captured two-store baseline only; NO live history promotion, general coin view, whole-wallet balance-read readiness or app/device/release qualification.')
                mixed_balance=None
                if mixed_balance_refusal:
                    # Genuine Orchard confirmation is NOT both transparent
                    # baselines. Query normal public surfaces, not injected
                    # wallet rows or a caller-provided balance/readiness flag.
                    before_balance=candidate_wallet_state();balance_rows=wallet_rows()
                    balance_candidates=snapshot_candidate_rows()
                    replies={method:call('rpc',method=method,params=[]) for method in
                        ('wallet.getbalance','wallet.getwalletinfo','wallet.snapshot')}
                    mixed_balance=dict(replies=replies,ordinary_before=before_balance,
                        ordinary_after=candidate_wallet_state(),mixed_before=mixed_before,
                        mixed_after=mixed_runtime_receipts(),orchard_balance=post_result)
                    (root/'mixed-balance-observation.json').write_text(json.dumps(mixed_balance,indent=2)+'\n')
                    assert candidate_wallet_state()==before_balance and wallet_rows()==balance_rows
                    assert snapshot_candidate_rows()==balance_candidates and original_lifecycle()==original
                    assert mixed_runtime_receipts()==mixed_before,'balance query minted mixed baselines/schema'
                    for method,balance_reply in replies.items():
                        assert balance_reply.get('error_code')=='wallet_balance_not_current' and balance_reply.get('retryable') is True,(method,balance_reply)
                        assert not any(key in balance_reply for key in ('confirmed','balance','total','spendable','balances')),(method,balance_reply)
                    info=replies['wallet.getwalletinfo']
                    assert 'error' not in info and info['walletname']=='wire-named-owner' and info['encrypted'] and info['unlocked'],info
                    assert info['balance_status']=='reconciliation_required' and info['balance_error'],info
                assert any(p['command']=='getwinput' and struct.unpack_from('<I',(root/p['file']).read_bytes(),54)[0]==105 for p in packets[wire_post:])
                new_ready['post_base_shield']=dict(discovery=discovery,pending=post_pending,queued=post_queued,finished=post_finished,
                    body=post_signed,mined=post_mined,operations=post_operations,balance=post_result,
                    mixed_balance_refusal=mixed_balance,
                    mixed_baseline=baseline_result,
                    scope='Actual original-assumed compact NEW post-base105 owned-change discovery during normal named reopen/unlock, CURRENT105 TCP input proof acquisition and public NEW queue/finish with real canonical FULL validation/mining106/SDK106/550000una. Both original historical funding outputs already spent; no wallet coin or caller balance injection, manual proof-backed spend, mixed/independent baseline, mainnet/v5/all lifecycles/apps/devices/release claim.')
        (root/'receipt.json').write_text(json.dumps(dict(original_assumed_mobile=True,new_compact_reservation=True,
            pending=pending_input,queued=new_queued,retry=new_retry,reopened=reopened,original_lifecycle=original,
            mixed_receipts_unchanged=mixed_before,prior_confirmation=operations,new_compact_ready=new_ready,post_base_shield=post_base_shield,
            scope='Actual native imported funded crossing to104, then a DIFFERENT unspent historical output obtains a new exact-tip proof over TCP and atomically queues a NEW bound shield reservation through public SDK C ABI. '+('Reserved-restart variant orderly stops and EXITS the original SDK PID, starts a fresh executor without snapshot re-import, opens/unlocks the SAME encrypted named wallet without phrase and authenticates its unchanged durable randomized intent before fresh CURRENT paths/signing. ' if reserved_restart else 'Ready variant signs in the SAME executor/session. ')+('Ready-restart variant additionally EXITS the signed-Ready process; a fresh PID/no-phrase wallet reopen retains identical encrypted signed intent, refuses default missing-pool retry, then explicitly reacquires CURRENT per-input DATA for independent admission of the SAME signed bytes. Existing poisoned entries refuse even explicit recovery. ' if ready_restart else '')+ 'Ready uses selected exact-current coins, freshly recaptures body/proofs for encrypted Ready, admits through ordinary proof-bearing ingress with peer full-proof refill disabled, preserves strict bad retained-proof refusals/retries and no-phrase SIGNED-Ready reopen, and truly mines/validates105 with confirmed535000una. Reservation-only variant retains its separate unsigned-reservation reopen gate. NO live-executor Reserved-job wallet-session rebind, forced/crash/powerloss, changed-tip/reorg recovery, mixed/index/independent history, apps/devices or release qualification.'),indent=2)+'\n')
        assert call('stop')==0;success=True
        print('PASS actual original-assumed current compact proof download and NEW '+('signing/Ready/pool/confirmation' if new_compact_ready else 'atomic reservation'),flush=True)
        sys.exit(0)
    assert rpc('wallet.restore',dict(name='late-funded-owner',mnemonic='abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about',rescan=False))['success'] is True
    later=bound('late-funded-owner')
    for _ in range(8):
        recovered=rpc('wallet.orchard.recoveraccount0',later)
        if recovered.get('complete'):break
    assert recovered.get('complete') and recovered['generated'] is False,recovered
    balance=rpc('wallet.orchard.getbalance',dict(later,account=0));notes=rpc('wallet.orchard.listreceived',dict(later,account=0))
    assert balance['confirmed_una']==520000 and notes['total_count']==2,(balance,notes)
    assert any(n['amount_una']==20000 and n['height']==104 and bytes.fromhex(n['memo_hex'])==b'funded queued crossing'.ljust(512,b'\0') for n in notes['received']),notes
    private_send=None;unshield=None;private_mixed=None
    if os.environ.get('DINERO_SDK_COMPACT_PRIVATE_SEND')=='1':
        # Distinct seed, normal named restore and explicit authenticated account0
        # recovery. The native producer never knows either wallet's spend plan.
        assert rpc('wallet.restore',dict(name='private-recipient',mnemonic='legal winner thank year wave sausage worth useful legal winner thank yellow',rescan=False))['success'] is True
        recipient_binding=bound('private-recipient')
        for _ in range(8):
            recipient_recovery=rpc('wallet.orchard.recoveraccount0',recipient_binding)
            if recipient_recovery.get('complete'):break
        assert recipient_recovery.get('complete') and recipient_recovery['generated'] is False,recipient_recovery
        receiver=rpc('wallet.orchard.getnewaddress',dict(recipient_binding,account=0))['address']
        # Spend the earlier funds from the later post-activation account0
        # recovery itself, not merely the pre-enrolled crossing wallet.
        assert rpc('wallet.open',['late-funded-owner'])['success'] is True
        assert rpc('wallet.encrypt',['late-private-send-password'])['success'] is True
        assert rpc('wallet.unlock',['late-private-send-password',0])['success'] is True
        payer=bound('late-funded-owner');payer_balance=rpc('wallet.orchard.getbalance',dict(payer,account=0))
        assert payer_balance['confirmed_una']==520000,payer_balance
        if assumed:private_mixed={name:mixed_runtime_receipts(name) for name in ('late-funded-owner','private-recipient')}
        private_id='81'+'00'*31
        payment=dict(payer,account=0,request_id=private_id,expected_revision=payer_balance['account_revision'],
            payments=[dict(address=receiver,amount_una=10000,memo_hex=b'compact private payment'.hex())],outputs=[],fee_una=100000)
        private_queued=rpc('wallet.orchard.queuespend',payment)
        assert private_queued['proof_queued'] and not private_queued['existing_request'],private_queued
        reserved_rows=wallet_rows('late-funded-owner');private_retry=rpc('wallet.orchard.queuespend',payment)
        assert private_retry['existing_request'] and not private_retry['proof_queued'] and wallet_rows('late-funded-owner')==reserved_rows,private_retry
        deadline=time.monotonic()+30
        while time.monotonic()<deadline:
            private_finished=call('rpc',method='wallet.orchard.finishspend',params=dict(payer,account=0,request_id=private_id))
            (root/'private-send-progress.json').write_text(json.dumps(dict(queued=private_queued,finished=private_finished),indent=2)+'\n')
            if isinstance(private_finished,dict) and private_finished.get('admitted'):break
            assert isinstance(private_finished,dict) and private_finished.get('error_code')=='proof_not_ready' and private_finished.get('proof_state') in ('queued','running'),private_finished
            time.sleep(.25)
        assert private_finished.get('admitted'),private_finished
        private_txid=private_finished['txid'];private_body=rpc('wallet.getrawtransaction',[private_txid])['hex']
        ready_rows=wallet_rows('late-funded-owner');private_refinished=rpc('wallet.orchard.finishspend',dict(payer,account=0,request_id=private_id))
        assert private_refinished['already_in_mempool'] and private_refinished['txid']==private_txid,private_refinished
        assert wallet_rows('late-funded-owner')==ready_rows and rpc('wallet.getrawtransaction',[private_txid])['hex']==private_body
        private_mined=native_call('mine_private',body=private_body);assert private_txid in private_mined['transactions'],private_mined
        bodies[bytes.fromhex(private_mined['hash'])[::-1]]=bytes.fromhex(private_mined['body'])
        headers.append(bytes.fromhex(private_mined['header']));deadline=time.monotonic()+60
        while time.monotonic()<deadline:
            assert not failures,failures
            if rpc('getblockcount',[])==105:break
            time.sleep(.25)
        assert rpc('getblockcount',[])==105
        # Certified post-activation accounts have their own existing public
        # continuation RPC. Advancing Orchard does NOT fabricate transparent
        # wallet/index baseline receipts or claim mixed-wallet readiness.
        for _ in range(8):
            payer_continuation=rpc('wallet.orchard.recoveraccount0',payer)
            if payer_continuation.get('complete'):break
        assert payer_continuation.get('complete') and payer_continuation['already_recovered'] and payer_continuation['generated'] is False,payer_continuation
        deadline=time.monotonic()+20
        while time.monotonic()<deadline:
            payer_balance=rpc('wallet.orchard.getbalance',dict(payer,account=0))
            if payer_balance['confirmed_una']==410000:break
            time.sleep(.25)
        assert payer_balance['confirmed_una']==410000,payer_balance
        assert rpc('wallet.open',['private-recipient'])['success'] is True
        recipient_binding=bound('private-recipient')
        for _ in range(8):
            recipient_continuation=rpc('wallet.orchard.recoveraccount0',recipient_binding)
            if recipient_continuation.get('complete'):break
        assert recipient_continuation.get('complete') and recipient_continuation['already_recovered'] and recipient_continuation['generated'] is False,recipient_continuation
        deadline=time.monotonic()+20
        while time.monotonic()<deadline:
            recipient_balance=rpc('wallet.orchard.getbalance',dict(recipient_binding,account=0))
            if recipient_balance['confirmed_una']==10000:break
            time.sleep(.25)
        received=rpc('wallet.orchard.listreceived',dict(recipient_binding,account=0))
        assert recipient_balance['confirmed_una']==10000 and received['total_count']==1,(recipient_balance,received)
        assert any(n['amount_una']==10000 and n['height']==105 and bytes.fromhex(n['memo_hex'])==b'compact private payment'.ljust(512,b'\0') for n in received['received']),received
        private_send=dict(queued=private_queued,finished=private_finished,refinished=private_refinished,mined=private_mined,
            payer_balance=payer_balance,recipient_balance=recipient_balance,received=received,recipient_recovery=recipient_recovery,
            payer_continuation=payer_continuation,recipient_continuation=recipient_continuation)
        print('PASS actual compact SDK queuespend/finishspend, native105 confirmation and distinct recovered recipient memo',flush=True)
        if assumed_unshield:
            # Normal owned receiver; no injected coins or manual proof spend.
            destination=rpc('wallet.getnewaddress',['taproot'])['address']
            decoded=rpc('wallet.validateaddress',[destination]);assert decoded['isvalid'] and decoded['ismine'],decoded
            destination_script=decoded['scriptPubKey'];assert call('watch_script',script=destination_script)==0
            assert rpc('wallet.open',['late-funded-owner'])['success'] is True
            assert rpc('wallet.unlock',['late-private-send-password',0])['success'] is True
            payer=bound('late-funded-owner');available=rpc('wallet.orchard.getbalance',dict(payer,account=0))
            assert available['confirmed_una']==410000,available
            unshield_id='a1'+'00'*31
            withdrawal=dict(payer,account=0,request_id=unshield_id,expected_revision=available['account_revision'],
                payments=[],outputs=[dict(address=destination,amount_una=10000)],fee_una=100000)
            unshield_queued=rpc('wallet.orchard.queuespend',withdrawal)
            assert unshield_queued['proof_queued'] and not unshield_queued['existing_request'],unshield_queued
            pending_rows=wallet_rows('late-funded-owner');unshield_retry=rpc('wallet.orchard.queuespend',withdrawal)
            assert unshield_retry['existing_request'] and not unshield_retry['proof_queued'] and wallet_rows('late-funded-owner')==pending_rows,unshield_retry
            deadline=time.monotonic()+35
            while time.monotonic()<deadline:
                unshield_finished=call('rpc',method='wallet.orchard.finishspend',params=dict(payer,account=0,request_id=unshield_id))
                (root/'unshield-progress.json').write_text(json.dumps(dict(queued=unshield_queued,finished=unshield_finished),indent=2)+'\n')
                if unshield_finished.get('admitted'):break
                assert unshield_finished.get('error_code')=='proof_not_ready' and unshield_finished.get('proof_state') in ('queued','running'),unshield_finished
                time.sleep(.25)
            assert unshield_finished.get('admitted'),unshield_finished
            withdrawal_txid=unshield_finished['txid'];withdrawal_body=rpc('wallet.getrawtransaction',[withdrawal_txid])['hex']
            ready_withdrawal=wallet_rows('late-funded-owner')
            unshield_refinished=rpc('wallet.orchard.finishspend',dict(payer,account=0,request_id=unshield_id))
            assert unshield_refinished['already_in_mempool'] and unshield_refinished['txid']==withdrawal_txid and wallet_rows('late-funded-owner')==ready_withdrawal,unshield_refinished
            observation=call('sdk_activation_state')['observed_transaction']
            assert observation['txid']==withdrawal_txid and observation['format']=='orchard',observation
            assert any(o['amount']==10000 and o['script']==destination_script for o in observation['outputs']),observation
            withdrawal_mined=native_call('mine_private',body=withdrawal_body)
            assert withdrawal_txid in withdrawal_mined['transactions'],withdrawal_mined
            assert withdrawal_mined['transparent_outputs']==[dict(index=0,amount_una=10000,script=destination_script,height=106)],withdrawal_mined
            bodies[bytes.fromhex(withdrawal_mined['hash'])[::-1]]=bytes.fromhex(withdrawal_mined['body'])
            headers.append(bytes.fromhex(withdrawal_mined['header']));deadline=time.monotonic()+60
            while time.monotonic()<deadline:
                assert not failures,failures
                if rpc('getblockcount',[])==106:break
                time.sleep(.25)
            assert rpc('getblockcount',[])==106
            for _ in range(8):
                withdrawal_continuation=rpc('wallet.orchard.recoveraccount0',payer)
                if withdrawal_continuation.get('complete'):break
            assert withdrawal_continuation.get('complete') and withdrawal_continuation['already_recovered'] and withdrawal_continuation['generated'] is False,withdrawal_continuation
            after_withdrawal=rpc('wallet.orchard.getbalance',dict(payer,account=0))
            assert after_withdrawal['confirmed_una']==300000,after_withdrawal
            withdrawal_ops=rpc('wallet.orchard.listoperations',dict(payer,account=0))
            assert any(o['operation_id']==unshield_id and (o.get('chain_observation') or {}).get('outcome')=='confirmed' for o in withdrawal_ops['operations']),withdrawal_ops
            assert rpc('wallet.open',['private-recipient'])['success'] is True
            recipient_binding=bound('private-recipient')
            for _ in range(8):
                recipient_after=rpc('wallet.orchard.recoveraccount0',recipient_binding)
                if recipient_after.get('complete'):break
            assert recipient_after.get('complete') and recipient_after['already_recovered'] and not recipient_after['generated'],recipient_after
            assert rpc('wallet.orchard.getbalance',dict(recipient_binding,account=0))['confirmed_una']==10000
            assert original_lifecycle()==original,'private spend/unshield changed original assumed lifecycle'
            assert mixed_runtime_receipts()==mixed_before
            assert {name:mixed_runtime_receipts(name) for name in private_mixed}==private_mixed,'Orchard-only spend/unshield minted mixed baseline/schema'
            unshield=dict(request=withdrawal,queued=unshield_queued,retry=unshield_retry,finished=unshield_finished,
                refinished=unshield_refinished,observer=observation,mined=withdrawal_mined,continuation=withdrawal_continuation,
                payer_balance=after_withdrawal,operations=withdrawal_ops,recipient_continuation=recipient_after,
                scope='Actual original-assumed public queuespend/finishspend unshield, real proof/nullifiers, canonical FULL mining106 and SDK validation106, confirmed encrypted payer300000una and real native transparent output10000una. NOT a transparent wallet/index baseline or ordinary balance-readiness receipt, physical apps/devices or release qualification.')
            print('PASS actual original-assumed compact unshield106, native transparent coin and payer confirmation',flush=True)
    (root/'receipt.json').write_text(json.dumps(dict(fixture=fixture,request=queued,transaction_id=txid,
        original_assumed_mobile=assumed,original_lifecycle=original if assumed else None,
        orchard_only_automatic_confirmation=assumed,mixed_receipts_unchanged=mixed_before,
        named_wallet_without_phrase=True,pause_no_reply=state['pause_probes'],pending_rows_identical=True,
        pool_revalidated_and_proof_served=True,operations=operations,mined=mined,recovered=recovered,notes=notes,
        compact_exact_queue_retry=True,compact_ready_retry_with_current_proof=True,unproved_retries_refused=True,
        compact_private_send=private_send,
        compact_unshield=unshield,private_mixed_receipts=private_mixed,
        scope=('Actual native C ABI automatic handoff from normal legacy snapshot import with original assumed SQL.' if assumed else 'Actual native C ABI automatic handoff from independently executed full parent.')+' Funded named recovered wallet and nonempty signed encrypted request/pool, real confirmation and later account0 recovery. NOT physical app/device, minimum OS, SDK slices or release qualification.'),indent=2)+'\n')
    assert call('stop')==0;success=True
    print('PASS funded named-wallet nonempty queued SDK handoff and real confirmation/recovery',flush=True)
finally:
    stop.set();listener.close()
    for conn in connections:
        try:conn.shutdown(socket.SHUT_RDWR)
        except OSError:pass
    thread.join(timeout=5);process.stdin.close();native.stdin.close()
    for child in (process,native):
        try:child.wait(timeout=30)
        except subprocess.TimeoutExpired:child.kill();child.wait(timeout=5)
    output.close();reply.close();native_log.close();native_read.close()
    (root/'peer-failures.json').write_text(json.dumps(failures,indent=2)+'\n')
    if success:assert process.returncode==0 and native.returncode==0 and not thread.is_alive() and not failures
