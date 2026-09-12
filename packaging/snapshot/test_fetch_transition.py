import hashlib,json,os,struct,subprocess,tempfile,unittest
from pathlib import Path
SCRIPT=Path(__file__).with_name('dinero-snapshot-fetch.sh').resolve()
class FetchTransition(unittest.TestCase):
 def run_case(self,version,height,signed=False,require=False,declared=None):
  with tempfile.TemporaryDirectory() as tmp:
   root=Path(tmp);mirror=root/'mirror';mirror.mkdir();bin_dir=root/'bin';bin_dir.mkdir();dest=root/'dest'
   payload=b'UXTO'+struct.pack('<I',version)+bytes(32)+struct.pack('<I',height)+bytes(1048576)
   (mirror/'snapshot.dat').write_bytes(payload)
   (mirror/'manifest.json').write_text(json.dumps({'network':'mainnet','snapshot':{'height':height if declared is None else declared,'bytes':len(payload),'sha256':hashlib.sha256(payload).hexdigest()}}))
   if signed:(mirror/'manifest.sig').write_text('fixture')
   curl=bin_dir/'curl';curl.write_text('#!/bin/bash\nwhile [ "$#" -gt 0 ]; do case "$1" in -o) out=$2; shift 2;; *) url=$1; shift;; esac; done\ncp "${url#file://}" "$out" 2>/dev/null\n');curl.chmod(0o755)
   ssl=bin_dir/'openssl';ssl.write_text('#!/bin/sh\nexit '+('0' if signed else '1')+'\n');ssl.chmod(0o755)
   args=['bash',str(SCRIPT),'--dest',str(dest),'--mirror','file://'+str(mirror)]
   if require:args+=['--require-publisher-signature']
   r=subprocess.run(args,env=dict(os.environ,PATH=str(bin_dir)+os.pathsep+os.environ['PATH']),capture_output=True,text=True)
   return r.returncode,(dest/'snapshot.dat').exists(),r.stdout+r.stderr
 def test_unsigned_activated_v5_staged_only(self):
  rc,exists,out=self.run_case(5,111000);self.assertEqual(rc,0);self.assertTrue(exists);self.assertIn('awaiting daemon validation',out)
 def test_unsigned_v4_rejected(self):
  rc,exists,_=self.run_case(4,110999);self.assertNotEqual(rc,0);self.assertFalse(exists)
 def test_signed_v4_preserved(self):
  rc,exists,_=self.run_case(4,110999,True);self.assertEqual(rc,0);self.assertTrue(exists)
 def test_signature_pin_rejects_unsigned_v5(self):
  rc,exists,_=self.run_case(5,111000,require=True);self.assertNotEqual(rc,0);self.assertFalse(exists)
 def test_signed_activated_v4_rejected(self):
  rc,exists,_=self.run_case(4,111000,True);self.assertNotEqual(rc,0);self.assertFalse(exists)
 def test_height_lie_rejected(self):
  rc,exists,_=self.run_case(5,110999,declared=111000);self.assertNotEqual(rc,0);self.assertFalse(exists)
 def test_unknown_version_rejected(self):
  rc,exists,_=self.run_case(6,111000,True);self.assertNotEqual(rc,0);self.assertFalse(exists)
 def test_pre_activation_v5_requires_signature(self):
  rc,exists,_=self.run_case(5,110999);self.assertNotEqual(rc,0);self.assertFalse(exists)
if __name__=='__main__':unittest.main()
