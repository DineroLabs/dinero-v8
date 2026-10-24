"""Assemble qualified slices without touching DineroDPI's installed framework."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess

from package_nodecore_archive import digest
from sdk_consensus_manifest import compose_receipts

ROOT=Path(__file__).resolve().parents[2]
# Public C headers: the node FFI and the Orchard wallet ABI (keys, notes,
# witnesses, spend/shield preparation, proving) already linked into the archive.
HEADERS={'nodecore_ffi.h':ROOT/'include/nodecore/nodecore_ffi.h',
         'orchard_backend_ffi.h':ROOT/'rust/orchard_backend/include/orchard_backend_ffi.h'}


def stage_headers(headers):
    for name,source in HEADERS.items():
        shutil.copy2(source,headers/name)
    return {name:digest(headers/name) for name in HEADERS}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('ios-device','ios-simulator','macos-arm64','macos-x86_64'):
        parser.add_argument('--'+name,type=Path,required=True,help='qualified slice build directory')
    parser.add_argument('--output',type=Path,required=True,help='new SDK directory (must not exist)')
    args=parser.parse_args()
    inputs={name:getattr(args,name.replace('-','_')).resolve()/'sdk'/('libnodecore-'+name+'.a')
            for name in ('ios-device','ios-simulator','macos-arm64','macos-x86_64')}
    receipts=[]
    for name,path in inputs.items():
        if not path.is_file():
            parser.error('missing slice: '+str(path))
        manifest=json.loads(path.with_suffix('.manifest.json').read_text())
        if not manifest.get('qualification_only') or manifest.get('sha256')!=digest(path):
            parser.error('missing or stale slice link receipt: '+str(path))
        receipts.append(manifest)
        subprocess.run(['xcrun','lipo',str(path),'-verify_arch','x86_64' if name.endswith('x86_64') else 'arm64'],check=True)
    provenance=compose_receipts(receipts,ROOT)
    output=args.output.resolve()
    if output.exists():
        parser.error('refusing to replace existing SDK directory: '+str(output))
    output.mkdir(parents=True)
    headers=output/'Headers';headers.mkdir()
    header_digests=stage_headers(headers)
    universal=output/'libnodecore-macos-universal.a'
    subprocess.run(['xcrun','lipo','-create',str(inputs['macos-arm64']),str(inputs['macos-x86_64']),
                    '-output',str(universal)],check=True)
    framework=output/'NodeCore.xcframework'
    command=['xcodebuild','-create-xcframework']
    for archive in (inputs['ios-device'],inputs['ios-simulator'],universal):
        command+=['-library',str(archive),'-headers',str(headers)]
    subprocess.run(command+['-output',str(framework)],check=True)
    (output/'manifest.json').write_text(json.dumps(dict(qualification_only=True,**provenance,
        inputs={name:dict(path=str(path),sha256=digest(path)) for name,path in inputs.items()},
        header_sha256=header_digests['nodecore_ffi.h'],headers_sha256=header_digests),indent=2)+'\n')


if __name__=='__main__':
    main()
