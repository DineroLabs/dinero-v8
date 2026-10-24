"""Stage both DineroDPI JNI ABIs, symbols and receipts; never replace app inputs."""
import argparse
import json
from pathlib import Path
import shutil
import struct
import subprocess

from package_nodecore_archive import digest

ROOT=Path(__file__).resolve().parents[2]


def check_elf(path,machine):
    data=path.read_bytes()
    if len(data)<64 or data[:6]!=b'\x7fELF\x02\x01':
        raise ValueError('expected a 64-bit little-endian ELF library')
    if struct.unpack_from('<HH',data,16)!=(3,machine):
        raise ValueError('wrong shared library architecture')
    offset=struct.unpack_from('<Q',data,32)[0]
    width,count=struct.unpack_from('<HH',data,54)
    if width!=56 or not count or offset>len(data) or count>(len(data)-offset)//width:
        raise ValueError('invalid program header table')
    loads=0
    for i in range(count):
        at=offset+i*width
        if struct.unpack_from('<I',data,at)[0]==1:
            loads+=1
            file_offset,address=struct.unpack_from('<QQ',data,at+8)
            alignment=struct.unpack_from('<Q',data,at+48)[0]
            if alignment<16384 or file_offset%16384!=address%16384:
                raise ValueError('JNI library is not 16 KiB page compatible')
    if not loads:
        raise ValueError('no ELF load segments')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arm64',type=Path,required=True,help='arm64 slice build directory')
    parser.add_argument('--x86-64',type=Path,required=True,help='x86_64 slice build directory')
    parser.add_argument('--ndk',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True,help='new SDK directory (must not exist)')
    args=parser.parse_args()
    tools=list((args.ndk/'toolchains/llvm/prebuilt').glob('*/bin'))
    if len(tools)!=1:
        parser.error('expected exactly one NDK host toolchain')
    bin_dir=tools[0]
    sources={'arm64-v8a':args.arm64.resolve(),'x86_64':args.x86_64.resolve()}
    for abi,build in sources.items():
        library=build/'lib/libnodecore_ffi.so'
        receipt=json.loads((build/'nodecore-sdk.manifest.json').read_text())
        if not receipt.get('qualification_only') or receipt.get('sha256')!=digest(library):
            parser.error('missing or stale JNI build receipt: '+str(build))
        check_elf(library,183 if abi=='arm64-v8a' else 62)
    output=args.output.resolve()
    if output.exists():
        parser.error('refusing to replace existing SDK directory: '+str(output))
    output.mkdir(parents=True)
    manifest=dict(qualification_only=True,dnorst02='dormant',abis={})
    for abi,build in sources.items():
        source=build/'lib/libnodecore_ffi.so'
        target=output/'jniLibs'/abi/source.name;target.parent.mkdir(parents=True)
        symbols=output/'symbols'/abi/(source.name+'.dbg');symbols.parent.mkdir(parents=True)
        shutil.copy2(source,target)
        subprocess.run([str(bin_dir/'llvm-objcopy'),'--only-keep-debug',str(source),str(symbols)],check=True)
        subprocess.run([str(bin_dir/'llvm-strip'),'--strip-unneeded',str(target)],check=True)
        check_elf(target,183 if abi=='arm64-v8a' else 62)
        exports=subprocess.check_output([str(bin_dir/'llvm-nm'),'--dynamic','--defined-only',str(target)],text=True)
        for symbol in ('nodecore_start','nodecore_stop','nodecore_rpc_call',
                       'Java_org_dinerolabs_dinerodpi_node_NativeNodeCore_nativeStart',
                       'Java_org_dinerolabs_dinerodpi_node_NativeNodeCore_nativeStatusJson'):
            if not any(line.split()[-1]==symbol for line in exports.splitlines() if line.split()):
                raise ValueError('missing packaged JNI/C ABI export: '+symbol)
        manifest['abis'][abi]=dict(source=str(source),source_sha256=digest(source),
            packaged_sha256=digest(target),debug_sha256=digest(symbols),page_alignment=16384)
    shutil.copy2(ROOT/'include/nodecore/nodecore_ffi.h',output/'nodecore_ffi.h')
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')


if __name__=='__main__':
    main()
