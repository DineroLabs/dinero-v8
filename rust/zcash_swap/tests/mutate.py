#!/usr/bin/env python3
"""Neuter checks for the ZEC transaction code: each mutation breaks one rule in
rust/zcash_swap/src/lib.rs or src/wallet/swap/zec_tx.cpp, rebuilds
test_swap_zec_tx and must make it fail. Sources are restored after each run.

    python3 rust/zcash_swap/tests/mutate.py <build-dir> [name-filter ...]
"""
import os, subprocess, sys
ROOT=os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
if len(sys.argv) < 2: sys.exit(__doc__)
BUILD=os.path.abspath(sys.argv[1])
LIB=ROOT+'/rust/zcash_swap/src/lib.rs'; CPP=ROOT+'/src/wallet/swap/zec_tx.cpp'
M=[
 ('sighash: always input 0', LIB, '        index,\n        &auth.scripts[index],', '        0,\n        &auth.scripts[index],'),
 ('sighash: amounts reversed', LIB, '        amounts.push(Zatoshis::from_u64(p.value_zat).map_err(|_| DZS_ERR_ARG)?);', '        amounts.insert(0, Zatoshis::from_u64(p.value_zat).map_err(|_| DZS_ERR_ARG)?);'),
 ('sighash: script of input 0', LIB, '        &auth.scripts[index],\n        &auth.scripts[index],', '        &auth.scripts[0],\n        &auth.scripts[0],'),
 ('build: branch ignored', LIB, '            TxVersion::V5,\n            branch,', '            TxVersion::V5,\n            BranchId::Nu6_1,'),
 ('build: locktime/expiry swapped', LIB, '            lock_time,\n            BlockHeight::from_u32(expiry_height),', '            expiry_height,\n            BlockHeight::from_u32(lock_time),'),
 ('build: expiry threshold dropped', LIB, '        if expiry_height >= TX_EXPIRY_HEIGHT_THRESHOLD {', '        if false && expiry_height >= TX_EXPIRY_HEIGHT_THRESHOLD {'),
 ('branch: v5 validity dropped', LIB, '    if !TxVersion::V5.valid_in_branch(branch) {', '    if false && !TxVersion::V5.valid_in_branch(branch) {'),
 ('read: trailing bytes allowed', LIB, '    if !cursor.is_empty() || tx.version()', '    if false && !cursor.is_empty() || tx.version()'),
 ('build: empty vout allowed', LIB, '    if vin.is_empty() || vout.is_empty() {', '    if vin.is_empty() {'),
 ('verify: CLTV flag dropped', LIB, '            | Flags::CHECKLOCKTIMEVERIFY\n', '\n'),
 # Without the discourage flag, CLTV becomes a plain NOP2: the refund tests that
 # expect rejection must then fail, proving they test the lock itself.
 ('verify: CLTV is a no-op', LIB, '            | Flags::DiscourageUpgradableNOPs\n            | Flags::CleanStack\n            | Flags::CHECKLOCKTIMEVERIFY\n', '            | Flags::CleanStack\n'),
 ('cpp: refund sequence final', CPP, 'terms.refund_locktime_unix, kLockTimeSequence);', 'terms.refund_locktime_unix, kFinalSequence);'),
 ('cpp: claim selector OP_FALSE', CPP, 'script_sig.push_back(0x51);', 'script_sig.push_back(0x00);'),
 ('cpp: refund selector OP_TRUE', CPP, "script_sig.push_back(0x00);  // OP_FALSE", "script_sig.push_back(0x51);  // OP_FALSE"),
 ('cpp: claim preimage check dropped', CPP, 'if (preimage.size() != 32 || hash != terms.payment_hash) {', 'if (false) {'),
 ('cpp: claim locktime nonzero', CPP, 'expiry_height, 0, kFinalSequence);', 'expiry_height, 1, kFinalSequence);'),
 ('cpp: fee guard dropped', CPP, 'if (fee_zat >= funding.value_zat)', 'if (false)'),
 ('cpp: sighash ignores funding check', CPP, 'if (tx.vin[0].prev_txid != funding.txid || tx.vin[0].prev_vout != funding.vout) {', 'if (false) {'),
 ('cpp: pushdata1 missing', CPP, '    if (data.size() > 75) script.push_back(0x4c);\n', ''),
 ('cpp: p2sh hash is sha256 only', CPP, '    return RIPEMD160(sha.data(), sha.size());', '    std::array<uint8_t, 20> h{}; std::copy(sha.begin(), sha.begin() + 20, h.begin()); return h;'),
 ('cpp: decode ignores kind', CPP, '    out.kind = static_cast<ZecAddressKind>(kind);', '    out.kind = ZecAddressKind::P2pkh;'),
]
only=sys.argv[2:]
res=[]
for name,path,old,new in M:
    if only and not any(o in name for o in only): continue
    src=open(path).read()
    if src.count(old)!=1: res.append((name,'PATTERN')); print(name,'PATTERN NOT UNIQUE',src.count(old)); continue
    open(path,'w').write(src.replace(old,new))
    try:
        b=subprocess.run(['nice','-n','15','cmake','--build',BUILD,'--target','test_swap_zec_tx','-j4'],capture_output=True,text=True)
        if b.returncode!=0: st='BUILD-FAIL'
        else:
            t=subprocess.run([BUILD+'/test_swap_zec_tx'],capture_output=True,text=True)
            fails=[l.split()[3] for l in t.stdout.splitlines() if l.startswith('[  FAILED  ] SwapZecTx.')]
            st='CAUGHT '+','.join(sorted(set(fails))) if t.returncode!=0 else 'SURVIVED'
    finally:
        open(path,'w').write(src)
    res.append((name,st)); print(f'{name:40s} {st}', flush=True)
subprocess.run(['nice','-n','15','cmake','--build',BUILD,'--target','test_swap_zec_tx','-j4'],capture_output=True)
survivors=[n for n,s in res if not s.startswith('CAUGHT')]
print(f'{len(res)-len(survivors)}/{len(res)} caught; survivors: {survivors}')
sys.exit(1 if survivors else 0)
