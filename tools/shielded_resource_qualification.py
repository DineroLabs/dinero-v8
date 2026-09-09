#!/usr/bin/env python3
"""Measure fresh proof processes; report qualification against explicit budgets.

This qualifies the proof component on the measured host, not total daemon memory,
mobile capability, or an unmeasured machine with the same core/RAM count.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import subprocess
import time

SHAPES = ('shield','transfer_1in_2out','transfer_2in_2out','transfer_4in_2out','unshield','block_8proofs')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--repetitions',type=int,default=3)
    parser.add_argument('--max-rss-mib',type=int,default=2048)
    parser.add_argument('--max-prove-ms',type=int,default=120000)
    parser.add_argument('--max-verify-ms',type=int,default=30000)
    parser.add_argument('--max-block-verify-ms',type=int,default=20000)
    args=parser.parse_args()
    if args.repetitions < 1:
        parser.error('repetitions must be positive')
    binary=args.binary.resolve()
    args.output.mkdir(parents=True,exist_ok=True)
    report={'scope':'proof-component-only','platform':platform.platform(),
        'machine':platform.machine(),'logical_cpus':os.cpu_count(),
        'source_commit':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),
        'source_dirty':bool(subprocess.check_output(['git','status','--porcelain'],text=True).strip()),
        'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),
        'budgets':{'rss_bytes':args.max_rss_mib*1024**2,'prove_ms':args.max_prove_ms,
                   'verify_ms':args.max_verify_ms,'block_verify_ms':args.max_block_verify_ms},
        'measurements':[],'summary':{},'qualified':False}
    try:
        for shape in SHAPES:
            rows=[]
            for iteration in range(args.repetitions):
                env=dict(os.environ,AUTH_RESOURCE_SHAPE=shape)
                start=time.monotonic()
                run=subprocess.run([str(binary),'--gtest_filter=*AuthResourceMeasurements'],
                    env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=600)
                (args.output/f'{shape}-{iteration+1}.log').write_text(run.stdout)
                if run.returncode:
                    raise RuntimeError(f'{shape}: proof/validation failed (exit {run.returncode})')
                lines=[line for line in run.stdout.splitlines() if line.startswith('AUTH_RESOURCE ')]
                if len(lines)!=1:
                    raise RuntimeError(f'{shape}: expected one measurement, got {len(lines)}')
                values=dict(item.split('=',1) for item in lines[0].split()[1:])
                if values.pop('shape')!=shape:
                    raise RuntimeError('measurement shape mismatch')
                row={key:int(value) for key,value in values.items()}
                row.update(shape=shape,iteration=iteration+1,wall_seconds=time.monotonic()-start)
                rows.append(row);report['measurements'].append(row)
                print(shape,iteration+1,row,flush=True)
            summary={}
            for key in ('prove_ms','verify_ms','process_peak_rss_bytes'):
                ordered=sorted(row[key] for row in rows)
                summary[key]={'p50':ordered[math.ceil(len(rows)*.5)-1],
                              'p95':ordered[math.ceil(len(rows)*.95)-1],'max':ordered[-1]}
            verify_limit=args.max_block_verify_ms if shape=='block_8proofs' else args.max_verify_ms
            summary['qualified']=(summary['process_peak_rss_bytes']['max']>0 and
                summary['process_peak_rss_bytes']['max']<=args.max_rss_mib*1024**2 and
                summary['prove_ms']['max']<=args.max_prove_ms and
                summary['verify_ms']['max']<=verify_limit)
            report['summary'][shape]=summary
        report['qualified']=all(s['qualified'] for s in report['summary'].values())
    finally:
        if hashlib.sha256(binary.read_bytes()).hexdigest()!=report['binary_sha256']:
            report['qualified']=False
            report['error']='binary changed during measurements'
        (args.output/'results.json').write_text(json.dumps(report,indent=2)+'\n')
    return 0 if report['qualified'] else 1

if __name__=='__main__':
    raise SystemExit(main())
