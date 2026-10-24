"""Build an opt-in Orchard SDK slice without updating any consumer app or schedule.

OpenSSL must already be built for the exact requested platform. SDK artifacts
are qualification candidates, never automatic releases or app replacements.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from package_nodecore_archive import digest, package

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', choices=['ios-device', 'ios-simulator', 'macos-arm64',
                                               'macos-x86_64', 'android-arm64', 'android-x86_64'], required=True)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--openssl', type=Path, required=True, help='platform prefix with include/ and libcrypto.a or lib/libcrypto.a')
    parser.add_argument('--ndk', type=Path)
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--memory-benchmark', action='store_true')
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    build, crypto = args.build.resolve(), args.openssl.resolve()
    libs = crypto / 'lib' if (crypto / 'lib/libcrypto.a').is_file() else crypto
    for file in (libs/'libcrypto.a', libs/'libssl.a', crypto/'include/openssl/opensslv.h'):
        if not file.is_file():
            parser.error(f'missing target OpenSSL input: {file}')
    options = ['-DCMAKE_BUILD_TYPE=Release', '-DBUILD_NODECORE=ON', '-DBUILD_TESTING=OFF',
               '-DDINERO_ORCHARD_BUILD_TESTS=OFF', '-DDINERO_BUILD_ORCHARD_BACKEND=ON',
               '-DDINERO_ORCHARD_MOBILE_QUALIFICATION=ON', '-DENABLE_HARDWARE_WALLETS=OFF',
               '-DBUILD_LIGHTNINGD=OFF', '-DDINERO_ENABLE_PORTMAPPING=OFF',
               '-DDINERO_VENDORED_OPENSSL_DIR='+str(libs),
               '-DDINERO_VENDORED_OPENSSL_SOURCE_DIR='+str(crypto)]
    targets = ['nodecore_link_smoke']
    if args.memory_benchmark:
        options.append('-DDINERO_ORCHARD_MEMORY_BENCHMARK=ON')
        targets.append('orchard_snapshot_memory')
    if args.platform.startswith('android-'):
        if args.ndk is None or not (args.ndk/'build/cmake/android.toolchain.cmake').is_file():
            parser.error('--ndk must identify an installed NDK')
        options += ['-DCMAKE_TOOLCHAIN_FILE='+str(args.ndk.resolve()/'build/cmake/android.toolchain.cmake'),
                    '-DANDROID_ABI='+('arm64-v8a' if args.platform.endswith('arm64') else 'x86_64'),
                    '-DANDROID_PLATFORM=28']
    elif args.platform.startswith('ios-'):
        options += ['-DCMAKE_TOOLCHAIN_FILE='+str(ROOT/'cmake/ios.toolchain.cmake'),
                    '-DIOS_PLATFORM='+('OS' if args.platform=='ios-device' else 'SIMULATOR'),
                    '-DCMAKE_OSX_ARCHITECTURES=arm64', '-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0']
    else:
        options += ['-DCMAKE_OSX_ARCHITECTURES='+args.platform.removeprefix('macos-'),
                    '-DCMAKE_OSX_DEPLOYMENT_TARGET='+(ROOT/'.macos-deployment-target').read_text().strip()]
    subprocess.run(['cmake', '-S', str(ROOT), '-B', str(build), '-G', 'Ninja', *options], check=True)
    subprocess.run(['cmake', '--build', str(build), '--target', *targets, '-j', str(args.jobs)], check=True)
    provenance = dict(platform=args.platform, qualification_only=True, dnorst02='dormant',
                      source_base=subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip(),
                      source_dirty=bool(subprocess.check_output(['git', '-C', str(ROOT),
                          '-c', 'diff.ignoreSubmodules=all', 'status', '--porcelain'], text=True)))
    if args.platform.startswith('android-'):
        library=build/'lib/libnodecore_ffi.so'
        provenance.update(library=str(library), sha256=digest(library))
        (build/'nodecore-sdk.manifest.json').write_text(json.dumps(provenance, indent=2)+'\n')
    else:
        command=subprocess.check_output(['ninja','-C',str(build),'-t','commands','nodecore_link_smoke'],text=True)
        manifest=package(build,command,build/('sdk/libnodecore-'+args.platform+'.a'))
        provenance.update(manifest)
        (build/'nodecore-sdk.manifest.json').write_text(json.dumps(provenance, indent=2)+'\n')


if __name__ == '__main__':
    main()
