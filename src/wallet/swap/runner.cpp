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
#include <cmath>
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
        << "btc_funding_raw=" << s.btc_funding_raw << "\n"
        << "din_sweep_pubkey=" << (s.din_sweep_pubkey == Bytes32{} ? std::string()
                                     : ToHex(std::vector<uint8_t>(s.din_sweep_pubkey.begin(), s.din_sweep_pubkey.end())))
        << "\n"
        << "din_swept=" << (s.din_swept ? 1 : 0) << "\n"
        << "din_claim_txid=" << s.din_claim_txid << "\n"
        << "din_claim_value=" << s.din_claim_value << "\n"
        << "din_payout_script=" << ToHex(s.din_payout_script) << "\n"
        << "btc_payout_script=" << ToHex(s.btc_payout_script) << "\n"
        << "tower_armed=" << (s.tower_armed ? 1 : 0) << "\n"
        << "tower_package_hash=" << s.tower_package_hash << "\n"
        << "btc_funding_vout=" << s.btc_funding_vout << "\n";
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
    if (kv.count("din_sweep_pubkey") && !kv["din_sweep_pubkey"].empty()) {
        const auto k = FromHex(kv["din_sweep_pubkey"]);
        if (k.size() != 32) Refuse("bad din_sweep_pubkey");
        std::copy(k.begin(), k.end(), s.din_sweep_pubkey.begin());
    }
    s.din_claim_txid = txid_field("din_claim_txid");
    if (kv.count("din_claim_value") && !kv["din_claim_value"].empty()) {
        const std::string& v = kv["din_claim_value"];
        if (v.size() > 19 || v.find_first_not_of("0123456789") != std::string::npos) Refuse("bad din_claim_value");
        s.din_claim_value = std::stoull(v);
    }
    if (kv.count("din_swept")) {
        if (kv["din_swept"] != "0" && kv["din_swept"] != "1") Refuse("bad din_swept");
        s.din_swept = kv["din_swept"] == "1";
    }
    if (kv.count("btc_funding_raw") && !kv["btc_funding_raw"].empty()) {
        FromHex(kv["btc_funding_raw"]);  // validates
        s.btc_funding_raw = kv["btc_funding_raw"];
    }
    if (kv.count("din_scan_from_height")) {
        const std::string& v = kv["din_scan_from_height"];
        if (v.empty() || v.size() > 10 || v.find_first_not_of("0123456789") != std::string::npos) Refuse("bad DIN scan height");
        const unsigned long long h = std::stoull(v);
        if (h > UINT32_MAX) Refuse("bad DIN scan height");
        s.din_scan_from_height = static_cast<uint32_t>(h);
    }
    s.tower_package_hash = txid_field("tower_package_hash");  // 64 hex, like a txid
    if (kv.count("tower_armed")) {  // absent in sessions saved before the tower existed
        if (kv["tower_armed"] != "0" && kv["tower_armed"] != "1") Refuse("bad tower_armed");
        s.tower_armed = kv["tower_armed"] == "1";
    }
    if (kv.count("btc_funding_vout")) {  // absent in sessions saved before it existed
        const std::string& v = kv["btc_funding_vout"];
        if (v != "-1" && (v.empty() || v.size() > 9 || v.find_first_not_of("0123456789") != std::string::npos)) {
            Refuse("bad btc_funding_vout");
        }
        s.btc_funding_vout = std::stoi(v);
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
    // With CPFP the claim pays Bob's sweep output; a child moves it to his wallet.
    const bool cpfp = s.din_sweep_pubkey != Bytes32{};
    const auto payout = cpfp ? BuildDinSweepOutput(s.din_sweep_pubkey).script_pubkey : s.din_payout_script;
    Transaction tx = BuildDinClaimTx(htlc, funding, Payout{payout, AmountUna::Una(fee_una)});
    SetDinClaimWitness(tx, terms, htlc, SchnorrSign(keys.din_secret_key, DinClaimSighash(tx, funding, htlc)),
                       SecretOf(s));
    return tx.Serialize(TxSerializationMode::WithWitness);
}

std::vector<uint8_t> SignedDinSweep(const SwapSession& s, const SwapKeys& keys, const FundingOutput& claim_output,
                                    uint64_t fee_una) {
    if (s.din_sweep_pubkey == Bytes32{}) Refuse("this swap has no sweep output");
    if (XOnlyOf(keys.din_sweep_secret_key) != s.din_sweep_pubkey) Refuse("sweep key does not match the swap");
    const auto sweep = BuildDinSweepOutput(s.din_sweep_pubkey);
    FundingOutput parent = claim_output;
    parent.script_pubkey = sweep.script_pubkey;
    Transaction tx = BuildDinSweepTx(sweep, parent, Payout{s.din_payout_script, AmountUna::Una(fee_una)});
    SetDinSweepWitness(tx, sweep, SchnorrSign(keys.din_sweep_secret_key, DinSweepSighash(tx, parent, sweep)));
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

PreparedBtcFunding SwapChainIo::PrepareFundBtc(const std::string&, uint64_t) {
    throw std::runtime_error("preparing a BTC funding transaction is not supported here");
}

PreparedBtcFunding RpcSwapChainIo::PrepareFundBtc(const std::string& address, uint64_t amount_sat) {
    char amount[32];
    std::snprintf(amount, sizeof amount, "%llu.%08llu", static_cast<unsigned long long>(amount_sat / 100'000'000),
                  static_cast<unsigned long long>(amount_sat % 100'000'000));
    Json::Value outputs(Json::objectValue);
    outputs[address] = std::string(amount);
    const auto raw = btc_("createrawtransaction", Params({Json::Value(Json::arrayValue), outputs}));
    if (!raw || !raw->isString()) throw std::runtime_error("createrawtransaction failed");
    Json::Value opts(Json::objectValue);
    opts["lockUnspents"] = false;  // locked persistently below (re-locking would be refused)
    // Not replaceable: a fee bump would give the funding a new txid, and the
    // pinned watchers and the tower's package would watch the wrong output.
    opts["replaceable"] = false;
    opts["conf_target"] = 2;
    const auto funded = btc_("fundrawtransaction", Params({*raw, opts}));
    if (!funded || !(*funded)["hex"].isString()) throw std::runtime_error("fundrawtransaction failed (wallet balance?)");
    const auto signed_tx = btc_("signrawtransactionwithwallet", Params({(*funded)["hex"]}));
    if (!signed_tx || !(*signed_tx)["complete"].asBool()) throw std::runtime_error("signrawtransactionwithwallet failed");
    const std::string hex = (*signed_tx)["hex"].asString();
    const auto decoded = btc_("decoderawtransaction", Params({hex}));
    if (!decoded) throw std::runtime_error("decoderawtransaction failed");
    PreparedBtcFunding p;
    p.raw = FromHex(hex);
    p.txid = (*decoded)["txid"].asString();
    bool found = false;
    for (const auto& out : (*decoded)["vout"]) {
        if (out["scriptPubKey"]["address"].asString() == address &&
            static_cast<uint64_t>(std::llround(out["value"].asDouble() * 1e8)) == amount_sat) {
            p.vout = out["n"].asUInt();
            found = true;
        }
    }
    if (!found) throw std::runtime_error("prepared funding does not pay the HTLC");
    // Persistent lock: survives a Bitcoin Core restart while the funding waits.
    Json::Value inputs(Json::arrayValue);
    for (const auto& in : (*decoded)["vin"]) {
        Json::Value o(Json::objectValue);
        o["txid"] = in["txid"];
        o["vout"] = in["vout"];
        inputs.append(o);
    }
    if (!btc_("lockunspent", Params({false, inputs, true}))) {
        throw std::runtime_error("could not lock the funding inputs in the Bitcoin wallet");
    }
    return p;
}

void RpcSwapChainIo::ReleasePreparedFunding(const std::vector<uint8_t>& raw) {
    const auto decoded = btc_("decoderawtransaction", Params({ToHex(raw)}));
    if (!decoded) return;
    Json::Value inputs(Json::arrayValue);
    for (const auto& in : (*decoded)["vin"]) {
        Json::Value o(Json::objectValue);
        o["txid"] = in["txid"];
        o["vout"] = in["vout"];
        inputs.append(o);
    }
    btc_("lockunspent", Params({true, inputs}));  // unlock: the wallet may spend them again
}

std::optional<bool> RpcSwapChainIo::PreparedFundingUnsent(const std::vector<uint8_t>& raw) {
    const auto decoded = btc_("decoderawtransaction", Params({ToHex(raw)}));
    if (!decoded || !decoded->isObject() || !(*decoded)["vin"].isArray() || (*decoded)["vin"].empty()) {
        return std::nullopt;
    }
    for (const auto& in : (*decoded)["vin"]) {
        const auto out = btc_("gettxout", Params({in["txid"], in["vout"], true}));  // mempool spends count
        if (!out) return std::nullopt;
        if (out->isNull()) return false;
    }
    return true;
}

bool RpcSwapChainIo::TowerAcknowledged(const std::string& swap_id, const std::string& package_hash) {
    return tower_ack_ && tower_ack_(swap_id, package_hash);
}

std::optional<bool> RpcSwapChainIo::DinOutputUnspent(const TxId& txid, uint32_t vout) {
    const auto r = din_("gettxout", Params({txid.AsUint256().GetHex(), Json::Value(vout)}));
    if (!r) return std::nullopt;
    return !r->isNull();
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
        SingleChainRebroadcast(din, btc, wall_clock_unix, report.events);
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
    const bool funds_btc = std::any_of(step.actions.begin(), step.actions.end(),
                                       [](const Action& a) { return a.kind == ActionKind::FundBtcHtlc; });
    if (funds_btc && config_.require_tower && !config_.use_tower) {
        // Held in Accepted, so it can still be cancelled or run once a tower is set.
        report.events.push_back("ALERT: not locking BTC: this network requires a watchtower "
                                "(set swap.tower_inbox and restart, or cancel the swap)");
        return report;
    }

    SwapSession next = session_;
    next.record = step.record;
    if (EncodeRecord(next.record) != EncodeRecord(session_.record)) store_.Save(next);  // throws: nothing done
    session_ = std::move(next);
    report.after = session_.record.state;

    for (const auto& a : step.actions) {
        report.actions.push_back(a.kind);
        Execute(a, din, btc, wall_clock_unix, report.events);
    }
    SendPreparedFunding(din, btc, wall_clock_unix, report.events);
    BumpOrSweepDin(din, wall_clock_unix, report.events);
    // Bob's tower must hold the package before he can safely go offline. The
    // engine asks once (ArmTower); a failed attempt is retried every tick.
    // Also while a prepared funding waits for the tower (it is never sent unarmed).
    if (session_.record.role == Role::BtcSeller && config_.use_tower && !session_.tower_armed &&
        (session_.record.state == SwapState::BtcLocked ||
         (session_.record.state == SwapState::BtcLockBroadcast && !session_.btc_funding_raw.empty()))) {
        ArmTower(din, btc, report.events);
    }
    return report;
}

void SwapRunner::ArmTower(const DinWatchReport& din, const BtcWatchReport& btc, std::vector<std::string>& events) {
    const auto btc_funding = ArmedBtcFunding(btc);  // the prepared, unsent funding too
    if (!din.funding || !btc_funding) {
        events.push_back("ALERT: watchtower not armed, this wallet must stay online: HTLC outputs not both known yet");
        return;
    }
    ArmTowerWith(*din.funding, *btc_funding, events);
}

TowerPackage SwapRunner::MyTowerPackage(const FundingOutput& din_funding, const BtcFunding& btc_funding) const {
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
    // Deterministic (fixed Schnorr aux, RFC 6979 ECDSA): rebuilding it gives the
    // exact transactions the tower holds.
    return BuildTowerPackage(session_, keys_, din_funding, btc_funding, din_policy, btc_policy);
}

std::optional<BtcFunding> SwapRunner::ArmedBtcFunding(const BtcWatchReport& btc) const {
    if (btc.funding) return btc.funding;
    if (session_.btc_funding_txid.size() != 64 || session_.btc_funding_vout < 0) return std::nullopt;
    const auto wire = FromHex(session_.btc_funding_txid);
    BtcFunding f;
    std::copy(wire.rbegin(), wire.rend(), f.txid.begin());
    f.vout = static_cast<uint32_t>(session_.btc_funding_vout);
    f.value_sat = session_.record.offer.btc_amount_sat;
    return f;
}

bool SwapRunner::ArmTowerWith(const FundingOutput& din_funding, const BtcFunding& btc_funding,
                              std::vector<std::string>& events) {
    try {
        const auto package = MyTowerPackage(din_funding, btc_funding);
        const std::string text = EncodeTowerPackage(package);
        io_.ArmTower(text);
        SwapSession next = session_;
        next.tower_armed = true;
        next.tower_package_hash = TowerPackageHash(text);
        next.btc_funding_vout = static_cast<int32_t>(btc_funding.vout);
        if (next.btc_funding_txid.empty()) {
            next.btc_funding_txid = ToHex(std::vector<uint8_t>(btc_funding.txid.rbegin(), btc_funding.txid.rend()));
        }
        store_.Save(next);
        session_ = std::move(next);
        events.push_back("watchtower armed: " + std::to_string(package.din_claims.size()) + " DIN claim and " +
                         std::to_string(package.btc_refunds.size()) + " BTC refund rungs");
        return true;
    } catch (const std::exception& e) {
        events.push_back(std::string("ALERT: watchtower not armed, this wallet must stay online: ") + e.what());
        return false;
    }
}

// Bob with a tower: his prepared (signed, unsent) funding leaves only once
// the tower confirmed it holds the package for exactly that outpoint.
void SwapRunner::ExpirePreparedFunding(uint32_t now, const std::string& why, std::vector<std::string>& events) {
    // The raw is still here after a lost broadcast reply too: abort only when
    // Bitcoin Core shows every prepared input unspent.
    const auto unsent = io_.PreparedFundingUnsent(FromHex(session_.btc_funding_raw));
    if (!unsent) {
        events.push_back("ALERT: prepared BTC funding expired but Bitcoin Core cannot confirm it was never sent; "
                         "not sending it, checking again: " + why);
        return;
    }
    if (!*unsent) {
        SwapSession next = session_;
        next.btc_funding_raw.clear();  // never send it again; the watcher picks up the lock if it went out
        store_.Save(next);
        session_ = std::move(next);
        events.push_back("ALERT: prepared BTC funding may have been sent (its inputs are spent); watching for "
                         "the lock: " + why);
        return;
    }
    try {
        io_.ReleasePreparedFunding(FromHex(session_.btc_funding_raw));
    } catch (const std::exception&) {
    }
    SwapSession next = session_;
    next.btc_funding_raw.clear();
    next.record.state = SwapState::Aborted;  // nothing was sent: nothing is locked
    next.record.state_since_unix = now;
    store_.Save(next);
    session_ = std::move(next);
    events.push_back("ALERT: prepared BTC funding discarded, swap aborted (nothing was sent): " + why);
}

void SwapRunner::SendPreparedFunding(const DinWatchReport& din, const BtcWatchReport& btc, uint32_t now,
                                     std::vector<std::string>& events) {
    const auto& r = session_.record;
    if (r.role != Role::BtcSeller || r.state != SwapState::BtcLockBroadcast || session_.btc_funding_raw.empty()) return;
    // The checks Bob made before preparing must still hold when the BTC leaves
    // (the tower's ack may come hours later).
    if (!din.ok || !btc.ok) return;
    const auto& o = r.offer;
    if (din.htlc.spent) return ExpirePreparedFunding(now, "the DIN lock is already spent", events);
    if (uint64_t(now) + kBobMinDinDeadlineAheadSeconds > o.t_din_unix ||
        uint64_t(din.mtp_unix) + kBobMinDinDeadlineAheadSeconds > o.t_din_unix) {
        return ExpirePreparedFunding(now, "the DIN deadline is now less than 36 h away", events);
    }
    if (!din.funding || din.funding->txid.AsUint256().GetHex() != session_.din_funding_txid ||
        din.htlc.output_value != o.din_amount_una || din.htlc.output_confirmations < o.n_din_confirmations ||
        now > din.mtp_unix + kMaxMtpLagSeconds || now > btc.mtp_unix + kMaxMtpLagSeconds) {
        return;  // not now (reorg or stall): wait, the deadline check above bounds the wait
    }
    const std::string id = [&] {
        const auto oid = OfferId(r.offer);
        return ToHex(std::vector<uint8_t>(oid.begin(), oid.begin() + 8));
    }();
    if (!session_.tower_armed || !io_.TowerAcknowledged(id, session_.tower_package_hash)) {
        if (now > r.state_since_unix + 30 * 60) {
            events.push_back("ALERT: the watchtower has not confirmed this swap; your BTC was NOT sent. "
                             "Check dinero-swap-tower, or cancel with swap.cancel");
        }
        return;
    }
    try {
        const std::string txid = io_.BroadcastBtc(FromHex(session_.btc_funding_raw));
        SwapSession next = session_;
        next.btc_funding_raw.clear();
        store_.Save(next);
        session_ = std::move(next);
        events.push_back("funded BTC HTLC (watchtower confirmed): " + txid);
    } catch (const std::exception& e) {
        events.push_back(std::string("BTC funding not accepted: ") + e.what());
    }
}

std::string SwapRunner::ForceRefund(uint32_t now) {
    const auto& r = session_.record;
    if (r.role == Role::DinSeller) {
        const DinWatchReport din = io_.ObserveDin();
        if (!din.ok) throw std::runtime_error("Dinero node unreachable");
        if (!din.funding) throw std::runtime_error("no DIN lock found to refund");
        if (din.htlc.spent) throw std::runtime_error("the DIN lock is already spent");
        return io_.BroadcastDin(SignedDinRefund(session_, keys_, *din.funding, config_.din_fee_una));
    }
    const BtcWatchReport btc = io_.ObserveBtc();
    if (!btc.ok) throw std::runtime_error("Bitcoin node unreachable");
    if (!btc.funding) throw std::runtime_error("no BTC lock found to refund");
    if (btc.htlc.spent) throw std::runtime_error("the BTC lock is already spent");
    return io_.BroadcastBtc(SignedBtcRefund(session_, keys_, *btc.funding, BtcFeeNow(now)));
}

// With one chain unobservable nothing is decided or saved, but transactions
// already committed to, which depend only on the observable chain, keep being
// broadcast: an outage of one node must not cost the other chain's deadline.
void SwapRunner::SingleChainRebroadcast(const DinWatchReport& din, const BtcWatchReport& btc, uint32_t now,
                                        std::vector<std::string>& events) {
    const auto& r = session_.record;
    const bool alice = r.role == Role::DinSeller;
    auto settled = [](const HtlcObservation& h) { return h.spent && h.spend_confirmations >= 1; };
    if (btc.ok && btc.funding && !settled(btc.htlc)) {
        if (alice && r.state == SwapState::BtcClaimBroadcast && r.claim_seen) {  // the secret is already public
            return Execute({ActionKind::ClaimBtc, "single-chain re-broadcast"}, din, btc, now, events);
        }
        if (!alice && r.state == SwapState::BtcRefundBroadcast && !btc.htlc.spent_by_claim &&
            btc.mtp_unix > r.offer.t_btc_unix) {
            return Execute({ActionKind::RefundBtc, "single-chain re-broadcast"}, din, btc, now, events);
        }
    }
    if (din.ok && din.funding && !din.htlc.spent) {
        if (!alice && r.state == SwapState::DinClaimBroadcast && r.secret) {
            return Execute({ActionKind::ClaimDin, "single-chain re-broadcast"}, din, btc, now, events);
        }
        if (alice && r.state == SwapState::DinRefundBroadcast && din.mtp_unix > r.offer.t_din_unix) {
            return Execute({ActionKind::RefundDin, "single-chain re-broadcast"}, din, btc, now, events);
        }
    }
}

// Dinero does not replace by fee: a DIN claim (or its CPFP child) is priced
// once, for the time left before Alice's refund opens (24 h or more: base fee;
// 6 h or less: urgent fee; linear in between).
uint64_t SwapRunner::DinFeeByUrgency(uint32_t din_mtp) const {
    const int64_t left = int64_t(session_.record.offer.t_din_unix) - int64_t(din_mtp);
    constexpr int64_t kRelaxed = 24 * 3600, kUrgent = 6 * 3600;
    const uint64_t lo = config_.din_fee_una, hi = std::max(config_.din_fee_una, config_.din_fee_urgent_una);
    return left >= kRelaxed ? lo
         : left <= kUrgent  ? hi
                            : lo + (hi - lo) * uint64_t(kRelaxed - left) / uint64_t(kRelaxed - kUrgent);
}

// Bob with CPFP: his claim pays a sweep output of his own key.
// - Stuck claim: 20 minutes after he broadcast it and still unmined, one
//   child spending his (first-seen) claim pays for both (Dinero mining sorts
//   by ancestor fee rate). Children cannot replace each other: one bump.
// - Mined claim (his or the tower's): sweep its output to his wallet; the
//   swap is not finished until that output is spent.
void SwapRunner::BumpOrSweepDin(const DinWatchReport& din, uint32_t now, std::vector<std::string>& events) {
    const auto& r = session_.record;
    if (r.role != Role::BtcSeller || session_.din_sweep_pubkey == Bytes32{} || session_.din_swept) return;
    const auto sweep_spk = BuildDinSweepOutput(session_.din_sweep_pubkey).script_pubkey;
    try {
        const int64_t left = int64_t(r.offer.t_din_unix) - int64_t(din.mtp_unix);
        const bool bump_due = left <= int64_t(config_.din_bump_window_seconds) ||
                              now >= r.state_since_unix + config_.din_bump_stuck_seconds;
        if (r.state == SwapState::DinClaimBroadcast && !din.htlc.spent && !session_.din_claim_txid.empty() &&
            now >= r.state_since_unix + config_.din_bump_after_seconds && bump_due) {
            FundingOutput parent;
            parent.txid = TxId(uint256::FromHexUnsafe(session_.din_claim_txid));
            parent.vout = 0;
            parent.value = AmountUna::Una(session_.din_claim_value);
            parent.script_pubkey = sweep_spk;
            // The only bump there will be: make it count.
            const uint64_t fee = std::max(config_.din_fee_urgent_una, DinFeeByUrgency(din.mtp_unix));
            events.push_back("DIN claim still unmined; CPFP child: " +
                             io_.BroadcastDin(SignedDinSweep(session_, keys_, parent, fee)));
            return;
        }
        if (din.claim_output && din.htlc.spent_by_claim && din.htlc.spend_confirmations >= 1 &&
            din.claim_output->script_pubkey == sweep_spk) {
            const auto unspent = io_.DinOutputUnspent(din.claim_output->txid, din.claim_output->vout);
            if (!unspent) return;
            if (*unspent) {
                events.push_back("sweeping the DIN to the wallet: " +
                                 io_.BroadcastDin(SignedDinSweep(session_, keys_, *din.claim_output, config_.din_fee_una)));
            } else if (din.htlc.spend_confirmations >=
                       std::max(kSettleConfirmations, session_.record.offer.n_din_confirmations)) {
                // gettxout reads the block UTXO set: null proves "spent in a
                // block" only once the claim itself cannot be reorganised away.
                SwapSession next = session_;
                next.din_swept = true;
                store_.Save(next);
                session_ = std::move(next);
                events.push_back("DIN swept to the wallet");
            }
        }
    } catch (const std::exception& e) {
        events.push_back(std::string("DIN bump/sweep not accepted: ") + e.what());
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

void SwapRunner::SetClaimSeen(bool seen) {
    SwapSession next = session_;
    next.record.claim_seen = seen;
    store_.Save(next);
    session_ = std::move(next);
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
            if (config_.use_tower) {
                // Arm before funding: build and sign the funding, give the tower
                // a package for that exact outpoint, send once it confirms.
                PreparedBtcFunding prep;
                try {
                    prep = io_.PrepareFundBtc(BtcHtlcAddressFor(session_.record, config_.btc_hrp),
                                              offer.btc_amount_sat);
                } catch (const std::exception& e) {
                    // Nothing was built or sent: back to Accepted, where Bob's
                    // checks run again next tick and cancel stays possible.
                    SwapSession back = session_;
                    back.record.state = SwapState::Accepted;
                    back.record.state_since_unix = now;
                    store_.Save(back);
                    session_ = std::move(back);
                    events.push_back(std::string("ALERT: could not prepare the BTC funding (retrying): ") + e.what());
                    return;
                }
                SwapSession next = session_;
                next.btc_funding_txid = prep.txid;
                next.btc_funding_vout = static_cast<int32_t>(prep.vout);
                next.btc_funding_raw = ToHex(prep.raw);
                store_.Save(next);
                session_ = std::move(next);
                io_.PinFunding("", prep.txid);
                const auto wire = FromHex(prep.txid);
                BtcFunding f;
                std::copy(wire.rbegin(), wire.rend(), f.txid.begin());
                f.vout = prep.vout;
                f.value_sat = offer.btc_amount_sat;
                ArmTowerWith(*din.funding, f, events);
                SendPreparedFunding(din, btc, now, events);
                return done("prepared BTC funding:", prep.txid);
            }
            const std::string txid =
                io_.FundBtc(BtcHtlcAddressFor(session_.record, config_.btc_hrp), offer.btc_amount_sat);
            PinAndSave("", txid);  // a decoy paying the same script is not Bob's lock
            return done("funded BTC HTLC:", txid);
        }
        case ActionKind::ClaimBtc: {
            if (!btc.funding) throw std::runtime_error("BTC HTLC output unknown");
            const auto raw = SignedBtcClaim(session_, keys_, *btc.funding, BtcFeeNow(now));
            // Possibly public from the moment it is handed to the node: record that
            // first, so a crash or lost reply never reads as "the secret never left".
            const bool was_seen = session_.record.claim_seen;
            if (!was_seen) SetClaimSeen(true);
            try {
                return done("broadcast BTC claim:", io_.BroadcastBtc(raw));
            } catch (const std::runtime_error& e) {
                const std::string why = e.what();
                const bool ambiguous = why.find("RPC failed") != std::string::npos ||
                                       why.find("already") != std::string::npos;
                if (!was_seen && !ambiguous) SetClaimSeen(false);  // the node refused it: it never left
                throw;
            }
        }
        case ActionKind::RefundBtc:
            if (!btc.funding) throw std::runtime_error("BTC HTLC output unknown");
            return done("broadcast BTC refund:",
                        io_.BroadcastBtc(SignedBtcRefund(session_, keys_, *btc.funding, BtcFeeNow(now))));
        case ActionKind::ClaimDin: {
            if (!din.funding) throw std::runtime_error("DIN HTLC output unknown");
            std::vector<uint8_t> raw;
            const auto armed_btc = ArmedBtcFunding(btc);
            if (session_.tower_armed && armed_btc && session_.din_sweep_pubkey != Bytes32{}) {
                // Send the very rung the tower would (its children are signed
                // against these txids), chosen by the same urgency rule.
                const auto package = MyTowerPackage(*din.funding, *armed_btc);
                size_t i = IndexByUrgency(package.din_claims.size(), offer.t_din_unix, din.mtp_unix, TowerConfig{});
                for (size_t k = 0; k < package.din_claims.size(); ++k) {  // once sent, always that one
                    if (TxId::Compute(package.din_claims[k].tx).AsUint256().GetHex() == session_.din_claim_txid) i = k;
                }
                Transaction tx = package.din_claims[i].tx;
                const auto terms = MakeDinTerms(offer, session_.record.accept);
                if (!session_.record.secret) throw std::runtime_error("the secret is not known");
                SetDinClaimWitness(tx, terms, BuildDinHtlc(terms), package.din_claims[i].signature,
                                   std::vector<uint8_t>(session_.record.secret->begin(), session_.record.secret->end()));
                raw = tx.Serialize(TxSerializationMode::WithWitness);
            } else {
                // Once sent, re-send that very claim (same fee, same txid): Dinero
                // keeps the first seen, and a CPFP child can only spend that one.
                const bool sent = !session_.din_claim_txid.empty() && session_.din_claim_value > 0 &&
                                  session_.din_claim_value < offer.din_amount_una;
                raw = SignedDinClaim(session_, keys_, *din.funding,
                                     sent ? offer.din_amount_una - session_.din_claim_value
                                          : DinFeeByUrgency(din.mtp_unix));
            }
            if (session_.din_claim_txid.empty()) {
                // Dinero keeps the first seen: this is the claim a CPFP child must
                // spend. Saved BEFORE sending, so a lost reply cannot lose it.
                Transaction tx;
                size_t used = 0;
                TransactionSerializer::Deserialize(tx, raw, used);
                SwapSession next = session_;
                next.din_claim_txid = TxId::Compute(tx).AsUint256().GetHex();
                next.din_claim_value = tx.vout.at(0).value.GetUna();
                store_.Save(next);
                session_ = std::move(next);
            }
            return done("broadcast DIN claim:", io_.BroadcastDin(raw));
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
