"""Assemble qualified slices without touching DineroDPI's installed framework."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess

from package_nodecore_archive import digest

ROOT=Path(__file__).resolve().parents[2]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('ios-device','ios-simulator','macos-arm64','macos-x86_64'):
        parser.add_argument('--'+name,type=Path,required=True,help='qualified slice build directory')
    parser.add_argument('--output',type=Path,required=True,help='new SDK directory (must not exist)')
    args=parser.parse_args()
    inputs={name:getattr(args,name.replace('-','_')).resolve()/'sdk'/('libnodecore-'+name+'.a')
            for name in ('ios-device','ios-simulator','macos-arm64','macos-x86_64')}
    for name,path in inputs.items():
        if not path.is_file():
            parser.error('missing slice: '+str(path))
        manifest=json.loads(path.with_suffix('.manifest.json').read_text())
        if not manifest.get('qualification_only') or manifest.get('sha256')!=digest(path):
            parser.error('missing or stale slice link receipt: '+str(path))
        subprocess.run(['xcrun','lipo',str(path),'-verify_arch','x86_64' if name.endswith('x86_64') else 'arm64'],check=True)
    output=args.output.resolve()
    if output.exists():
        parser.error('refusing to replace existing SDK directory: '+str(output))
    output.mkdir(parents=True)
    headers=output/'Headers';headers.mkdir()
    shutil.copy2(ROOT/'include/nodecore/nodecore_ffi.h',headers/'nodecore_ffi.h')
    universal=output/'libnodecore-macos-universal.a'
    subprocess.run(['xcrun','lipo','-create',str(inputs['macos-arm64']),str(inputs['macos-x86_64']),
                    '-output',str(universal)],check=True)
    framework=output/'NodeCore.xcframework'
    command=['xcodebuild','-create-xcframework']
    for archive in (inputs['ios-device'],inputs['ios-simulator'],universal):
        command+=['-library',str(archive),'-headers',str(headers)]
    subprocess.run(command+['-output',str(framework)],check=True)
    (output/'manifest.json').write_text(json.dumps(dict(qualification_only=True,dnorst02='dormant',
        inputs={name:dict(path=str(path),sha256=digest(path)) for name,path in inputs.items()},
        header_sha256=digest(headers/'nodecore_ffi.h')),indent=2)+'\n')


if __name__=='__main__':
    main()
