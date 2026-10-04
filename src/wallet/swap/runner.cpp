#include "wallet/swap/runner.h"

#include "bech32/bech32.hpp"
#include "crypto/evp_secp256k1.h"
#include "crypto/sha256.h"
#include "wallet/swap/btc_tx.h"

#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <secp256k1_schnorrsig.h>

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

std::string ToHex(const std::vector<uint8_t>& b) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (auto x : b) { s += d[x >> 4]; s += d[x & 15]; }
    return s;
}

std::vector<uint8_t> FromHex(const std::string& h) {
    if (h.size() % 2) Refuse("odd-length hex");
    std::vector<uint8_t> out;
    for (size_t i = 0; i < h.size(); i += 2) {
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        const int hi = nib(h[i]), lo = nib(h[i + 1]);
        if (hi < 0 || lo < 0) Refuse("bad hex");
        out.push_back(static_cast<uint8_t>(hi << 4 | lo));
    }
    return out;
}

secp256k1_keypair Keypair(const Bytes32& secret_key) {
    secp256k1_keypair kp;
    if (secp256k1_keypair_create(crypto::GetSecp256k1ContextSignVerify(), &kp, secret_key.data()) != 1) {
        Refuse("invalid secret key");
    }
    return kp;
}

Bytes32 XOnlyOf(const Bytes32& secret_key) {
    auto* secp = crypto::GetSecp256k1ContextSignVerify();
    auto kp = Keypair(secret_key);
    secp256k1_xonly_pubkey x;
    secp256k1_keypair_xonly_pub(secp, &x, nullptr, &kp);
    Bytes32 out{};
    secp256k1_xonly_pubkey_serialize(secp, out.data(), &x);
    return out;
}

std::array<uint8_t, 33> CompressedOf(const Bytes32& secret_key) {
    auto* secp = crypto::GetSecp256k1ContextSignVerify();
    secp256k1_pubkey pk;
    if (secp256k1_ec_pubkey_create(secp, &pk, secret_key.data()) != 1) Refuse("invalid secret key");
    std::array<uint8_t, 33> out{};
    size_t len = out.size();
    secp256k1_ec_pubkey_serialize(secp, out.data(), &len, &pk, SECP256K1_EC_COMPRESSED);
    return out;
}

std::array<uint8_t, 64> SchnorrSign(const Bytes32& secret_key, const Bytes32& msg) {
    auto kp = Keypair(secret_key);
    std::array<uint8_t, 64> sig{};
    const std::array<uint8_t, 32> aux{};
    if (secp256k1_schnorrsig_sign32(crypto::GetSecp256k1ContextSignVerify(), sig.data(), msg.data(), &kp,
                                    aux.data()) != 1) {
        Refuse("schnorr signing failed");
    }
    return sig;
}

std::vector<uint8_t> EcdsaSignAll(const Bytes32& secret_key, const Bytes32& msg) {
    auto* secp = crypto::GetSecp256k1ContextSignVerify();
    secp256k1_ecdsa_signature sig;
    if (secp256k1_ecdsa_sign(secp, &sig, msg.data(), secret_key.data(), nullptr, nullptr) != 1) {  // low-S
        Refuse("ecdsa signing failed");
    }
    std::vector<uint8_t> der(72);
    size_t len = der.size();
    secp256k1_ecdsa_signature_serialize_der(secp, der.data(), &len, &sig);
    der.resize(len);
    der.push_back(0x01);  // SIGHASH_ALL
    return der;
}

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

// Node results may also report a failure in-band as {"error": ...}.
std::string TxidFrom(const std::optional<Json::Value>& r, const std::string& what) {
    if (!r) throw std::runtime_error(what + ": RPC failed");
    if (r->isString()) return r->asString();
    if (r->isObject() && r->isMember("error") && !(*r)["error"].isNull()) {
        throw std::runtime_error(what + ": " + (*r)["error"].toStyledString());
    }
    if (r->isObject() && (*r)["txid"].isString()) return (*r)["txid"].asString();
    throw std::runtime_error(what + ": unexpected result " + r->toStyledString());
}

}  // namespace

// ---- Session persistence ---------------------------------------------------

std::string EncodeSession(const SwapSession& s) {
    std::ostringstream out;
    out << "record=" << EncodeRecord(s.record) << "\n"
        << "btc_scan_from_height=" << s.btc_scan_from_height << "\n"
        << "din_payout_script=" << ToHex(s.din_payout_script) << "\n"
        << "btc_payout_script=" << ToHex(s.btc_payout_script) << "\n";
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
    return report;
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
            events.push_back("watchtower not available yet (milestone 6): this wallet must stay online");
            return;
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
