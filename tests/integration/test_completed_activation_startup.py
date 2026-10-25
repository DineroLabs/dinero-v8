"""Actual fresh-process DaemonApp completed-parent/descendant/undo startup.

Fixed test-only genuine-PoW profile, no installed data or daemon schedule option.
Retains every root, failed child output and prior original before-image.
"""
import hashlib,json,os,subprocess,tempfile
from pathlib import Path

def digest(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def inventory(root):
    return {str(p.relative_to(root)):digest(p) for p in sorted(root.rglob('*')) if p.is_file()}

root=Path(tempfile.mkdtemp(prefix='completed-activation-process-evidence-')).resolve()
print('Completed activation evidence:',root,flush=True)
generator=Path(os.environ['ACTIVATION_FIXTURE_GENERATOR']).resolve()
driver=Path(os.environ['NODECORE_DRIVER']).resolve()
manifest=root/'roots.json'
limit=int(os.environ.get('ACTIVATION_FIXTURE_LIMIT','8'));assert 1<=limit<=8
env=dict(os.environ,DINERO_COMPLETED_ACTIVATION_MANIFEST=str(manifest),DINERO_COMPLETED_ACTIVATION_LIMIT=str(limit))
with (root/'generator.log').open('xb') as log:
    p=subprocess.run([str(generator),'--export-completed-activation-fixtures'],env=env,
        stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT,timeout=150)
assert p.returncode==0,(root/'generator.log')
roots=json.loads(manifest.read_text());assert len(roots)==limit
results=[]
for row in roots:
    original=Path(row['original']);before=inventory(original)
    sentinel=Path(row['datadir'])/'named-wallet.sentinel';lifecycle=Path(row['datadir'])/'blockchain/original-lifecycle.sentinel'
    sentinels={str(p):digest(p) for p in [sentinel,lifecycle]}
    for attempt in range(1 if row['refuse'] else 2):
        prefix=f"phase{row['phase']}-cold{attempt+1}"
        reply=root/(prefix+'.jsonl');command=dict(op='completed_activation_cold',datadir=row['datadir'],profile=row['profile'])
        with (root/(prefix+'.log')).open('xb') as log:
            p=subprocess.run([str(driver),str(reply)],input=json.dumps(command)+'\n',text=True,
                stdout=log,stderr=subprocess.STDOUT,timeout=90)
        assert p.returncode==0,(prefix,p.returncode)
        lines=reply.read_text().splitlines();assert len(lines)==1
        result=json.loads(lines[0])
        if row['refuse']:
            assert result=={'driver_error':'completed activation App Init refused'},(prefix,result)
        else:
            assert 'driver_error' not in result,(prefix,result)
            assert result['tip']==result['active_tip']==row['tip'] and result['height']==row['height'],result
            assert result['validated_height']==0 and result['profile']==row['profile'],result
            assert result['named_wallet']=='handoff-named-owner' and result['wallet_open'] and result['stopped'],result
        assert inventory(original)==before,'original before-image changed'
        assert sentinels=={p:digest(Path(p)) for p in sentinels},'wallet/lifecycle sentinel changed'
        results.append(dict(phase=row['phase'],attempt=attempt+1,result=result,reply_sha256=digest(reply)))
(root/'receipt.json').write_text(json.dumps(dict(results=results,generator_sha256=digest(generator),driver_sha256=digest(driver),
    scope='Actual linked production DaemonApp cold start/stop in six fresh processes; fixture-host conversion, empty pending corpus, not a queued-payment crossing, mobile assumed-history lifecycle or device acceptance.'),indent=2)+'\n')
print('PASS: cold process gates at',limit,'states; original and sentinel evidence unchanged',flush=True)
