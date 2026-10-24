"""Reviewed source defaults, never packaging authority or runtime attestation.

Pin the reviewed source bytes rather than silently guessing new schedules after
an edit. A future chainparams change requires explicit metadata/vector review.
Composed SDKs inherit their input receipts, never the packager's current HEAD.
"""
import hashlib
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
REVIEWED = {
    'src/consensus/chainparams_impl.cpp': '61d0a18d675c9ffb8eb39e637170b976ef79a18feed26ad323adcacc310fd9b4',
    'include/consensus/chainparams.h': 'ab60a2f1a4fa7e0e8b6aed9f5da75ea963a14d99666b383ab278c0572ac6eb4b',
}


def reviewed_defaults(root=ROOT):
    actual = {name: hashlib.sha256((root/name).read_bytes()).hexdigest() for name in REVIEWED}
    if actual != REVIEWED:
        raise ValueError('unreviewed chainparams source: review SDK consensus declarations before packaging')
    return dict(schema=1, authority='compiled ChainParams; packaging cannot activate consensus',
                evidence='reviewed source defaults, NOT runtime attestation', source_sha256=actual,
                dnorst02_activation_height=dict(mainnet=150000, testnet=4294967295, regtest=4294967295),
                never_activation_height=4294967295)


def source_receipt(root=ROOT):
    return dict(source_base=subprocess.check_output(['git','-C',str(root),'rev-parse','HEAD'],text=True).strip(),
                source_dirty=bool(subprocess.check_output(['git','-C',str(root),'status','--porcelain',
                                                          '--ignore-submodules=all'],text=True).strip()),
                ffi_header_sha256=hashlib.sha256((root/'include/nodecore/nodecore_ffi.h').read_bytes()).hexdigest(),
                source_consensus_declaration=reviewed_defaults(root))


def build_source_receipt(build):
    cache=(build/'CMakeCache.txt').read_text()
    matches=re.findall(r'^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$',cache,re.M)
    if len(matches)!=1:
        raise ValueError('missing or ambiguous SDK build source directory')
    return source_receipt(Path(matches[0]).resolve())


def compose_receipts(receipts,root=ROOT):
    expected=reviewed_defaults(root)
    header=hashlib.sha256((root/'include/nodecore/nodecore_ffi.h').read_bytes()).hexdigest()
    if not receipts:
        raise ValueError('no SDK source receipts')
    bases=set()
    for receipt in receipts:
        if receipt.get('source_consensus_declaration')!=expected:
            raise ValueError('missing or unreviewed input consensus declaration; rebuild SDK, do not relabel it')
        base=receipt.get('source_base','')
        if not isinstance(base,str) or not re.fullmatch('[0-9a-f]{40}',base) or receipt.get('source_dirty') is not False:
            raise ValueError('SDK inputs require clean, identified source receipts')
        if receipt.get('ffi_header_sha256')!=header:
            raise ValueError('SDK input/header identity mismatch')
        bases.add(base)
    if len(bases)!=1:
        raise ValueError('SDK slices have different source identities')
    return dict(source_base=bases.pop(),source_dirty=False,ffi_header_sha256=header,
                source_consensus_declaration=expected)
