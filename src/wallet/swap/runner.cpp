#include "wallet/swap/runner.h"

#include "bech32/bech32.hpp"
#include "crypto/evp_secp256k1.h"
#include "crypto/sha256.h"
#include "wallet/swap/btc_tx.h"
#include "wallet/swap/swap_crypto.h"
#include "wallet/swap/tower.h"

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

RpcSwapChainIo::RpcSwapChainIo(DinRpc din, BtcRpc btc, const SwapSession& session, const RunnerConfig& config)
    : din_(std::move(din)), btc_(std::move(btc)),
      din_watcher_(din_, MakeDinTerms(session.record.offer, session.record.accept), config.din_hrp),
      btc_watcher_(btc_, BtcWatchTarget{MakeBtcTerms(session.record.offer, session.record.accept),
                                        session.btc_scan_from_height}) {}

RpcSwapChainIo::RpcSwapChainIo(DinRpc din, BtcRpc btc, const SwapOffer& offer, const SwapAccept& accept,
                               uint32_t btc_scan_from_height, const std::string& din_hrp)
    : din_(std::move(din)), btc_(std::move(btc)),
      din_watcher_(din_, MakeDinTerms(offer, accept), din_hrp),
      btc_watcher_(btc_, BtcWatchTarget{MakeBtcTerms(offer, accept), btc_scan_from_height}) {}

void SwapChainIo::ArmTower(const std::string&) {
    throw std::runtime_error("no watchtower configured");
}

void RpcSwapChainIo::ArmTower(const std::string& package_text) {
    if (!tower_sink_) throw std::runtime_error("no watchtower configured");
    tower_sink_(package_text);
}

DinWatchReport RpcSwapChainIo::ObserveDin() { return din_watcher_.Observe(); }
BtcWatchReport RpcSwapChainIo::ObserveBtc() { return btc_watcher_.Observe(); }

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
        Execute(a, din, btc, report.events);
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

void SwapRunner::Execute(const Action& action, const DinWatchReport& din, const BtcWatchReport& btc,
                         std::vector<std::string>& events) {
    const auto& offer = session_.record.offer;
    auto done = [&](const char* what, const std::string& txid) {
        events.push_back(std::string(what) + " " + txid + " (" + action.reason + ")");
    };
    try {
        switch (action.kind) {
        case ActionKind::FundDinHtlc:
            return done("funded DIN HTLC:",
                        io_.FundDin(DinHtlcAddressFor(session_.record, config_.din_hrp), offer.din_amount_una));
        case ActionKind::FundBtcHtlc:
            return done("funded BTC HTLC:",
                        io_.FundBtc(BtcHtlcAddressFor(session_.record, config_.btc_hrp), offer.btc_amount_sat));
        case ActionKind::ClaimBtc:
            if (!btc.funding) throw std::runtime_error("BTC HTLC output unknown");
            return done("broadcast BTC claim:",
                        io_.BroadcastBtc(SignedBtcClaim(session_, keys_, *btc.funding, config_.btc_fee_sat)));
        case ActionKind::RefundBtc:
            if (!btc.funding) throw std::runtime_error("BTC HTLC output unknown");
            return done("broadcast BTC refund:",
                        io_.BroadcastBtc(SignedBtcRefund(session_, keys_, *btc.funding, config_.btc_fee_sat)));
        case ActionKind::ClaimDin:
            if (!din.funding) throw std::runtime_error("DIN HTLC output unknown");
            return done("broadcast DIN claim:",
                        io_.BroadcastDin(SignedDinClaim(session_, keys_, *din.funding, config_.din_fee_una)));
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
