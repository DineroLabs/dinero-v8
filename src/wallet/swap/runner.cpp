#include "wallet/swap/runner.h"

#include "bech32/bech32.hpp"
#include "crypto/evp_secp256k1.h"
#include "crypto/sha256.h"
#include "wallet/swap/btc_tx.h"
#include "wallet/swap/swap_crypto.h"
#include "wallet/swap/tower.h"

#include <algorithm>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace dinero::swap {
namespace {

[[noreturn]] void Refuse(const std::string& why) {
    throw std::invalid_argument("swap runner refused: " + why);
}

using namespace detail;

std::vector<uint8_t> SecretOf(const SwapSession& s) {
    if (!s.record.secret) Refuse("the secret is not known");
    const auto& secret = *s.record.secret;
    Bytes32 h{};
    crypto::CSHA256().Write(secret.data(), secret.size()).Finalize(h.data());
    if (h != s.record.offer.payment_hash) Refuse("the secret does not match the payment hash");
    return {secret.begin(), secret.end()};
}

void RequireDinKey(const SwapKeys& keys, const Bytes32& expected) {
    if (XOnlyOf(keys.din_secret_key) != expected) Refuse("DIN key does not match the swap");
}

void RequireBtcKey(const SwapKeys& keys, const std::array<uint8_t, 33>& expected) {
    if (CompressedOf(keys.btc_secret_key) != expected) Refuse("BTC key does not match the swap");
}

Json::Value Params(std::initializer_list<Json::Value> items) {
    Json::Value p(Json::arrayValue);
    for (const auto& i : items) p.append(i);
    return p;
}

// Node results may also report a failure in-band as {"error": ...}; dinerod's
// sendrawtransaction nests the txid as {"result": "<txid>"}.
std::string TxidFrom(const std::optional<Json::Value>& r, const std::string& what) {
    if (!r) throw std::runtime_error(what + ": RPC failed");
    if (r->isString()) return r->asString();
    if (r->isObject() && r->isMember("error") && !(*r)["error"].isNull()) {
        throw std::runtime_error(what + ": " + (*r)["error"].toStyledString());
    }
    if (r->isObject() && (*r)["txid"].isString()) return (*r)["txid"].asString();
    if (r->isObject() && (*r)["result"].isString()) return (*r)["result"].asString();
    throw std::runtime_error(what + ": unexpected result " + r->toStyledString());
}

}  // namespace

// ---- Session persistence ---------------------------------------------------

std::string EncodeSession(const SwapSession& s) {
    std::ostringstream out;
    out << "record=" << EncodeRecord(s.record) << "\n"
        << "btc_scan_from_height=" << s.btc_scan_from_height << "\n"
        << "din_scan_from_height=" << s.din_scan_from_height << "\n"
        << "din_funding_txid=" << s.din_funding_txid << "\n"
        << "btc_funding_txid=" << s.btc_funding_txid << "\n"
        << "din_payout_script=" << ToHex(s.din_payout_script) << "\n"
        << "btc_payout_script=" << ToHex(s.btc_payout_script) << "\n"
        << "tower_armed=" << (s.tower_armed ? 1 : 0) << "\n";
    return out.str();
}

SwapSession DecodeSession(const std::string& text) {
    std::map<std::string, std::string> kv;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        if (line.empty()) continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) Refuse("malformed session line");
        if (!kv.emplace(line.substr(0, eq), line.substr(eq + 1)).second) Refuse("duplicate session key");
    }
    for (const char* k : {"record", "btc_scan_from_height", "din_payout_script", "btc_payout_script"}) {
        if (!kv.count(k)) Refuse(std::string("session is missing ") + k);
    }
    SwapSession s;
    s.record = DecodeRecord(kv["record"]);
    try {
        size_t used = 0;
        const unsigned long h = std::stoul(kv["btc_scan_from_height"], &used);
        if (used != kv["btc_scan_from_height"].size() || h > UINT32_MAX) Refuse("bad scan height");
        s.btc_scan_from_height = static_cast<uint32_t>(h);
    } catch (const std::logic_error&) {
        Refuse("bad scan height");
    }
    s.din_payout_script = FromHex(kv["din_payout_script"]);
    s.btc_payout_script = FromHex(kv["btc_payout_script"]);
    auto txid_field = [&](const char* key) -> std::string {
        if (!kv.count(key) || kv[key].empty()) return {};
        if (kv[key].size() != 64 || kv[key].find_first_not_of("0123456789abcdef") != std::string::npos) {
            Refuse(std::string("bad ") + key);
        }
        return kv[key];
    };
    s.din_funding_txid = txid_field("din_funding_txid");
    s.btc_funding_txid = txid_field("btc_funding_txid");
    if (kv.count("din_scan_from_height")) {
        const std::string& v = kv["din_scan_from_height"];
        if (v.empty() || v.size() > 10 || v.find_first_not_of("0123456789") != std::string::npos) Refuse("bad DIN scan height");
        const unsigned long long h = std::stoull(v);
        if (h > UINT32_MAX) Refuse("bad DIN scan height");
        s.din_scan_from_height = static_cast<uint32_t>(h);
    }
    if (kv.count("tower_armed")) {  // absent in sessions saved before the tower existed
        if (kv["tower_armed"] != "0" && kv["tower_armed"] != "1") Refuse("bad tower_armed");
        s.tower_armed = kv["tower_armed"] == "1";
    }
    if (s.din_payout_script.empty() || s.btc_payout_script.empty()) Refuse("empty payout script");
    return s;
}

PlaintextFileSwapStore::PlaintextFileSwapStore(std::string path) : path_(std::move(path)) {}

void PlaintextFileSwapStore::Save(const SwapSession& session) {
    const std::string text = EncodeSession(session);
    const std::string tmp = path_ + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) throw std::runtime_error("swap store: cannot open " + tmp);
    size_t off = 0;
    while (off < text.size()) {
        const ssize_t n = ::write(fd, text.data() + off, text.size() - off);
        if (n <= 0) { ::close(fd); throw std::runtime_error("swap store: write failed"); }
        off += static_cast<size_t>(n);
    }
    if (::fsync(fd) != 0) { ::close(fd); throw std::runtime_error("swap store: fsync failed"); }
    ::close(fd);
    if (std::rename(tmp.c_str(), path_.c_str()) != 0) throw std::runtime_error("swap store: rename failed");
}

SwapSession PlaintextFileSwapStore::Load(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("swap store: cannot read " + path);
    std::stringstream buf;
    buf << in.rdbuf();
    return DecodeSession(buf.str());
}

// ---- Addresses and signed spends ------------------------------------------

std::string DinHtlcAddressFor(const SwapRecord& record, const std::string& hrp) {
    return DinHtlcAddress(BuildDinHtlc(MakeDinTerms(record.offer, record.accept)), hrp);
}

std::string BtcHtlcAddressFor(const SwapRecord& record, const std::string& hrp) {
    const auto script = BuildBtcHtlcWitnessScript(MakeBtcTerms(record.offer, record.accept));
    std::vector<uint8_t> program(32);
    crypto::CSHA256().Write(script.data(), script.size()).Finalize(program.data());
    return bech32::Encode(hrp, 0, program, bech32::Encoding::BECH32);
}

std::vector<uint8_t> SignedDinClaim(const SwapSession& s, const SwapKeys& keys,
                                    const FundingOutput& funding, uint64_t fee_una) {
    const auto terms = MakeDinTerms(s.record.offer, s.record.accept);
    RequireDinKey(keys, terms.claim_pubkey);
    const auto htlc = BuildDinHtlc(terms);
    Transaction tx = BuildDinClaimTx(htlc, funding, Payout{s.din_payout_script, AmountUna::Una(fee_una)});
    SetDinClaimWitness(tx, terms, htlc, SchnorrSign(keys.din_secret_key, DinClaimSighash(tx, funding, htlc)),
                       SecretOf(s));
    return tx.Serialize(TxSerializationMode::WithWitness);
}

std::vector<uint8_t> SignedDinRefund(const SwapSession& s, const SwapKeys& keys,
                                     const FundingOutput& funding, uint64_t fee_una) {
    const auto terms = MakeDinTerms(s.record.offer, s.record.accept);
    RequireDinKey(keys, terms.refund_pubkey);
    const auto htlc = BuildDinHtlc(terms);
    Transaction tx = BuildDinRefundTx(terms, htlc, funding, Payout{s.din_payout_script, AmountUna::Una(fee_una)});
    SetDinRefundWitness(tx, htlc, SchnorrSign(keys.din_secret_key, DinRefundSighash(tx, funding, htlc)));
    return tx.Serialize(TxSerializationMode::WithWitness);
}

std::vector<uint8_t> SignedBtcClaim(const SwapSession& s, const SwapKeys& keys,
                                    const BtcFunding& funding, uint64_t fee_sat) {
    const auto terms = MakeBtcTerms(s.record.offer, s.record.accept);
    RequireBtcKey(keys, terms.claim_pubkey);
    BtcTx tx = BuildBtcClaimTx(funding, s.btc_payout_script, fee_sat);
    SetBtcClaimWitness(tx, terms, EcdsaSignAll(keys.btc_secret_key, BtcHtlcSighash(tx, terms, funding)),
                       SecretOf(s));
    return SerializeBtcTx(tx);
}

std::vector<uint8_t> SignedBtcRefund(const SwapSession& s, const SwapKeys& keys,
                                     const BtcFunding& funding, uint64_t fee_sat) {
    const auto terms = MakeBtcTerms(s.record.offer, s.record.accept);
    RequireBtcKey(keys, terms.refund_pubkey);
    BtcTx tx = BuildBtcRefundTx(terms, funding, s.btc_payout_script, fee_sat);
    SetBtcRefundWitness(tx, terms, EcdsaSignAll(keys.btc_secret_key, BtcHtlcSighash(tx, terms, funding)));
    return SerializeBtcTx(tx);
}

// ---- Node RPC chain access ---------------------------------------------------

SwapWatchSpec WatchSpecFor(const SwapSession& session, const RunnerConfig& config) {
    SwapWatchSpec w;
    w.offer = session.record.offer;
    w.accept = session.record.accept;
    w.din_scan_from_height = session.din_scan_from_height;
    w.btc_scan_from_height = session.btc_scan_from_height;
    w.din_funding_txid = session.din_funding_txid;
    w.btc_funding_txid = session.btc_funding_txid;
    w.btc_chain = config.btc_chain;
    return w;
}

RpcSwapChainIo::RpcSwapChainIo(DinRpc din, BtcRpc btc, const SwapSession& session, const RunnerConfig& config)
    : RpcSwapChainIo(std::move(din), std::move(btc), WatchSpecFor(session, config)) {}

RpcSwapChainIo::RpcSwapChainIo(DinRpc din, BtcRpc btc, SwapWatchSpec spec)
    : din_(std::move(din)), btc_(std::move(btc)), spec_(std::move(spec)) {
    RebuildWatchers();
}

void RpcSwapChainIo::RebuildWatchers() {
    DinWatchTarget d;
    d.terms = MakeDinTerms(spec_.offer, spec_.accept);
    d.scan_from_height = spec_.din_scan_from_height;
    d.expected_amount_una = spec_.offer.din_amount_una;
    d.expected_funding_txid = spec_.din_funding_txid;
    din_watcher_ = std::make_unique<DinWatcher>(din_, d);
    BtcWatchTarget b;
    b.terms = MakeBtcTerms(spec_.offer, spec_.accept);
    b.scan_from_height = spec_.btc_scan_from_height;
    b.expected_amount_sat = spec_.offer.btc_amount_sat;
    b.expected_funding_txid = spec_.btc_funding_txid;
    b.expected_chain = spec_.btc_chain;
    btc_watcher_ = std::make_unique<BtcWatcher>(btc_, b);
}

void RpcSwapChainIo::PinFunding(const std::string& din_txid, const std::string& btc_txid) {
    bool changed = false;
    if (!din_txid.empty() && din_txid != spec_.din_funding_txid) { spec_.din_funding_txid = din_txid; changed = true; }
    if (!btc_txid.empty() && btc_txid != spec_.btc_funding_txid) { spec_.btc_funding_txid = btc_txid; changed = true; }
    if (changed) RebuildWatchers();
}

void SwapChainIo::ArmTower(const std::string&) {
    throw std::runtime_error("no watchtower configured");
}

void RpcSwapChainIo::ArmTower(const std::string& package_text) {
    if (!tower_sink_) throw std::runtime_error("no watchtower configured");
    tower_sink_(package_text);
}

DinWatchReport RpcSwapChainIo::ObserveDin() { return din_watcher_->Observe(); }
BtcWatchReport RpcSwapChainIo::ObserveBtc() { return btc_watcher_->Observe(); }

std::string RpcSwapChainIo::FundDin(const std::string& address, uint64_t amount_una) {
    Json::Value p(Json::objectValue);
    p["address"] = address;
    p["amount_una"] = Json::UInt64(amount_una);
    return TxidFrom(din_("wallet.sendtoaddress", p), "wallet.sendtoaddress");
}

std::string RpcSwapChainIo::FundBtc(const std::string& address, uint64_t amount_sat) {
    // Exact decimal string: never let a double round the amount.
    char amount[32];
    std::snprintf(amount, sizeof amount, "%llu.%08llu", static_cast<unsigned long long>(amount_sat / 100'000'000),
                  static_cast<unsigned long long>(amount_sat % 100'000'000));
    return TxidFrom(btc_("sendtoaddress", Params({address, std::string(amount)})), "sendtoaddress");
}

std::string RpcSwapChainIo::BroadcastDin(const std::vector<uint8_t>& raw_tx) {
    return TxidFrom(din_("sendrawtransaction", Params({ToHex(raw_tx)})), "sendrawtransaction (DIN)");
}

std::string RpcSwapChainIo::BroadcastBtc(const std::vector<uint8_t>& raw_tx) {
    return TxidFrom(btc_("sendrawtransaction", Params({ToHex(raw_tx)})), "sendrawtransaction (BTC)");
}

// ---- Runner -----------------------------------------------------------------

SwapRunner::SwapRunner(SwapSession session, SwapKeys keys, RunnerConfig config, SwapChainIo& io, SwapStore& store)
    : session_(std::move(session)), keys_(keys), config_(std::move(config)), io_(io), store_(store) {
    const auto& r = session_.record;
    MakeDinTerms(r.offer, r.accept);  // throws if the accept is for another offer
    if (r.role == Role::DinSeller) {
        RequireDinKey(keys_, r.offer.din_refund_pubkey);
        RequireBtcKey(keys_, r.offer.btc_claim_pubkey);
        SecretOf(session_);
    } else {
        RequireDinKey(keys_, r.accept.din_claim_pubkey);
        RequireBtcKey(keys_, r.accept.btc_refund_pubkey);
    }
    if (session_.din_payout_script.empty() || session_.btc_payout_script.empty()) Refuse("empty payout script");
}

TickReport SwapRunner::Tick(uint32_t wall_clock_unix) {
    TickReport report;
    report.before = report.after = session_.record.state;
    const DinWatchReport din = io_.ObserveDin();
    const BtcWatchReport btc = io_.ObserveBtc();
    if (!din.ok || !btc.ok) {
        report.events.push_back(std::string("not observed: ") + (din.ok ? "" : "Dinero ") + (btc.ok ? "" : "Bitcoin ") +
                                "node unreachable or inconsistent");
        return report;
    }
    report.observed = true;

    Observations obs;
    obs.wall_clock_unix = wall_clock_unix;
    obs.din_mtp_unix = din.mtp_unix;
    obs.btc_mtp_unix = btc.mtp_unix;
    obs.din = din.htlc;
    obs.btc = btc.htlc;
    const StepResult step = Step(session_.record, obs);

    SwapSession next = session_;
    next.record = step.record;
    if (EncodeRecord(next.record) != EncodeRecord(session_.record)) store_.Save(next);  // throws: nothing done
    session_ = std::move(next);
    report.after = session_.record.state;

    for (const auto& a : step.actions) {
        report.actions.push_back(a.kind);
        Execute(a, din, btc, wall_clock_unix, report.events);
    }
    // Bob's tower must hold the package before he can safely go offline. The
    // engine asks once (ArmTower); a failed attempt is retried every tick.
    if (session_.record.role == Role::BtcSeller && config_.use_tower && !session_.tower_armed &&
        session_.record.state == SwapState::BtcLocked) {
        ArmTower(din, btc, report.events);
    }
    return report;
}

void SwapRunner::ArmTower(const DinWatchReport& din, const BtcWatchReport& btc, std::vector<std::string>& events) {
    try {
        if (!din.funding || !btc.funding) throw std::runtime_error("HTLC outputs not both known yet");
        const auto& offer = session_.record.offer;
        FeeLadderPolicy din_policy;
        din_policy.start_feerate_una_per_vb = config_.din_tower_start_feerate_una_per_vb;
        din_policy.max_rungs = config_.tower_rungs;
        din_policy.max_fee_una = offer.din_amount_una / 100 * config_.tower_max_fee_percent;
        din_policy.min_payout_una = offer.din_amount_una / 2;
        BtcFeeLadderPolicy btc_policy;
        btc_policy.start_feerate_sat_per_vb = config_.btc_tower_start_feerate_sat_per_vb;
        btc_policy.max_rungs = config_.tower_rungs;
        btc_policy.max_fee_sat = offer.btc_amount_sat / 100 * config_.tower_max_fee_percent;
        btc_policy.min_payout_sat = offer.btc_amount_sat / 2;
        const auto package = BuildTowerPackage(session_, keys_, *din.funding, *btc.funding, din_policy, btc_policy);
        io_.ArmTower(EncodeTowerPackage(package));
        SwapSession next = session_;
        next.tower_armed = true;
        store_.Save(next);
        session_ = std::move(next);
        events.push_back("watchtower armed: " + std::to_string(package.din_claims.size()) + " DIN claim and " +
                         std::to_string(package.btc_refunds.size()) + " BTC refund rungs");
    } catch (const std::exception& e) {
        events.push_back(std::string("ALERT: watchtower not armed, this wallet must stay online: ") + e.what());
    }
}

// Bitcoin replaces by fee: while a claim/refund stays unconfirmed, its fee
// doubles every 30 minutes since the state began, up to btc_fee_max_percent of
// the amount. Derived from the persisted state, so a restart picks up where it was.
uint64_t SwapRunner::BtcFeeNow(uint32_t now) const {
    const uint64_t base = config_.btc_fee_sat;
    const uint64_t cap = std::max(base, session_.record.offer.btc_amount_sat / 100 * config_.btc_fee_max_percent);
    const uint32_t since = session_.record.state_since_unix;
    const uint32_t doublings = now > since ? (now - since) / 1800 : 0;
    uint64_t fee = base;
    for (uint32_t i = 0; i < doublings && fee < cap; ++i) fee *= 2;
    return std::min(fee, cap);
}

void SwapRunner::PinAndSave(const std::string& din_txid, const std::string& btc_txid) {
    SwapSession next = session_;
    if (!din_txid.empty()) next.din_funding_txid = din_txid;
    if (!btc_txid.empty()) next.btc_funding_txid = btc_txid;
    store_.Save(next);
    session_ = std::move(next);
    io_.PinFunding(din_txid, btc_txid);
}

void SwapRunner::Execute(const Action& action, const DinWatchReport& din, const BtcWatchReport& btc, uint32_t now,
                         std::vector<std::string>& events) {
    const auto& offer = session_.record.offer;
    auto done = [&](const char* what, const std::string& txid) {
        events.push_back(std::string(what) + " " + txid + " (" + action.reason + ")");
    };
    try {
        switch (action.kind) {
        case ActionKind::FundDinHtlc: {
            const std::string txid =
                io_.FundDin(DinHtlcAddressFor(session_.record, config_.din_hrp), offer.din_amount_una);
            PinAndSave(txid, "");  // only this transaction is Alice's lock
            return done("funded DIN HTLC:", txid);
        }
        case ActionKind::FundBtcHtlc: {
            // Pin the DIN lock Bob checked BEFORE his BTC leaves: later payments
            // to the same address must never become "the lock" for his claim.
            if (!din.funding) throw std::runtime_error("DIN lock unknown; not funding BTC");
            PinAndSave(din.funding->txid.AsUint256().GetHex(), "");
            const std::string txid =
                io_.FundBtc(BtcHtlcAddressFor(session_.record, config_.btc_hrp), offer.btc_amount_sat);
            PinAndSave("", txid);  // a decoy paying the same script is not Bob's lock
            return done("funded BTC HTLC:", txid);
        }
        case ActionKind::ClaimBtc:
            if (!btc.funding) throw std::runtime_error("BTC HTLC output unknown");
            return done("broadcast BTC claim:",
                        io_.BroadcastBtc(SignedBtcClaim(session_, keys_, *btc.funding, BtcFeeNow(now))));
        case ActionKind::RefundBtc:
            if (!btc.funding) throw std::runtime_error("BTC HTLC output unknown");
            return done("broadcast BTC refund:",
                        io_.BroadcastBtc(SignedBtcRefund(session_, keys_, *btc.funding, BtcFeeNow(now))));
        case ActionKind::ClaimDin: {
            if (!din.funding) throw std::runtime_error("DIN HTLC output unknown");
            // Dinero does not replace by fee: pick the fee for the time left before
            // Alice's refund opens (24 h or more: base fee; 6 h or less: urgent fee).
            const int64_t left = int64_t(offer.t_din_unix) - int64_t(din.mtp_unix);
            constexpr int64_t kRelaxed = 24 * 3600, kUrgent = 6 * 3600;
            const uint64_t lo = config_.din_fee_una, hi = std::max(config_.din_fee_una, config_.din_fee_urgent_una);
            const uint64_t fee = left >= kRelaxed ? lo
                               : left <= kUrgent  ? hi
                                                  : lo + (hi - lo) * uint64_t(kRelaxed - left) / uint64_t(kRelaxed - kUrgent);
            return done("broadcast DIN claim:", io_.BroadcastDin(SignedDinClaim(session_, keys_, *din.funding, fee)));
        }
        case ActionKind::RefundDin:
            if (!din.funding) throw std::runtime_error("DIN HTLC output unknown");
            return done("broadcast DIN refund:",
                        io_.BroadcastDin(SignedDinRefund(session_, keys_, *din.funding, config_.din_fee_una)));
        case ActionKind::ArmTower:
            if (!config_.use_tower) events.push_back("no watchtower configured: this wallet must stay online");
            return;  // armed after the actions (see Tick), retried until it succeeds
        case ActionKind::Alert:
            events.push_back("ALERT: " + action.reason);
            return;
        }
    } catch (const std::exception& e) {
        // No rollback: the engine re-broadcasts on later ticks, or alerts on funding.
        events.push_back("action failed (" + action.reason + "): " + e.what());
    }
}

}  // namespace dinero::swap
