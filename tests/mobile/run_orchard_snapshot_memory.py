"""Run each capacity phase in a fresh process; report actual process high-water RSS.

This is allocator/platform-specific codec capacity evidence, NOT a valid chain,
NodeCore service lifecycle, device jetsam, or RocksDB installation receipt.
"""
import argparse
import json
import platform
from pathlib import Path
import subprocess
import tempfile
import shlex


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--counts', type=int, nargs='+', default=[10000, 100000])
    parser.add_argument('--max-rss-mib', type=int)
    parser.add_argument('--report', type=Path)
    executor = parser.add_mutually_exclusive_group()
    executor.add_argument('--adb-serial', help='run the cross-built binary on this Android test device')
    executor.add_argument('--simulator', help='run the cross-built binary on this booted iOS simulator UUID')
    args = parser.parse_args()
    rows = []
    failed = False
    adb = ['adb', '-s', args.adb_serial] if args.adb_serial else None
    remote = None
    if adb:
        remote = subprocess.check_output(adb+['shell','mktemp','-d','/data/local/tmp/orchard-v6-memory.XXXXXX'],text=True).strip()
        if not remote.startswith('/data/local/tmp/orchard-v6-memory.') or '/' in remote.removeprefix('/data/local/tmp/'):
            raise RuntimeError('unexpected Android temporary directory')
        subprocess.run(adb+['push',str(args.binary.resolve()),remote+'/benchmark'],check=True,capture_output=True)
        subprocess.run(adb+['shell','chmod','700',remote+'/benchmark'],check=True)
    try:
      with tempfile.TemporaryDirectory(prefix='orchard-v6-memory-') as temp:
        for count in args.counts:
            for profile in ('full', 'compact'):
                path = (remote+'/' if remote else str(Path(temp))+'/') + f'{profile}-{count}.v6'
                for phase in ('generate', 'read', 'proof-roundtrip'):
                    command = [remote+'/benchmark' if remote else str(args.binary.resolve()),phase,profile,str(count),path]
                    if adb:
                        command = adb+['shell',shlex.join(command)]
                    elif args.simulator:
                        command = ['xcrun','simctl','spawn',args.simulator]+command
                    result = subprocess.run(command,
                                            capture_output=True, text=True, timeout=1800)
                    if result.returncode:
                        raise RuntimeError(f'{phase}/{profile}/{count}: {result.stdout}\n{result.stderr}')
                    row = json.loads(result.stdout.strip().splitlines()[-1])
                    row['within_budget'] = (args.max_rss_mib is None or
                                             row['peak_rss_bytes'] <= args.max_rss_mib * 1024**2)
                    failed |= not row['within_budget']
                    rows.append(row)
                    print(json.dumps(row), flush=True)
                if adb:
                    subprocess.run(adb+['shell','rm','-f',path,path+'.proof'],check=True)
    finally:
        if adb and remote:
            # Only exact test-owned files, never recursive device cleanup.
            for count in args.counts:
                for profile in ('full','compact'):
                    path=remote+f'/{profile}-{count}.v6'
                    subprocess.run(adb+['shell','rm','-f',path,path+'.proof'],check=True)
            subprocess.run(adb+['shell','rm','-f',remote+'/benchmark'],check=True)
            subprocess.run(adb+['shell','rmdir',remote],check=True)
    report = dict(platform=platform.platform(), machine=platform.machine(),
                  max_rss_mib=args.max_rss_mib, measurements=rows)
    if adb:
        report['executor']='android-device'
        report['android_properties']={key:subprocess.check_output(adb+['shell','getprop',key],text=True).strip()
            for key in ('ro.build.version.sdk','ro.build.version.release','ro.product.cpu.abi',
                        'ro.product.model','ro.kernel.qemu')}
    elif args.simulator:
        report['executor']='ios-simulator'
        report['simulator']=args.simulator
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    return 1 if failed else 0


if __name__ == '__main__':
    raise SystemExit(main())
