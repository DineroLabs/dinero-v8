#!/usr/bin/env python3
"""Neuter checks for the ZEC swap code: each mutation breaks one rule in
rust/zcash_swap/src/lib.rs or src/wallet/swap/zec_{tx,watcher}.{h,cpp},
rebuilds, and must make a test fail: test_swap_zec_tx, or for the rules only a
real node exercises (marked R), tests/wallet/swap_zec_regtest.sh. Sources are
restored after each run.

    python3 rust/zcash_swap/tests/mutate.py <build-dir> [--zebrad PATH] [name-filter ...]

Without --zebrad the R mutations are skipped (and reported as skipped).
"""
import os, subprocess, sys
ROOT=os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
if len(sys.argv) < 2: sys.exit(__doc__)
BUILD=os.path.abspath(sys.argv[1])
args=sys.argv[2:]
ZEBRAD=None
if args[:1]==['--zebrad']: ZEBRAD=args[1]; args=args[2:]
LIB=ROOT+'/rust/zcash_swap/src/lib.rs'; CPP=ROOT+'/src/wallet/swap/zec_tx.cpp'
WCPP=ROOT+'/src/wallet/swap/zec_watcher.cpp'; WHDR=ROOT+'/include/wallet/swap/zec_watcher.h'
ZIO=ROOT+'/src/wallet/swap/zec_chain_io.cpp'; RUN=ROOT+'/src/wallet/swap/runner.cpp'; TOW=ROOT+'/src/wallet/swap/tower.cpp'
ZHDR=ROOT+'/include/wallet/swap/zec_chain_io.h'
R='regtest'; IO='io'
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
 # milestone 4: deposit, fees, extraction, guard, chain reading, watcher
 ('cpp: extract ignores the hash', CPP, '            if (hash == payment_hash) return item;', '            return item;'),
 ('cpp: extract accepts non-push ops', CPP, '            return std::nullopt;  // not push-only', '            continue;'),
 ('cpp: zip317 floor of two dropped', CPP, 'std::max<size_t>({2, (tx_in_total_size', 'std::max<size_t>({0, (tx_in_total_size'),
 ('cpp: zip317 output divisor 150', CPP, '(tx_out_total_size + 33) / 34', '(tx_out_total_size + 149) / 150'),
 ('cpp: funding short check dropped', CPP, '    if (total < htlc_amount_zat || total - htlc_amount_zat < plan.fee_zat) {', '    if (false) {'),
 ('cpp: dust change kept', CPP, '    if (change < kZip317MarginalFee) {', '    if (false) {'),
 ('cpp: sweep pays the fee twice', CPP, '    const uint64_t fee = SignedFee(tx, 1);', '    const uint64_t fee = SignedFee(tx, 2);'),
 ('cpp: p2pkh scriptSig without pubkey', CPP, '        PushData(script_sig, {pubkey.begin(), pubkey.end()});\n', ''),
 ('guard: no 10% margin', WHDR, 'constexpr double kZecProjectedSecondsPerBlock = 75.0 / 1.10;', 'constexpr double kZecProjectedSecondsPerBlock = 75.0;'),
 ('guard: active upgrades count', WCPP, 'if (u.activation_height <= chain.tip_height || u.activation_height > projected) continue;', 'if (u.activation_height > projected) continue;'),
 ('guard: last instead of first', WCPP, 'if (!first || u.activation_height < first->activation_height) first = u;', 'first = u;'),
 ('chain: tip time as mtp', WCPP, '    chain.mtp_unix = times[times.size() / 2];', '    chain.mtp_unix = chain.tip_time_unix;'),
 ('chain: network not checked', WCPP, '    if (!expected_chain.empty() && (*info)["chain"].asString() != expected_chain) return std::nullopt;', ''),
 ('deposit: depth ignored', WCPP, '        if (c.confirmations >= min_confirmations) total += c.coin.value_zat;', '        total += c.coin.value_zat;'),
 ('deposit: address not checked', WCPP, ' || u["address"].asString() != address) {', ') {'),
 ('deposit: height above tip allowed', WCPP, '        if (height > report.tip_height) return report;\n', ''),
 ('watch: no mempool scan', WCPP, '        } else if (!FindMempoolSpend(report.htlc)) {', '        } else if (false) {', R),
 ('watch: reorg keeps the funding', WCPP, '        if (*gone) {\n            funding_.reset();', '        if (false) {\n            funding_.reset();', R),
 ('watch: claim read as refund', WCPP, '    obs.spent_by_claim = obs.revealed_preimage.has_value();', '    obs.spent_by_claim = false;', R),
 # milestone 5: the Zcash counter chain behind the runner
 ('runner: Bitcoin rules for every chain', RUN, '    const StepResult step = Step(session_.record, obs, io_.CounterRules());', '    const StepResult step = Step(session_.record, obs);', IO),
 ('io: no early-refund allowance', ZIO, '    r.early_refund_seconds = 90 * 60;', '    r.early_refund_seconds = 0;', IO),
 ('io: upgrade guard skipped', ZIO, '    if (const auto u = ZecUpgradeBefore(chain, config_.clock(),', '    if (const auto u = std::optional<ZecUpgrade>{}; false && ZecUpgradeBefore(chain, config_.clock(),', IO),
 ('io: lock prepared with an expiry', ZIO, 'config_.funding_fee_multiplier,\n                                  chain.next_block_branch_id, 0);', 'config_.funding_fee_multiplier,\n                                  chain.next_block_branch_id, chain.tip_height + 40);', IO),
 ('io: shallow deposit coins spent', ZIO, '    const auto coins = deposit.spendable(config_.deposit_confirmations);', '    const auto coins = deposit.spendable(0);', IO),
 ('io: address not checked', ZIO, '    if (address != CounterHtlcAddress(probe, RunnerConfig{})) throw', '    if (false) throw', IO),
 ('io: consensus refusal read as maybe-left', ZIO, '    for (const char* refused : {"did not pass consensus validation", ', '    for (const char* refused : {', IO),
 ('io: spend conflict read as refused', ZIO, '"transaction is non-standard", "reached transaction expiry height"}) {', '"transaction is non-standard", "reached transaction expiry height", "already spent"}) {', IO),
 ('io: spend expiry dropped', ZIO, '    const uint32_t expiry = chain.tip_height + config_.spend_expiry_blocks;', '    const uint32_t expiry = 0;', IO),
 ('io: fixed branch id', ZIO, 'fee, chain.next_block_branch_id, expiry)\n                     : BuildZecRefundTx', 'fee, 0x4dec4df0, expiry)\n                     : BuildZecRefundTx', IO),
 ('io: unsent despite spent coins', ZIO, '    if (deposit.coins.empty()) return false;  // spent by something: it may have gone out\n', '', IO),
 ('io: claim role not checked', ZIO, '    if (s.record.role != Role::DinSeller || detail::CompressedOf(keys.btc_secret_key) != s.record.offer.btc_claim_pubkey) {', '    if (false) {', IO),
 ('tower: zec rung signature not checked', ZIO, '        if (!detail::EcdsaVerifyAll(terms.refund_pubkey, ZecHtlcSighash(tx, terms, f), r.signature)) {', '        if (false) {', IO),
 ('tower: zec rung payout not checked', ZIO, '        if (r.payout_script.empty() || r.payout_script != p.zec_refunds.front().payout_script) {', '        if (false) {', IO),
 ('tower: zec rung expires', ZIO, 'f, r.payout_script, r.fee_zat, r.branch_id, 0);', 'f, r.payout_script, r.fee_zat, r.branch_id, 500);', IO),
 ('tower: zec package without hooks accepted', TOW, '        if (!ZecHooks().verify) Bad("this build cannot verify Zcash refund rungs");', '        if (!ZecHooks().verify) return;', IO),
 ('tower: zec rungs allowed in btc package', TOW, '        if (p.zec_funding || !p.zec_refunds.empty()) Bad("Zcash rungs in a Bitcoin package");', '', IO),
 ('tower: zec rung fee 2x', ZHDR, '    uint64_t rung_fee_zat{4 * 10\'000};', '    uint64_t rung_fee_zat{2 * 10\'000};', IO),
 ('watch: amount filter dropped', WCPP, '                    if (target_.expected_amount_zat && zat != target_.expected_amount_zat) continue;\n', '', R),
]
only=args
res=[]
for name,path,old,new,*kind in M:
    if only and not any(o in name for o in only): continue
    regtest=kind==[R]
    io=kind==[IO]
    if regtest and not ZEBRAD: res.append((name,'SKIPPED')); print(f'{name:40s} SKIPPED (no --zebrad)'); continue
    src=open(path).read()
    if src.count(old)!=1: res.append((name,'PATTERN')); print(name,'PATTERN NOT UNIQUE',src.count(old)); continue
    open(path,'w').write(src.replace(old,new))
    try:
        target='swap_zec_regtest_tool' if regtest else 'test_swap_zec_chain_io' if io else 'test_swap_zec_tx'
        b=subprocess.run(['nice','-n','15','cmake','--build',BUILD,'--target',target,'-j4'],capture_output=True,text=True)
        if b.returncode!=0: st='BUILD-FAIL'
        elif regtest:
            t=subprocess.run([ROOT+'/tests/wallet/swap_zec_regtest.sh',BUILD+'/swap_zec_regtest_tool',ZEBRAD],capture_output=True,text=True)
            fails=[l.split(' FAIL ')[0].strip() for l in t.stdout.splitlines() if ' FAIL ' in l]
            st='CAUGHT '+' | '.join(fails) if t.returncode!=0 else 'SURVIVED'
        else:
            t=subprocess.run([BUILD+'/'+target],capture_output=True,text=True)
            fails=[l.split()[3] for l in t.stdout.splitlines() if l.startswith('[  FAILED  ] SwapZec')]
            st='CAUGHT '+','.join(sorted(set(fails))) if t.returncode!=0 else 'SURVIVED'
    finally:
        open(path,'w').write(src)
    res.append((name,st)); print(f'{name:40s} {st}', flush=True)
subprocess.run(['nice','-n','15','cmake','--build',BUILD,'--target','test_swap_zec_tx','test_swap_zec_chain_io','swap_zec_regtest_tool','-j4'],capture_output=True)
survivors=[n for n,s in res if not s.startswith('CAUGHT') and s!='SKIPPED']
skipped=[n for n,s in res if s=='SKIPPED']
if skipped: print(f'skipped (need --zebrad): {skipped}')
print(f'{len(res)-len(survivors)}/{len(res)} caught; survivors: {survivors}')
sys.exit(1 if survivors else 0)
