"""Real publisher shell + filesystem + Ed25519; RPC/native gate is an explicit fixture.
Native v6 proof/import is independently exercised by OrchardSnapshotRuntime.
"""
import hashlib,json,os,struct,subprocess,tempfile,unittest,shutil
from pathlib import Path
SCRIPT=Path(__file__).with_name('dinero-snapshot-publish.sh').resolve()

class PublisherV6(unittest.TestCase):
    def run_publication(self, profile=2, gate=True, reorg=False, key=True, fresh=False, optimize=False):
        tmp=tempfile.TemporaryDirectory(); self.addCleanup(tmp.cleanup)
        root=Path(tmp.name); dumps=root/'data/snapshots'; dumps.mkdir(parents=True)
        section=b'ORCH'+struct.pack('<I',1)+bytes([profile])+bytes(1048576)
        payload=b'UXTO'+struct.pack('<I',6)+bytes.fromhex('ab'*32)+struct.pack('<I',150000)+bytes(24)+struct.pack('<Q',len(section))+section+bytes(32)
        (root/'template.dat').write_bytes(payload)
        (dumps/'pending.dat').write_bytes(payload)
        (dumps/'pending.json').write_text(json.dumps({'base_height':150000,'base_hash':'ab'*32}))
        previous=root/'www/archive/previous';previous.mkdir(parents=True)
        (previous/'retained').write_text('previous publication')
        (root/'www/latest').symlink_to('archive/previous')
        if fresh:
            (dumps/'pending.dat').unlink();(dumps/'pending.json').unlink()
        bin=root/'bin';bin.mkdir()
        def executable(name, text):
            p=bin/name;p.write_text(text);p.chmod(0o755);return p
        for name in ('flock','chown'):executable(name,'#!/bin/sh\nexit 0\n')
        executable('stat','#!/usr/bin/env python3\nimport os,sys\nprint(os.stat(sys.argv[-1]).st_size)\n')
        executable('mv','#!/usr/bin/env python3\nimport os,sys\nos.replace(sys.argv[-2],sys.argv[-1])\n')
        executable('sleep','#!/bin/sh\n/bin/sleep 0.01\n')
        executable('head','#!/usr/bin/env python3\nimport sys\ndata=sys.stdin.readlines()\nn=int(sys.argv[2]);sys.stdout.writelines(data[:n] if n<0 else data[:n])\n')
        state={'assumeutxo_active':gate,'snapshot_base_height':150000,'snapshot_base_block':'ab'*32,'status':'NotStarted','error_message':''}
        cli=executable('cli', '#!/usr/bin/env python3\nimport json,sys,pathlib,shutil\na=sys.argv\nr=pathlib.Path('+repr(str(root))+')\nif "dumptxoutset" in a:\n i=a.index("dumptxoutset");shutil.copyfile(r/"template.dat",a[i+1]);(r/"export-args.json").write_text(json.dumps(a[i+1:]));print(json.dumps({"base_height":150000,"base_hash":"ab"*32}))\nelif "getbackgroundvalidationprogress" in a:\n (r/"gate-ran").touch(); print('+repr(json.dumps(state))+')\nelif "getblockcount" in a: print(150288)\nelif "getblockhash" in a: print("cd"*32 if '+repr(reorg)+' and (r/"gate-ran").exists() else "ab"*32)\nelse: sys.exit(99)\n')
        daemon=executable('daemon','#!/bin/sh\ntrap "exit 0" TERM\nwhile :; do /bin/sleep 0.1; done\n')
        ssl=shutil.which('openssl'); keypath=root/'key.pem'
        subprocess.run([ssl,'genpkey','-algorithm','ED25519','-out',str(keypath)],check=True,capture_output=True)
        pub=subprocess.check_output([ssl,'pkey','-in',str(keypath),'-pubout','-outform','DER'])[-32:]
        env=dict(os.environ,DATADIR=str(root/'data'),PUBROOT=str(root/'www'),KEY=str(keypath if key else root/'missing'),CLI_BIN=str(cli),DINEROD=str(daemon),GATE_TIMEOUT='2',PATH=str(bin)+os.pathsep+os.environ['PATH'])
        if optimize: env['PYTHONOPTIMIZE']='1'
        result=subprocess.run(['bash',str(SCRIPT)],env=env,capture_output=True,text=True,timeout=15)
        return root,payload,pub,result
    def test_signed_compact_publication_and_manifest(self):
        root,payload,pub,r=self.run_publication();self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        latest=root/'www/latest';manifest=json.loads((latest/'manifest.json').read_bytes())
        self.assertEqual(manifest['format_version'],6);self.assertEqual(manifest['snapshot_profile'],'compact')
        self.assertEqual(manifest['snapshot']['sha256'],hashlib.sha256(payload).hexdigest())
        self.assertEqual(manifest['snapshot']['bytes'],len(payload));self.assertEqual((latest/'snapshot.dat').read_bytes(),payload)
        export=os.environ.get('SNAPSHOT_PUBLISHER_EVIDENCE')
        if export:
            dest=Path(export);dest.mkdir(parents=True,exist_ok=True)
            for name in ('snapshot.dat','manifest.json','manifest.sig'):shutil.copyfile(latest/name,dest/name)
            (dest/'public-key.raw').write_bytes(pub)
    def test_fresh_dump_explicitly_requests_compact(self):
        root,_,_,r=self.run_publication(fresh=True);self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        args=json.loads((root/'export-args.json').read_text());self.assertEqual(args[-1],'compact')
    def test_full_export_cannot_publish_to_phone(self):
        root,_,_,r=self.run_publication(profile=1);self.assertNotEqual(r.returncode,0);self.assertEqual((root/'www/latest/retained').read_text(),'previous publication')
    def test_native_gate_refusal_preserves_latest(self):
        root,_,_,r=self.run_publication(gate=False);self.assertNotEqual(r.returncode,0);self.assertEqual((root/'www/latest/retained').read_text(),'previous publication')
    def test_reorg_during_gate_refuses(self):
        root,_,_,r=self.run_publication(reorg=True);self.assertNotEqual(r.returncode,0);self.assertIn('during gate',r.stdout);self.assertEqual((root/'www/latest/retained').read_text(),'previous publication')
    def test_python_optimization_cannot_disable_native_gate(self):
        root,_,_,r=self.run_publication(gate=False,optimize=True)
        self.assertNotEqual(r.returncode,0);self.assertEqual((root/'www/latest/retained').read_text(),'previous publication')
    def test_phone_publication_requires_signature(self):
        root,_,_,r=self.run_publication(key=False);self.assertNotEqual(r.returncode,0);self.assertEqual((root/'www/latest/retained').read_text(),'previous publication')

if __name__=='__main__':unittest.main()
