#!/usr/bin/env python3
"""Native MSVC component qualification only; never starts a node or installer."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tomllib
import traceback
import verify_orchard_windows as gate


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fingerprint(root):
    names = subprocess.check_output(['git', 'ls-files', '--recurse-submodules', '-z'], cwd=root).decode().split('\0')
    result = {}
    for name in filter(None, names):
        path = root/name
        # Hash a tracked link as a link, never open its runtime/external target.
        result[name] = ({'kind': 'symlink', 'target': os.readlink(path)}
                        if path.is_symlink() else
                        {'kind': 'file', 'sha256': digest(path)})
    return result


def qualify(args):
    root=args.source.resolve();out=args.output.resolve()
    actual=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip()
    gate.require(actual==args.expected_sha,'Unexpected checkout')
    gate.require(not subprocess.check_output(['git','status','--porcelain','--untracked-files=no'],cwd=root).strip(),'Dirty tracked source')
    initial=fingerprint(root)
    pin=tomllib.loads((root/'rust/orchard_backend/rust-toolchain.toml').read_text())['toolchain']['channel']
    gate.require(re.fullmatch(r'\d+\.\d+\.\d+',pin),'Invalid pinned toolchain')
    evidence=out/'evidence';build=out/'build'
    (evidence/'source.json').write_text(json.dumps({'head':actual,'files':initial,'rust':pin},indent=2)+'\n')
    env=dict(os.environ,CARGO_BUILD_JOBS='2',RAYON_NUM_THREADS='2',CMAKE_BUILD_PARALLEL_LEVEL='2',OPENSSL_VERSION='3.5.7',OPENSSL_REBUILD='1')
    # Do not inherit a caller's compiler parallelism or alternate dependency roots.
    for name in ['CL','_CL_','CFLAGS','CXXFLAGS','OPENSSL_SOURCE_DIR','OPENSSL_OUTPUT_DIR']:
        gate.require(not env.get(name),'Uncontrolled '+name)
    commands=[]
    def run(label,command,cap):
        row={'name':label,'command':[str(x) for x in command],'timeout_seconds':cap};commands.append(row)
        (evidence/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
        with (evidence/(label+'.log')).open('wb') as stream:
            child=subprocess.Popen(row['command'],cwd=root,env=env,stdout=stream,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
            row['pid']=child.pid
            try:row['exit_code']=child.wait(timeout=cap)
            except subprocess.TimeoutExpired:
                subprocess.run(['taskkill','/PID',str(child.pid),'/T','/F'],check=False,stdout=stream,stderr=subprocess.STDOUT)
                child.wait();row['timed_out']=True;raise
            finally:(evidence/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
        gate.require(row['exit_code']==0,label+' failed; see retained log')
        return (evidence/(label+'.log')).read_text(encoding='utf-8-sig',errors='replace')
    result={'status':'failure','scope':'Native Windows standalone build and two non-IPC component tests only'}
    try:
        run('driver-contracts',[sys.executable,'-B',root/'scripts/ci/test_run_orchard_windows.py'],120)
        run('contracts',[sys.executable,'-B',root/'scripts/ci/test_orchard_msvc_bridge.py'],120)
        run('evidence-contracts',[sys.executable,'-B',root/'scripts/ci/test_verify_orchard_windows.py'],120)
        caps=json.loads(run('cmake-capabilities',['cmake','-E','capabilities'],60));gate.require((caps['version']['major'],caps['version']['minor'])>=(3,21),'CTest JUnit support required')
        run('rust-version',['rustc','+'+pin,'-vV'],60)
        run('openssl',['pwsh','-NoProfile','-File',root/'scripts/build-openssl-vendored.ps1'],1800)
        crypto=root/'third_party/openssl-3.5.7/prebuilt/windows-x86_64-msvc'
        gate.require((crypto/'libcrypto.lib').is_file(),'Missing fresh OpenSSL artifact')
        sqlite=out/'sqlite-source';sqlite.mkdir();sqlite_build=out/'sqlite-build'
        (sqlite/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.21)\nproject(orchard_qualification_sqlite LANGUAGES C)\nadd_subdirectory("'+(root/'third_party/sqlite-amalgamation-3480000').as_posix()+'" sqlite)\n')
        run('sqlite-configure',['cmake','-S',sqlite,'-B',sqlite_build,'-G','Ninja','-DCMAKE_BUILD_TYPE=Release','-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL'],120)
        run('sqlite-build',['cmake','--build',sqlite_build,'--parallel','2'],600)
        run('configure',['cmake','-S',root/'rust/orchard_backend','-B',build,'-G','Ninja','-DCMAKE_BUILD_TYPE=Release','-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL','-DCMAKE_EXPORT_COMPILE_COMMANDS=ON','-DBUILD_TESTING=ON','-DDINERO_ORCHARD_WINDOWS_QUALIFICATION=ON','-DOPENSSL_USE_STATIC_LIBS=TRUE','-DOPENSSL_ROOT_DIR='+str(crypto),'-DSQLite3_INCLUDE_DIR='+str(root/'third_party/sqlite-amalgamation-3480000'),'-DSQLite3_LIBRARY='+str(sqlite_build/'lib/sqlite3.lib')],180)
        run('rust-build',['cmake','--build',build,'--target','dinero_orchard_rust_build','--parallel','1'],1200)
        run('native-build',['cmake','--build',build,'--parallel','4','--verbose'],1200)
        full=run('inventory',['ctest','--test-dir',build,'-N','--show-only=json-v1'],60);(evidence/'inventory.json').write_text(full)
        select='^(OrchardBackendCpp|OrchardWalletSpend)$'
        selected=run('selected',['ctest','--test-dir',build,'-N','--show-only=json-v1','-R',select],60);(evidence/'selected.json').write_text(selected)
        gate.inventory(json.loads(full),root,build);gate.inventory(json.loads(selected),root,build,selected=True)
        run('ctest',['ctest','--test-dir',build,'-R',select,'--verbose','--no-tests=error','--parallel','1','--output-junit',evidence/'results.xml'],1300)
        gate.reports((evidence/'results.xml').read_text(),(evidence/'ctest.log').read_text(encoding='utf-8-sig'))
        result.update(status='success',executed_tests=list(gate.SELECTED))
    except Exception as error:
        result['error']=str(error);(evidence/'exception.txt').write_text(traceback.format_exc())
    finally:
        try:
            gate.require(fingerprint(root)==initial,'Tracked source changed during qualification')
            result['source_unchanged']=True
        except Exception as error:result.update(status='failure',source_error=str(error))
        for name in ['CMakeCache.txt','compile_commands.json','build.ninja','orchard-native-libraries.rsp','orchard-native-libraries.rsp.cargo.stdout','orchard-native-libraries.rsp.cargo.stderr','CTestTestfile.cmake','Testing/Temporary/LastTest.log']:
            src=build/name
            if src.is_file():dest=evidence/'build'/name;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(src,dest)
        artifacts={str(p.relative_to(out)):{'sha256':digest(p),'bytes':p.stat().st_size} for p in out.rglob('*') if p.is_file() and p.suffix.lower() in ('.exe','.lib','.obj')}
        (evidence/'artifacts.json').write_text(json.dumps(artifacts,indent=2)+'\n');(evidence/'terminal.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result));return 0 if result['status']=='success' else 1
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--source',type=Path,required=True);parser.add_argument('--output',type=Path,required=True);parser.add_argument('--expected-sha',required=True);args=parser.parse_args()
    gate.require(sys.platform == 'win32', 'Native Windows qualification required')
    gate.require(re.fullmatch('[0-9a-f]{40}',args.expected_sha), 'Expected full source SHA')
    root=args.source.resolve();out=args.output.resolve()
    gate.require(not out.exists() and not out.is_relative_to(root), 'Fresh external build directory required')
    out.mkdir();evidence=out/'evidence';evidence.mkdir()
    (evidence/'request.json').write_text(json.dumps({'expected_sha':args.expected_sha})+'\n')
    try:
        return qualify(args)
    except Exception as error:
        # Preflight failures must remain inspectable through the always-upload step.
        result={'status':'failure','stage':'driver','error':str(error)}
        (evidence/'exception.txt').write_text(traceback.format_exc())
        (evidence/'terminal.json').write_text(json.dumps(result,indent=2)+'\n')
        print(json.dumps(result));return 1

if __name__=='__main__':sys.exit(main())
