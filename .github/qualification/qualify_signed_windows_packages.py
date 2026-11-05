#!/usr/bin/env python3
"""Qualify already signed candidate bytes without compiling, signing or publishing."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import zipfile

SOURCE = '249b5604def52ff6e1eb42ba265d60f98e7600e6'
PRODUCER = 'e916dbb9652dc9a3a3eccfce4d392592df1a42db'
INSTALLERS = {
    'Dinero-8.2.0-windows-x86_64-Setup.exe': '05a839643111c14499bdd72bb6988d7b9da2fb6362f471707964ac0d687a20cb',
    'Dinero-Server-8.2.0-windows-x86_64-Setup.exe': '9a310630534fd7e8e9c7fb8dc2f67679432a45771642e26ae6bb0b8016d43992',
}
OUTPUT = Path('qualified-windows')


def require(ok, reason):
    if not ok:
        raise RuntimeError(reason)


def powershell(script, log_name, *, expect_success=True):
    path = OUTPUT/(log_name+'.ps1')
    path.write_text(script, encoding='utf-8')
    result = subprocess.run(['powershell','-NoProfile','-ExecutionPolicy','Bypass','-File',
                             str(path.resolve())], capture_output=True, timeout=650)
    log = result.stdout+result.stderr
    (OUTPUT/(log_name+'.log')).write_bytes(log)
    print(log.decode('utf-8',errors='replace'))
    if expect_success:
        require(result.returncode == 0, 'native check failed: '+log_name)
    return result


def main():
    OUTPUT.mkdir()
    paths = {}
    for name, pin in INSTALLERS.items():
        matches = list(Path('signed-input').rglob(name))
        require(len(matches) == 1 and hashlib.sha256(matches[0].read_bytes()).hexdigest() == pin,
                'signed producer installer differs: '+name)
        paths[name] = matches[0]
        powershell("$ErrorActionPreference='Stop'\nif ((Get-AuthenticodeSignature -LiteralPath '"+
                   str(matches[0].resolve())+"').Status -ne 'Valid') { throw 'Invalid installer signature' }\n",
                   'signature-'+name)
    temp = Path(os.environ['RUNNER_TEMP'])
    unpack = temp/'signed-gui-payload'
    subprocess.run([r'C:\Program Files\7-Zip\7z.exe','x','-y','-o'+str(unpack),
                    str(paths['Dinero-8.2.0-windows-x86_64-Setup.exe'])],
                   check=True, stdout=subprocess.DEVNULL)
    subprocess.run(['git','fetch','--depth=1','origin',PRODUCER,SOURCE,
                    '77cd515ba72819bf4804e9e4231311ec6a5fc26b'],check=True)
    helper = subprocess.check_output(['git','show',PRODUCER+':packaging/windows/package_signed_operator.py'])
    require(hashlib.sha256(helper).hexdigest() == 'ccb9c7f07c63d061c1da00f36d98d0c7b3376628125f90007206cb49569fb7b2',
            'reviewed portable helper differs')
    helper_path = temp/'package_signed_operator.py';helper_path.write_bytes(helper)
    spec = importlib.util.spec_from_file_location('signed_operator',helper_path)
    package = importlib.util.module_from_spec(spec);spec.loader.exec_module(package)
    stage = temp/'signed-operator-stage';stage.mkdir()
    payload = {}
    for name in package.TOOLS + ('nvrtc64_120_0.dll','nvrtc-builtins64_122.dll'):
        matches = list(unpack.rglob(name))
        require(len(matches) == 1, 'GUI signed payload inventory differs: '+name)
        data = matches[0].read_bytes();(stage/name).write_bytes(data)
        payload[name] = hashlib.sha256(data).hexdigest()
    license_file = temp/'operator-LICENSE'
    license_file.write_bytes(subprocess.check_output(['git','show',SOURCE+':LICENSE']))
    dist = Path('packaging/windows/dist');dist.mkdir(parents=True,exist_ok=True)
    portable_name = 'dinero-v8.2.0-windows-x86_64-msvc.zip'
    package.assemble(stage,license_file,dist/portable_name,'8.2.0')
    final = temp/'qualified-portable'
    with zipfile.ZipFile(dist/portable_name) as archive:
        require(archive.testzip() is None and len(archive.namelist()) == 14, 'portable archive differs')
        archive.extractall(final)
        for line in archive.read('SHA256SUMS.txt').decode().splitlines():
            pin,name = line.split('  ',1)
            require(hashlib.sha256(archive.read(name)).hexdigest() == pin, 'portable inner checksum differs')
    for name,pin in payload.items():
        path = final/'bin'/name
        require(hashlib.sha256(path.read_bytes()).hexdigest() == pin, 'signed payload changed')
        powershell("$ErrorActionPreference='Stop'\nif ((Get-AuthenticodeSignature -LiteralPath '"+
                   str(path)+"').Status -ne 'Valid') { throw 'Invalid payload signature' }\n",
                   'signature-'+name)
    clean = dict(os.environ,PATH=os.environ['SystemRoot']+'\\System32;'+os.environ['SystemRoot'])
    daemon = final/'bin/dinerod.exe'
    require(SOURCE in subprocess.check_output([str(daemon),'--version'],env=clean,text=True),
            'portable daemon source differs')
    server_unpack = temp/'signed-server-payload'
    subprocess.run([r'C:\Program Files\7-Zip\7z.exe','x','-y','-o'+str(server_unpack),
                    str(paths['Dinero-Server-8.2.0-windows-x86_64-Setup.exe'])],
                   check=True,stdout=subprocess.DEVNULL)
    server_daemons = list(server_unpack.rglob('dinerod.exe'))
    require(len(server_daemons) == 1 and SOURCE in subprocess.check_output(
        [str(server_daemons[0]),'--version'],env=clean,text=True), 'server daemon source differs')
    powershell("$ErrorActionPreference='Stop'\nif ((Get-AuthenticodeSignature -LiteralPath '"+
               str(server_daemons[0])+"').Status -ne 'Valid') { throw 'Invalid server daemon signature' }\n",
               'signature-server-daemon')
    workflow = Path('.github/workflows/windows-release.yml').read_text()
    # Execute the permanent release workflow's actual polling code against a
    # guaranteed closed RPC endpoint; do not duplicate a test-only implementation.
    begin = workflow.index('                  $priorPreference = $ErrorActionPreference')
    end = workflow.index('                  if ($cliExit -eq 0)',begin)
    block = '\n'.join(line[18:] for line in workflow[begin:end].splitlines())
    fixture = temp/'readiness-refusal';fixture.mkdir()
    (fixture/'dinero.conf').write_text('rpcport=1\nrpchost=127.0.0.1\n')
    setup = "$ErrorActionPreference='Stop'\n$serverData='"+str(fixture)+"'\n$case=@{Dir='"+str(final/'bin')+"'}\n"
    old = powershell(setup+"& (Join-Path $case.Dir 'dinero-cli.exe') \"-datadir=$serverData\" getblockchaininfo 2>$null\n",
                     'old-warmup-refusal',expect_success=False)
    require(old.returncode != 0 and b'NativeCommandError' in old.stdout+old.stderr,
            'old warm-up failure did not reproduce')
    powershell(setup+block+"\nif ($cliExit -eq 0) { throw 'Closed RPC endpoint unexpectedly ready' }\nWrite-Host 'WARMUP_REFUSAL_CAPTURED_WITHOUT_ABORT'\n",
               'fixed-warmup-refusal')
    # Copy the two installers byte-for-byte to the exact paths used by the
    # permanent release lane, then execute its full real installation checks.
    for name,path in paths.items():
        (dist/name).write_bytes(path.read_bytes())
    section = workflow.split('      - name: Silent GUI and server installer smoke\n',1)[1]
    section = section.split('      - name: Preserve failed installer diagnostics',1)[0]
    body = section.split('        run: |\n',1)[1]
    script = '\n'.join(line[10:] if line.startswith('          ') else line for line in body.splitlines())
    script = script.replace('${{ steps.ver.outputs.version }}','8.2.0')
    require('${{' not in script, 'unresolved release script expression')
    installed = powershell("$ErrorActionPreference='Stop'\n"+script,'installed-qt-server')
    require(b'Fresh mainnet server ready and stable at height' in installed.stdout,
            'real installed server readiness not proven')
    revision = '77cd515ba72819bf4804e9e4231311ec6a5fc26b'
    harnesses = {
        'owned':('.github/qualification/wallet_reorg_owned_replacement.py','91d84c9bed99e6afd9dee720810400706aff83f7100547432bfc3ef12fa77856'),
        'mid':('.github/qualification/wallet_reorg_midtransition.py','0b61911713f95dee77104e5df3d70b05fa3c9fe116c5fcff90bfda82d320a697')}
    for key,(name,pin) in list(harnesses.items()):
        data = subprocess.check_output(['git','show',revision+':'+name])
        require(hashlib.sha256(data).hexdigest() == pin,'wallet harness differs')
        path = temp/Path(name).name;path.write_bytes(data);harnesses[key] = str(path)
    for label,key,args in [('normal','owned',[]),('pending-restart','owned',['--crash-pending-connect']),('midtransition','mid',[])]:
        result = subprocess.run([sys.executable,harnesses[key],*args],
                                env=dict(clean,DINEROD=str(daemon)),capture_output=True,timeout=360)
        log = result.stdout+result.stderr;(OUTPUT/('wallet-'+label+'.log')).write_bytes(log)
        print(log.decode('utf-8',errors='replace'))
        require(result.returncode == 0,'shipped wallet recovery failed: '+label)
    files = {}
    for name in tuple(INSTALLERS)+(portable_name,):
        data = (dist/name).read_bytes();(OUTPUT/name).write_bytes(data)
        files[name] = hashlib.sha256(data).hexdigest()
    (OUTPUT/'SHA256SUMS-windows-x86_64-8.2.0').write_text(
        ''.join(pin+'  '+name+'\n' for name,pin in sorted(files.items())),encoding='ascii')
    receipt = {'status':'PASS','source_sha':SOURCE,'producer_run':42108289368,
               'producer_tooling_commit':PRODUCER,'qualification_tooling_commit':os.environ['GITHUB_SHA'],
               'native_signature_files':14,'unchanged_signed_payloads':11,'compiled_executables':0,
               'warmup_refusal_negative_control':'PASS','fixed_warmup_capture':'PASS',
               'fresh_mainnet_service_RPC_and_stable_PID':'PASS','installed_gui':'PASS',
               'clean_PATH':'PASS','wallet_cases':3,'files':files,'payload_sha256':payload}
    (OUTPUT/'qualification.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps(receipt))


if __name__ == '__main__':
    main()
