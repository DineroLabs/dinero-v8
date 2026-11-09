#!/bin/bash
# Prepare an authentic mainnet compact v6 candidate in a PRIVATE publication
# root. This never promotes the public phone endpoint. Both phone runtimes
# must qualify these exact bytes before public promotion.
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
export DATADIR=${DATADIR:-/var/lib/dinero}
export PUBROOT=${PHONE_V6_QUALIFICATION_ROOT:-/var/lib/dinero/snapshot-qualification/compact-v6}
export SNAPSHOT_PROFILE=compact
export SNAPSHOT_COMPACT_PENDING_NAMESPACE=phone-v6-qualification
export GATE_PORT=${GATE_PORT:-27999}
export GATE_RPC=${GATE_RPC:-27998}
export GATE_WALLET=${GATE_WALLET:-28000}
CLI=${CLI_BIN:-/usr/bin/dinero-cli}
PUBLISHER=${PHONE_V6_PUBLISHER:-$SCRIPT_DIR/dinero-snapshot-publish.sh}

# A public root must never be supplied accidentally. Production promotion is
# a separate operation after native iOS/Android receipts exist.
case "$PUBROOT" in /var/www|/var/www/*) echo 'refusing public web root for phone qualification' >&2; exit 2;; esac
if [ -f "$PUBROOT/phone-qualification-required.json" ]; then
    echo 'Candidate prepared; native iOS and Android qualification still required.'
    exit 0
fi
TIP=$("$CLI" "-datadir=$DATADIR" getblockcount)
[[ "$TIP" =~ ^[0-9]+$ ]] || { echo 'invalid selected tip' >&2; exit 1; }
if [ "$TIP" -lt 150000 ]; then
    echo "WAIT_ACTIVATION: selected tip $TIP; compact v6 needs a base at or above 150000"
    exit 0
fi
bash "$PUBLISHER"
if [ ! -f "$PUBROOT/latest/manifest.json" ]; then
    echo 'WAIT_BURIAL_OR_PUBLISHER_LOCK: private candidate not ready'
    exit 0
fi
python3 - "$PUBROOT" <<'PY'
import datetime,hashlib,json,os,pathlib,sys
root=pathlib.Path(sys.argv[1]);generation=(root/'latest').resolve(strict=True)
manifest=generation/'manifest.json';data=json.loads(manifest.read_bytes());snapshot=data['snapshot']
if (data.get('network')!='mainnet' or data.get('format_version')!=6 or
    data.get('snapshot_profile')!='compact' or type(snapshot.get('height')) is not int or
    snapshot['height']<150000 or snapshot.get('snapshot_file')!='snapshot.dat'):
    raise SystemExit('private candidate is not a mainnet compact v6 phone publication')
payload=generation/'snapshot.dat'
with payload.open('rb') as stream:digest=hashlib.file_digest(stream,'sha256').hexdigest()
if digest!=snapshot['sha256'] or payload.stat().st_size!=snapshot['bytes'] or not (generation/'manifest.sig').is_file():
    raise SystemExit('private candidate payload/manifest/signature set is incomplete')
receipt={'state':'SERVER_NATIVE_IMPORT_PASSED_PHONE_QUALIFICATION_REQUIRED',
         'generated_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),
         'generation':str(generation),'snapshot':snapshot,
         'manifest_sha256':hashlib.sha256(manifest.read_bytes()).hexdigest(),
         'ios_qualified':False,'android_qualified':False,'public_promotion_performed':False}
temporary=root/'phone-qualification-required.json.tmp'
temporary.write_text(json.dumps(receipt,indent=2)+'\n');os.replace(temporary,root/'phone-qualification-required.json')
print(json.dumps(receipt,indent=2))
PY
