#include "wallet/swap/zec_chain_io.h"

#include "wallet/swap/swap_crypto.h"
#include "wallet/swap/tower.h"

#include <algorithm>
#include <ctime>
#include <stdexcept>

namespace dinero::swap {
namespace {

using detail::FromHex;
using detail::ToHex;

std::string DisplayTxid(const Bytes32& wire) { return ToHex(std::vector<uint8_t>(wire.rbegin(), wire.rend())); }

[[noreturn]] void BadRung(const std::string& why) { throw std::invalid_argument("tower package refused: " + why); }

ZecTx UnsignedRung(const TowerPackage& p, const ZecRefundRung& r) {
    const ZecFunding f{p.zec_funding->txid, p.zec_funding->vout, p.zec_funding->value_sat};
    return BuildZecRefundTx(MakeBtcTerms(p.offer, p.accept), f, r.payout_script, r.fee_zat, r.branch_id, 0);
}

// Every rung refunds the package's ZEC lock to one payout, at rising fees, and
// carries Bob's valid signature.
void VerifyZecRungs(const TowerPackage& p) {
    const auto terms = MakeBtcTerms(p.offer, p.accept);
    const ZecFunding f{p.zec_funding->txid, p.zec_funding->vout, p.zec_funding->value_sat};
    uint64_t last_fee = 0;
    for (size_t i = 0; i < p.zec_refunds.size(); ++i) {
        const auto& r = p.zec_refunds[i];
        const std::string at = "ZEC refund rung " + std::to_string(i) + ": ";
        if (r.payout_script.empty() || r.payout_script != p.zec_refunds.front().payout_script) {
            BadRung(at + "pays a different destination");
        }
        if (r.fee_zat == 0 || r.fee_zat >= p.offer.btc_amount_sat) BadRung(at + "fee does not fit the amount");
        if (i > 0 && r.fee_zat <= last_fee) BadRung(at + "fees must rise");
        last_fee = r.fee_zat;
        ZecTx tx;
        try {
            tx = UnsignedRung(p, r);
        } catch (const std::exception& e) {
            BadRung(at + e.what());
        }
        if (!detail::EcdsaVerifyAll(terms.refund_pubkey, ZecHtlcSighash(tx, terms, f), r.signature)) {
            BadRung(at + "signature does not verify");
        }
    }
}

std::vector<uint8_t> ZecRungRaw(const TowerPackage& p, size_t i) {
    const auto& r = p.zec_refunds.at(i);
    auto tx = UnsignedRung(p, r);
    SetZecRefundScriptSig(tx, MakeBtcTerms(p.offer, p.accept), r.signature);
    return SerializeZecTx(tx);
}

}  // namespace

ZecNetwork ZecNetworkFor(SwapNetwork network) {
    switch (network) {
    case SwapNetwork::Mainnet: return ZecNetwork::Main;
    case SwapNetwork::Testnet: return ZecNetwork::Test;
    case SwapNetwork::Regtest: return ZecNetwork::Regtest;
    }
    throw std::invalid_argument("unknown swap network");
}

std::string ZecGenesisFor(SwapNetwork network) {
    switch (network) {
    case SwapNetwork::Mainnet: return "00040fe8ec8471911baa1db1266ea15dd06b4a8a5c453883c000b031973dce08";
    case SwapNetwork::Testnet: return "05a60a92d99d85997cce3b87616c089f6124d7342af37106edc76126334a2c38";
    case SwapNetwork::Regtest: return "029f11d80ef9765602235e1bc9727e3eb6ba20839319f761fee920d63401e327";
    }
    throw std::invalid_argument("unknown swap network");
}

ZecRpc GuardZecGenesis(ZecRpc rpc, std::string genesis_hash) {
    return [rpc = std::move(rpc), genesis_hash = std::move(genesis_hash)](
               const std::string& method, const Json::Value& params) -> std::optional<Json::Value> {
        if (method == "getblockchaininfo") {
            Json::Value zero(Json::arrayValue);
            zero.append(0);
            const auto genesis = rpc("getblockhash", zero);
            if (!genesis || !genesis->isString() || genesis->asString() != genesis_hash) return std::nullopt;
        }
        return rpc(method, params);
    };
}

ZecSwapChainIo::ZecSwapChainIo(DinRpc din, ZecRpc zec, SwapWatchSpec spec, ZecIoConfig config)
    : RpcSwapChainIo(std::move(din), [](const std::string&, const Json::Value&) { return std::optional<Json::Value>{}; },
                     spec),
      zec_(std::move(zec)), zspec_(std::move(spec)), config_(std::move(config)) {
    if (zspec_.offer.chain != CounterChain::Zcash) throw std::invalid_argument("not a Zcash swap");
    InstallZecTowerHooks({VerifyZecRungs, ZecRungRaw});  // the tower (and Bob's runner) can now check Zcash rungs
    if (!config_.clock) config_.clock = [] { return static_cast<uint32_t>(std::time(nullptr)); };
    if (!config_.broadcast) {
        config_.broadcast = [this](const std::string& hex) {
            Json::Value p(Json::arrayValue);
            p.append(hex);
            const auto r = zec_("sendrawtransaction", p);
            if (!r || !r->isString()) throw std::runtime_error("sendrawtransaction (ZEC) failed");
            return r->asString();
        };
    }
    RebuildZecWatcher();
}

void ZecSwapChainIo::RebuildZecWatcher() {
    ZecWatchTarget t;
    t.terms = MakeBtcTerms(zspec_.offer, zspec_.accept);
    t.scan_from_height = zspec_.btc_scan_from_height;
    t.expected_amount_zat = zspec_.offer.btc_amount_sat;
    t.expected_funding_txid = zspec_.btc_funding_txid;
    t.expected_chain = config_.expected_chain;
    watcher_ = std::make_unique<ZecWatcher>(zec_, t);
}

void ZecSwapChainIo::PinFunding(const std::string& din_txid, const std::string& zec_txid) {
    RpcSwapChainIo::PinFunding(din_txid, "");
    if (!din_txid.empty()) zspec_.din_funding_txid = din_txid;
    if (!zec_txid.empty() && zec_txid != zspec_.btc_funding_txid) {
        zspec_.btc_funding_txid = zec_txid;
        RebuildZecWatcher();
    }
}

BtcWatchReport ZecSwapChainIo::ObserveBtc() {
    const auto z = watcher_->Observe();
    BtcWatchReport r;
    r.ok = z.ok;
    if (!z.ok) return r;
    chain_ = z.chain;
    r.htlc = z.htlc;
    r.mtp_unix = z.chain.mtp_unix;
    if (z.funding) r.funding = BtcFunding{z.funding->txid, z.funding->vout, z.funding->value_zat};
    return r;
}

ZecChainInfo ZecSwapChainIo::ChainNow() {
    const auto c = ObserveZecChain(zec_, config_.expected_chain);
    if (!c) throw std::runtime_error("Zebra unreachable or on another network");
    chain_ = *c;
    return *c;
}

std::string ZecSwapChainIo::FundBtc(const std::string&, uint64_t) {
    throw std::runtime_error("a Zcash lock is funded from the deposit (prepared), never sent from a wallet");
}

PreparedBtcFunding ZecSwapChainIo::PrepareFundBtc(const std::string& address, uint64_t amount_zat) {
    const SwapRecord probe{Role::BtcSeller, zspec_.offer, zspec_.accept};
    if (address != CounterHtlcAddress(probe, RunnerConfig{})) throw std::runtime_error("not this swap's ZEC HTLC address");
    if (amount_zat != zspec_.offer.btc_amount_sat) throw std::runtime_error("not this swap's ZEC amount");
    if (config_.deposit_secret_key == Bytes32{} || config_.change_script.empty()) {
        throw std::runtime_error("no deposit key or change script configured");
    }
    const auto chain = ChainNow();
    // A pre-signed ZEC transaction binds the branch id: refuse a window that
    // reaches an upgrade (the tower rung would be void after it).
    const uint64_t until = uint64_t(zspec_.offer.t_btc_unix) + config_.upgrade_margin_seconds;
    if (const auto u = ZecUpgradeBefore(chain, config_.clock(), static_cast<uint32_t>(std::min<uint64_t>(until, UINT32_MAX)))) {
        throw std::runtime_error("Zcash network upgrade " + u->name + " at height " +
                                 std::to_string(u->activation_height) + " falls inside this swap; not locking ZEC");
    }
    const auto deposit_addr = EncodeZecTAddress(
        ZecP2pkhAddressOf(detail::CompressedOf(config_.deposit_secret_key), config_.network));
    const auto deposit = ObserveZecDeposit(zec_, deposit_addr, config_.expected_chain);
    if (!deposit.ok) throw std::runtime_error("cannot read the deposit");
    const auto coins = deposit.spendable(config_.deposit_confirmations);
    const auto htlc = ZecP2shScriptPubKey(BuildZecHtlcRedeemScript(MakeBtcTerms(zspec_.offer, zspec_.accept)));
    // Expiry 0: a lock prepared while the tower arms must not die waiting.
    auto plan = BuildZecFundingTx(coins, htlc, amount_zat, config_.change_script, config_.funding_fee_multiplier,
                                  chain.next_block_branch_id, 0);
    SignZecP2pkhInputs(plan.tx, coins, config_.deposit_secret_key);
    PreparedBtcFunding prep;
    prep.raw = SerializeZecTx(plan.tx);
    prep.txid = DisplayTxid(ZecTxid(prep.raw));
    prep.vout = 0;  // BuildZecFundingTx puts the HTLC first
    return prep;
}

void ZecSwapChainIo::ReleasePreparedFunding(const std::vector<uint8_t>&) {}  // no wallet holds the coins

std::optional<bool> ZecSwapChainIo::PreparedFundingUnsent(const std::vector<uint8_t>& raw) {
    // Sent means Zebra knows the transaction (mempool or chain). Unsent needs
    // both: Zebra reachable and not knowing it, and the deposit still holding
    // coins (getaddressutxos ignores the mempool, which getrawtransaction covers).
    const auto c = ObserveZecChain(zec_, config_.expected_chain);
    if (!c) return std::nullopt;
    Json::Value p(Json::arrayValue);
    p.append(DisplayTxid(ZecTxid(raw)));
    p.append(0);
    if (zec_("getrawtransaction", p)) return false;
    if (config_.deposit_secret_key == Bytes32{}) return std::nullopt;
    const auto deposit = ObserveZecDeposit(
        zec_, EncodeZecTAddress(ZecP2pkhAddressOf(detail::CompressedOf(config_.deposit_secret_key), config_.network)),
        config_.expected_chain);
    if (!deposit.ok) return std::nullopt;
    if (deposit.coins.empty()) return false;  // spent by something: it may have gone out
    return true;
}

std::string ZecSwapChainIo::BroadcastBtc(const std::vector<uint8_t>& raw_tx) { return config_.broadcast(ToHex(raw_tx)); }

CounterChainRules ZecSwapChainIo::CounterRules() const {
    CounterChainRules r;
    r.exclusive_windows = false;          // after T_zec claim and refund race, as on Bitcoin
    r.claim_cutoff_seconds = 6 * 60 * 60;
    r.early_refund_seconds = 90 * 60;     // a block's own time may run 90 min past the median
    return r;
}

std::string ZecSwapChainIo::CounterHtlcAddress(const SwapRecord& record, const RunnerConfig&) const {
    const auto spk = ZecP2shScriptPubKey(BuildZecHtlcRedeemScript(MakeBtcTerms(record.offer, record.accept)));
    ZecTAddress a{ZecNetworkFor(record.offer.network), ZecAddressKind::P2sh, {}};
    std::copy(spk.begin() + 2, spk.begin() + 22, a.hash.begin());
    return EncodeZecTAddress(a);
}

uint64_t ZecSwapChainIo::CounterSpendFee(const SwapSession&, const RunnerConfig&, uint32_t) const {
    return config_.spend_fee_zat;  // no RBF: a fixed fee, re-signed when it expires
}

ZecTx ZecSwapChainIo::Spend(const SwapSession& s, const SwapKeys& keys, const BtcFunding& funding, uint64_t fee,
                            bool claim) {
    const auto chain = chain_ ? *chain_ : ChainNow();
    const auto terms = MakeBtcTerms(s.record.offer, s.record.accept);
    const ZecFunding f{funding.txid, funding.vout, funding.value_sat};
    const uint32_t expiry = chain.tip_height + config_.spend_expiry_blocks;
    ZecTx tx = claim ? BuildZecClaimTx(f, s.btc_payout_script, fee, chain.next_block_branch_id, expiry)
                     : BuildZecRefundTx(terms, f, s.btc_payout_script, fee, chain.next_block_branch_id, expiry);
    const auto sig = detail::EcdsaSignAll(keys.btc_secret_key, ZecHtlcSighash(tx, terms, f));
    if (claim) {
        if (!s.record.secret) throw std::invalid_argument("the secret is not known");
        SetZecClaimScriptSig(tx, terms, sig, std::vector<uint8_t>(s.record.secret->begin(), s.record.secret->end()));
    } else {
        SetZecRefundScriptSig(tx, terms, sig);
    }
    return tx;
}

std::vector<uint8_t> ZecSwapChainIo::SignCounterClaim(const SwapSession& s, const SwapKeys& keys,
                                                      const BtcFunding& funding, uint64_t fee) {
    if (s.record.role != Role::DinSeller || detail::CompressedOf(keys.btc_secret_key) != s.record.offer.btc_claim_pubkey) {
        throw std::invalid_argument("only Alice's ZEC key claims");
    }
    return SerializeZecTx(Spend(s, keys, funding, fee, true));
}

std::vector<uint8_t> ZecSwapChainIo::SignCounterRefund(const SwapSession& s, const SwapKeys& keys,
                                                       const BtcFunding& funding, uint64_t fee) {
    if (s.record.role != Role::BtcSeller || detail::CompressedOf(keys.btc_secret_key) != s.record.accept.btc_refund_pubkey) {
        throw std::invalid_argument("only Bob's ZEC key refunds");
    }
    return SerializeZecTx(Spend(s, keys, funding, fee, false));
}

bool ZecSwapChainIo::CounterBroadcastMayHaveLeft(const std::string& error) const {
    // Only Zebra's verdicts on this transaction itself prove it never left.
    // Everything else counts as possibly public: a lost reply, "already
    // exists in mempool", and a spend conflict (which may be Alice's own
    // earlier claim, signed with an older expiry).
    for (const char* refused : {"did not pass consensus validation", "did not pass standard validation",
                                "transaction is non-standard", "reached transaction expiry height"}) {
        if (error.find(refused) != std::string::npos) return false;
    }
    return true;
}

// One rung: Zcash has no RBF, so more rungs would only conflict. 4 x the
// conventional fee is ZIP 317's weight cap, so it ranks first. nExpiryHeight
// 0 keeps it valid until it is needed; it binds the branch id it is signed
// for, which the upgrade guard keeps stable for the swap's window.
void ZecSwapChainIo::AddCounterTowerRungs(TowerPackage& package, const SwapSession& s, const SwapKeys& keys,
                                          const BtcFunding& funding, const BtcFeeLadderPolicy& policy) const {
    if (!chain_) throw std::runtime_error("Zebra not observed yet: cannot sign the refund rung");
    const auto terms = MakeBtcTerms(s.record.offer, s.record.accept);
    if (detail::CompressedOf(keys.btc_secret_key) != terms.refund_pubkey) {
        throw std::invalid_argument("ZEC key does not match the swap");
    }
    if (config_.rung_fee_zat >= funding.value_sat || funding.value_sat - config_.rung_fee_zat < policy.min_payout_sat) {
        throw std::invalid_argument("the ZEC refund rung fee does not fit the amount");
    }
    ZecRefundRung r;
    r.fee_zat = config_.rung_fee_zat;
    r.branch_id = chain_->next_block_branch_id;
    r.payout_script = s.btc_payout_script;
    package.zec_funding = funding;
    const auto tx = UnsignedRung(package, r);
    r.signature = detail::EcdsaSignAll(keys.btc_secret_key,
                                       ZecHtlcSighash(tx, terms, {funding.txid, funding.vout, funding.value_sat}));
    package.zec_refunds = {r};
}

}  // namespace dinero::swap
