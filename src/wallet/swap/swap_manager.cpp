#include "wallet/swap/swap_manager.h"

#include "bech32/bech32.hpp"
#include "crypto/sha256.h"
#include "crypto/secure_random.h"
#include "wallet/swap/swap_crypto.h"
#include "wallet/swap/tower.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace dinero::swap {
namespace {

namespace fs = std::filesystem;
using namespace detail;

constexpr uint32_t kScanMargin = 6;  // start BTC scans a few blocks back (reorg slack)
constexpr size_t kKeepEvents = 12;

bool Terminal(SwapState s) {
    return s == SwapState::Done || s == SwapState::Refunded || s == SwapState::Aborted || s == SwapState::Lost;
}

// Bob's DIN still at his swap key: done on chain, not yet in the wallet.
bool SweepPending(const SwapSession& s) {
    return s.record.role == Role::BtcSeller && s.record.state == SwapState::Done &&
           s.din_sweep_pubkey != Bytes32{} && !s.din_swept;
}

bool Finished(const SwapSession& s) { return Terminal(s.record.state) && !SweepPending(s); }

Bytes32 Derive(const KeyDeriver& derive, const std::vector<uint32_t>& path) {
    const auto k = derive(path);
    if (!k) throw std::runtime_error("wallet locked: unlock it to use swaps");
    return *k;
}

std::map<std::string, std::string> ParseKeyValues(const std::string& text) {
    std::istringstream in(text);
    std::map<std::string, std::string> kv;
    for (std::string line; std::getline(in, line);) {
        const auto eq = line.find('=');
        if (eq != std::string::npos) kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return kv;
}

uint32_t ParseIndex(const std::string& name, const std::string& prefix, const std::string& ext) {
    if (name.size() <= prefix.size() + ext.size() || name.compare(0, prefix.size(), prefix) != 0 ||
        name.compare(name.size() - ext.size(), ext.size(), ext) != 0) {
        return UINT32_MAX;
    }
    const std::string digits = name.substr(prefix.size(), name.size() - prefix.size() - ext.size());
    if (digits.empty() || digits.size() > 9 || digits.find_first_not_of("0123456789") != std::string::npos) {
        return UINT32_MAX;
    }
    return static_cast<uint32_t>(std::stoul(digits));
}

// Every network: fees must stay small next to the amounts, so a claim or
// refund can always be built. Mainnet: at least 3 BTC confirmations before
// Alice reveals the secret.
void RequireSane(const SwapManagerConfig& c, uint64_t din_una, uint64_t btc_sat, uint32_t n_btc) {
    const uint64_t din_fee = std::max(c.runner.din_fee_una, c.runner.din_fee_urgent_una);
    if (din_una < 100 * din_fee) {
        throw std::invalid_argument("DIN amount too small for the swap fees (minimum " + std::to_string(100 * din_fee) +
                                    " una)");
    }
    if (btc_sat < 20 * c.runner.btc_fee_sat) {
        throw std::invalid_argument("BTC amount too small for the swap fees (minimum " +
                                    std::to_string(20 * c.runner.btc_fee_sat) + " sat)");
    }
    if (c.network == SwapNetwork::Mainnet && n_btc < 3) {
        throw std::invalid_argument("mainnet swaps need at least 3 BTC confirmations");
    }
}

void RequireWithinCaps(const SwapManagerConfig& c, uint64_t din_una, uint64_t btc_sat) {
    if (c.max_btc_sat && btc_sat > c.max_btc_sat) {
        throw std::invalid_argument("swap exceeds this node's BTC limit (" + std::to_string(c.max_btc_sat) +
                                    " sat per swap during the beta)");
    }
    if (c.max_din_una && din_una > c.max_din_una) {
        throw std::invalid_argument("swap exceeds this node's DIN limit (" + std::to_string(c.max_din_una) +
                                    " una per swap during the beta)");
    }
}

}  // namespace

// ---- Keys -------------------------------------------------------------------

Bytes32 SwapStoreKeyFromSeed(const KeyDeriver& derive, SwapNetwork network) {
    return DeriveSwapStoreKey(Derive(derive, {kSwapKeyPurpose, uint32_t(network), 0}));
}

SwapKeyMaterial SwapKeysForIndex(const KeyDeriver& derive, SwapNetwork network, uint32_t index) {
    if (index >= 0x80000000U) throw std::invalid_argument("swap index out of range");
    SwapKeyMaterial m;
    m.keys.din_secret_key = Derive(derive, {kSwapKeyPurpose, uint32_t(network), 1, index});
    m.keys.btc_secret_key = Derive(derive, {kSwapKeyPurpose, uint32_t(network), 2, index});
    m.keys.din_sweep_secret_key = Derive(derive, {kSwapKeyPurpose, uint32_t(network), 3, index});
    XOnlyOf(m.keys.din_secret_key);  // throws on an invalid scalar (negligible probability)
    CompressedOf(m.keys.btc_secret_key);
    return m;
}

std::vector<uint8_t> PayoutScriptFromAddress(const std::string& address, const std::string& hrp) {
    const auto r = bech32::Decode(hrp, address);
    if (!r) throw std::invalid_argument("not a " + hrp + " segwit address: " + address);
    const size_t n = r->program.size();
    if (r->witver == 0 && r->encoding == bech32::Encoding::BECH32 && (n == 20 || n == 32)) {
        std::vector<uint8_t> spk{0x00, static_cast<uint8_t>(n)};
        spk.insert(spk.end(), r->program.begin(), r->program.end());
        return spk;
    }
    if (r->witver == 1 && r->encoding == bech32::Encoding::BECH32M && n == 32) {
        std::vector<uint8_t> spk{0x51, 0x20};
        spk.insert(spk.end(), r->program.begin(), r->program.end());
        return spk;
    }
    throw std::invalid_argument("unsupported payout address (P2WPKH, P2WSH or P2TR only): " + address);
}

std::string SwapId(const SwapOffer& offer) {
    const auto id = OfferId(offer);
    return ToHex(std::vector<uint8_t>(id.begin(), id.begin() + 8));
}

// ---- Manager ----------------------------------------------------------------

SwapManager::SwapManager(SwapManagerConfig config, KeyDeriver derive, DinRpc din, BtcRpc btc)
    : config_(std::move(config)), derive_(std::move(derive)), din_(std::move(din)), btc_(std::move(btc)) {}

void SwapManager::SetTowerSink(std::function<void(const std::string&)> sink) {
    std::lock_guard<std::mutex> lock(mu_);
    tower_sink_ = std::move(sink);
}

void SwapManager::SetTowerAck(std::function<bool(const std::string&, const std::string&)> ack) {
    std::lock_guard<std::mutex> lock(mu_);
    tower_ack_ = std::move(ack);
}

Bytes32 SwapManager::StoreKey() {
    try {
        store_key_ = SwapStoreKeyFromSeed(derive_, config_.network);
    } catch (const std::runtime_error&) {
        if (!store_key_) throw;  // locked and never unlocked since start: paused
    }
    return *store_key_;
}

std::string SwapManager::SwapPath(uint32_t i) const { return config_.dir + "/swap-" + std::to_string(i) + ".swap"; }
std::string SwapManager::OfferPath(uint32_t i) const { return config_.dir + "/offer-" + std::to_string(i) + ".offer"; }

std::map<uint32_t, std::string> SwapManager::ScanDir(const std::string& ext) const {
    std::map<uint32_t, std::string> out;
    std::error_code ec;
    if (!fs::is_directory(config_.dir, ec)) return out;
    const std::string prefix = ext == ".swap" ? "swap-" : "offer-";
    for (const auto& e : fs::directory_iterator(config_.dir, ec)) {
        const uint32_t i = ParseIndex(e.path().filename().string(), prefix, ext);
        if (i != UINT32_MAX) out[i] = e.path().string();
    }
    return out;
}

uint32_t SwapManager::AllocateIndex() {
    fs::create_directories(config_.dir);
    fs::permissions(config_.dir, fs::perms::owner_all, fs::perm_options::replace);
    uint32_t next = 0;
    if (std::ifstream in(config_.dir + "/next_index"); in) {
        std::string t;
        in >> t;
        if (!t.empty() && t.size() <= 9 && t.find_first_not_of("0123456789") == std::string::npos) next = std::stoul(t);
    }
    // Never below anything on disk: a restored older backup cannot roll it back.
    for (const char* ext : {".swap", ".offer"}) {
        const auto files = ScanDir(ext);
        if (!files.empty()) next = std::max(next, files.rbegin()->first + 1);
    }
    if (fs::exists(SwapPath(next)) || fs::exists(OfferPath(next))) throw std::runtime_error("swap index already in use");
    WriteFileAtomically(config_.dir + "/next_index", std::to_string(next + 1) + "\n");
    return next;
}

SwapSession SwapManager::LoadSession(uint32_t index, const Bytes32& store_key) const {
    return EncryptedFileSwapStore::Load(SwapPath(index), store_key);
}

void SwapManager::StartSession(uint32_t index, SwapSession session, const Bytes32& store_key) {
    const auto mat = SwapKeysForIndex(derive_, config_.network, index);
    RunnerConfig rc = config_.runner;
    rc.use_tower = static_cast<bool>(tower_sink_);
    rc.require_tower = config_.require_tower_for_bob;
    Live live;
    live.store = std::make_unique<EncryptedFileSwapStore>(SwapPath(index), store_key);
    live.io = std::make_unique<RpcSwapChainIo>(din_, btc_, session, rc);
    if (tower_sink_) live.io->SetTowerSink(tower_sink_);
    if (tower_ack_) live.io->SetTowerAck(tower_ack_);
    live.runner = std::make_unique<SwapRunner>(std::move(session), mat.keys, rc, *live.io, *live.store);
    live_[index] = std::move(live);
}

std::string SwapManager::MakeOffer(const OfferRequest& r, uint32_t now) {
    std::lock_guard<std::mutex> lock(mu_);
    StoreKey();  // refuses a locked wallet before using an index
    if (r.din_lock_hours < r.btc_lock_hours + 24) throw std::invalid_argument("DIN lock must be >= BTC lock + 24 h");
    RequireWithinCaps(config_, r.din_amount_una, r.btc_amount_sat);
    RequireSane(config_, r.din_amount_una, r.btc_amount_sat, r.n_btc_confirmations);
    const auto din_payout = PayoutScriptFromAddress(r.din_refund_address, config_.runner.din_hrp);
    const auto btc_payout = PayoutScriptFromAddress(r.btc_claim_address, config_.runner.btc_hrp);
    // The cached store key outlives a relock: ask the wallet itself, so a
    // locked wallet is refused before an index is used up.
    SwapStoreKeyFromSeed(derive_, config_.network);
    const uint32_t index = AllocateIndex();
    const auto mat = SwapKeysForIndex(derive_, config_.network, index);
    // Fresh per offer, never derived from the index: see the header.
    const auto random = secure_random_bytes(32);
    Bytes32 secret{};
    std::copy(random.begin(), random.end(), secret.begin());

    SwapOffer o;
    o.network = config_.network;
    o.din_amount_una = r.din_amount_una;
    o.btc_amount_sat = r.btc_amount_sat;
    crypto::CSHA256().Write(secret.data(), secret.size()).Finalize(o.payment_hash.data());
    o.din_refund_pubkey = XOnlyOf(mat.keys.din_secret_key);
    o.btc_claim_pubkey = CompressedOf(mat.keys.btc_secret_key);
    o.t_btc_unix = now + r.btc_lock_hours * 3600;
    o.t_din_unix = now + r.din_lock_hours * 3600;
    o.expires_unix = now + r.expires_minutes * 60;
    o.n_din_confirmations = r.n_din_confirmations;
    o.n_btc_confirmations = r.n_btc_confirmations;
    const std::string text = EncodeOffer(o);  // throws on any rule violation

    std::ostringstream f;
    f << "offer=" << text << "\ndin_payout=" << ToHex(din_payout) << "\nbtc_payout=" << ToHex(btc_payout) << "\n";
    f << "secret=" << ToHex(std::vector<uint8_t>(secret.begin(), secret.end())) << "\n";
    SealToFile(OfferPath(index), StoreKey(), f.str());
    WriteFileAtomically(OfferPath(index) + ".id", SwapId(o) + "\n");  // public: readable while locked
    return text;
}

SwapManager::AcceptResult SwapManager::Accept(const std::string& text, const std::string& din_payout_address,
                                              const std::string& btc_refund_address, uint32_t now) {
    std::lock_guard<std::mutex> lock(mu_);
    const Bytes32 store_key = StoreKey();
    const auto btc_tip = btc_("getblockcount", Json::Value(Json::arrayValue));
    if (!btc_tip || !btc_tip->isNumeric()) throw std::runtime_error("Bitcoin node unreachable (swap.btc_rpc_*)");
    const uint32_t scan_from = btc_tip->asUInt() > kScanMargin ? btc_tip->asUInt() - kScanMargin : 0;
    const auto din_tip = din_("getblockcount", Json::Value(Json::arrayValue));
    if (!din_tip || !din_tip->isNumeric()) throw std::runtime_error("Dinero node unreachable");
    const uint32_t din_scan_from = din_tip->asUInt() > kScanMargin ? din_tip->asUInt() - kScanMargin : 0;

    auto already_started = [&](const std::string& id) {
        for (const auto& [i, path] : ScanDir(".swap")) {
            if (live_.count(i) && SwapId(live_[i].runner->session().record.offer) == id) return true;
            try {
                if (SwapId(LoadSession(i, store_key).record.offer) == id) return true;
            } catch (const std::exception&) {
            }
        }
        return false;
    };

    if (text.rfind(kOfferPrefix, 0) == 0) {  // Bob accepts Alice's offer
        const SwapOffer offer = DecodeOffer(text);
        if (offer.network != config_.network) throw std::invalid_argument("offer is for another network");
        RequireAcceptableNow(offer, now);
        if (config_.require_tower_for_bob && (!tower_sink_ || !tower_ack_)) {
            throw std::runtime_error("mainnet beta: buying DIN needs a watchtower (set swap.tower_inbox and run "
                                     "dinero-swap-tower)");
        }
        RequireWithinCaps(config_, offer.din_amount_una, offer.btc_amount_sat);
        RequireSane(config_, offer.din_amount_una, offer.btc_amount_sat, offer.n_btc_confirmations);
        const std::string id = SwapId(offer);
        if (already_started(id)) throw std::runtime_error("swap " + id + " already exists");
        SwapSession s;
        s.din_payout_script = PayoutScriptFromAddress(din_payout_address, config_.runner.din_hrp);
        s.btc_payout_script = PayoutScriptFromAddress(btc_refund_address, config_.runner.btc_hrp);
        SwapStoreKeyFromSeed(derive_, config_.network);  // a relocked wallet: refused before an index is used
        const uint32_t index = AllocateIndex();
        const auto mat = SwapKeysForIndex(derive_, config_.network, index);
        s.record.role = Role::BtcSeller;
        s.record.offer = offer;
        s.record.accept.offer_id = OfferId(offer);
        s.record.accept.din_claim_pubkey = XOnlyOf(mat.keys.din_secret_key);
        s.record.accept.btc_refund_pubkey = CompressedOf(mat.keys.btc_secret_key);
        s.din_sweep_pubkey = XOnlyOf(mat.keys.din_sweep_secret_key);  // CPFP: the claim pays here
        s.record.state = SwapState::Accepted;
        s.record.state_since_unix = now;
        s.btc_scan_from_height = scan_from;
        s.din_scan_from_height = din_scan_from;
        const std::string accept_text = EncodeAccept(s.record.accept);
        EncryptedFileSwapStore(SwapPath(index), store_key).Save(s);
        WriteFileAtomically(SwapPath(index) + ".id", id + "\n");  // public: readable while locked
        StartSession(index, std::move(s), store_key);
        return {id, accept_text};
    }
    if (text.rfind(kAcceptPrefix, 0) == 0) {  // Alice receives Bob's accept
        const SwapAccept accept = DecodeAccept(text);
        for (const auto& [index, path] : ScanDir(".offer")) {
            auto kv = ParseKeyValues(OpenSealedFile(path, store_key));
            const SwapOffer offer = DecodeOffer(kv["offer"]);
            if (OfferId(offer) != accept.offer_id) continue;
            const std::string id = SwapId(offer);
            if (already_started(id)) throw std::runtime_error("swap " + id + " already started");
            const auto mat = SwapKeysForIndex(derive_, config_.network, index);
            SwapSession s;
            s.record.role = Role::DinSeller;
            s.record.offer = offer;
            s.record.accept = accept;
            const auto secret_bytes = FromHex(kv["secret"]);
            if (secret_bytes.size() != 32) throw std::runtime_error("pending offer file has no secret");
            Bytes32 secret{};
            std::copy(secret_bytes.begin(), secret_bytes.end(), secret.begin());
            s.record.secret = secret;
            s.record.state = SwapState::Accepted;
            s.record.state_since_unix = now;
            s.btc_scan_from_height = scan_from;
            s.din_scan_from_height = din_scan_from;
            s.din_payout_script = FromHex(kv["din_payout"]);
            s.btc_payout_script = FromHex(kv["btc_payout"]);
            MakeDinTerms(offer, accept);  // throws on a mismatched accept
            EncryptedFileSwapStore(SwapPath(index), store_key).Save(s);
            WriteFileAtomically(SwapPath(index) + ".id", id + "\n");  // public: readable while locked
            fs::remove(path);
            fs::remove(path + ".id");
            StartSession(index, std::move(s), store_key);
            return {id, std::nullopt};
        }
        bool started = already_started([&] {  // a repeated accept for a swap already running
            for (const auto& [i, live] : live_) {
                if (live.runner->session().record.accept.offer_id == accept.offer_id) {
                    return SwapId(live.runner->session().record.offer);
                }
            }
            return std::string();
        }());
        throw std::runtime_error(started ? "swap already started" : "no pending offer matches this accept");
    }
    throw std::invalid_argument("expected a dinswap1o offer or a dinswap1a accept");
}

SwapSummary SwapManager::Summarize(uint32_t index, const SwapSession& s, const std::vector<std::string>& events) const {
    SwapSummary m;
    m.id = SwapId(s.record.offer);
    m.index = index;
    m.role = s.record.role;
    m.state = s.record.state;
    m.din_amount_una = s.record.offer.din_amount_una;
    m.btc_amount_sat = s.record.offer.btc_amount_sat;
    m.t_btc_unix = s.record.offer.t_btc_unix;
    m.t_din_unix = s.record.offer.t_din_unix;
    m.tower_armed = s.tower_armed;
    m.sweep_pending = SweepPending(s);
    m.last_events = events;
    return m;
}

std::vector<SwapSummary> SwapManager::List() {
    std::lock_guard<std::mutex> lock(mu_);
    std::optional<Bytes32> store_key;
    try {
        store_key = StoreKey();
    } catch (const std::runtime_error&) {
    }
    std::vector<SwapSummary> out;
    for (const auto& [index, path] : ScanDir(".swap")) {
        if (auto it = live_.find(index); it != live_.end()) {
            out.push_back(Summarize(index, it->second.runner->session(), it->second.last_events));
            continue;
        }
        if (!store_key) {
            SwapSummary m;
            m.index = index;
            m.id = "swap-" + std::to_string(index);
            if (std::ifstream in(path + ".id"); in) in >> m.id;
            m.wallet_locked = true;
            out.push_back(m);
            continue;
        }
        try {
            out.push_back(Summarize(index, LoadSession(index, *store_key), {}));
        } catch (const std::exception& e) {
            SwapSummary m;
            m.index = index;
            m.id = "swap-" + std::to_string(index);
            m.last_events = {std::string("unreadable: ") + e.what()};
            out.push_back(m);
        }
    }
    for (const auto& [index, path] : ScanDir(".offer")) {
        if (!store_key) {
            SwapSummary m;
            m.index = index;
            m.id = "offer-" + std::to_string(index);
            if (std::ifstream in(path + ".id"); in) in >> m.id;
            m.role = Role::DinSeller;
            m.pending_accept = true;
            m.wallet_locked = true;
            out.push_back(m);
            continue;
        }
        try {
            const SwapOffer o = DecodeOffer(ParseKeyValues(OpenSealedFile(path, *store_key))["offer"]);
            SwapSummary m;
            m.id = SwapId(o);
            m.index = index;
            m.role = Role::DinSeller;
            m.din_amount_una = o.din_amount_una;
            m.btc_amount_sat = o.btc_amount_sat;
            m.t_btc_unix = o.t_btc_unix;
            m.t_din_unix = o.t_din_unix;
            m.pending_accept = true;
            out.push_back(m);
        } catch (const std::exception&) {
        }
    }
    return out;
}

SwapSummary SwapManager::Status(const std::string& id) {
    for (auto& s : List()) {
        if (s.id == id) return s;
    }
    throw std::runtime_error("unknown swap " + id);
}

void SwapManager::Cancel(const std::string& id) {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& [index, path] : ScanDir(".offer")) {
        std::string offer_id;
        if (std::ifstream in(path + ".id"); in) in >> offer_id;
        if (offer_id == id) {
            fs::remove(path);  // nothing was locked for an unanswered offer
            fs::remove(path + ".id");
            return;
        }
    }
    const Bytes32 store_key = StoreKey();
    for (const auto& [index, path] : ScanDir(".swap")) {
        SwapSession s = live_.count(index) ? live_[index].runner->session() : LoadSession(index, store_key);
        if (SwapId(s.record.offer) != id) continue;
        // Bob's prepared funding that was never sent (the tower never confirmed)
        // locks nothing either.
        const bool unsent = s.record.state == SwapState::BtcLockBroadcast && !s.btc_funding_raw.empty();
        if (s.record.state != SwapState::Accepted && !unsent) {
            throw std::runtime_error(std::string("cannot cancel in state ") + StateName(s.record.state) +
                                     ": funds may be locked; the swap will finish or refund");
        }
        if (unsent) {
            // A lost broadcast reply leaves the raw here although the BTC went
            // out: only Bitcoin Core can say nothing was sent.
            const auto raw = FromHex(s.btc_funding_raw);
            RpcSwapChainIo io(din_, btc_, s, config_.runner);
            const auto never_sent = io.PreparedFundingUnsent(raw);
            if (!never_sent || !*never_sent) {
                throw std::runtime_error(never_sent ? "the prepared BTC funding may have been sent; not cancelling — "
                                                      "the swap will finish or refund"
                                                    : "cannot reach Bitcoin Core to check the prepared BTC funding; "
                                                      "not cancelling");
            }
            try {
                io.ReleasePreparedFunding(raw);
            } catch (const std::exception&) {
                // the inputs stay locked in Core; harmless
            }
            s.btc_funding_raw.clear();
        }
        s.record.state = SwapState::Aborted;
        EncryptedFileSwapStore(SwapPath(index), store_key).Save(s);
        live_.erase(index);
        return;
    }
    throw std::runtime_error("unknown swap " + id);
}

std::string SwapManager::Refund(const std::string& id, uint32_t now) {
    std::lock_guard<std::mutex> lock(mu_);
    const Bytes32 store_key = StoreKey();
    for (const auto& [index, path] : ScanDir(".swap")) {
        if (!live_.count(index)) {
            SwapSession s = LoadSession(index, store_key);
            if (SwapId(s.record.offer) != id) continue;
            StartSession(index, std::move(s), store_key);  // finished swaps too: the caller asked
        }
        auto& live = live_[index];
        if (SwapId(live.runner->session().record.offer) != id) continue;
        const std::string txid = live.runner->ForceRefund(now);
        live.last_events.push_back("manual refund broadcast: " + txid);
        return txid;
    }
    throw std::runtime_error("unknown swap " + id);
}

bool SwapManager::TickAll(uint32_t now) {
    std::lock_guard<std::mutex> lock(mu_);
    Bytes32 store_key;
    try {
        store_key = StoreKey();  // remembered across a relock while the daemon runs
    } catch (const std::runtime_error&) {
        return false;  // locked since start: paused, not failed
    }
    for (const auto& [index, path] : ScanDir(".swap")) {
        if (live_.count(index)) continue;
        try {
            SwapSession s = LoadSession(index, store_key);
            if (!Finished(s)) StartSession(index, std::move(s), store_key);
        } catch (const std::exception&) {
            // unreadable or foreign file: List() reports it
        }
    }
    for (auto it = live_.begin(); it != live_.end();) {
        auto& live = it->second;
        try {
            const auto r = live.runner->Tick(now);
            for (const auto& e : r.events) live.last_events.push_back(e);
            if (r.before != r.after) {
                live.last_events.push_back(std::string(StateName(r.before)) + " -> " + StateName(r.after));
            }
        } catch (const std::exception& e) {
            live.last_events.push_back(std::string("tick failed: ") + e.what());
        }
        if (live.last_events.size() > kKeepEvents) {
            live.last_events.erase(live.last_events.begin(), live.last_events.end() - kKeepEvents);
        }
        it = Finished(live.runner->session()) ? live_.erase(it) : std::next(it);
    }
    return true;
}

}  // namespace dinero::swap

namespace dinero::swap {

BetaDecision BetaPolicy(SwapNetwork network, bool mainnet_beta_opt_in, uint64_t configured_max_btc_sat,
                        uint64_t configured_max_din_una) {
    BetaDecision d;
    if (network != SwapNetwork::Mainnet) return d;  // test networks: no caps
    if (!mainnet_beta_opt_in) {
        d.refusal = "mainnet swaps are a beta: set swap.mainnet_beta=1 to accept the risk (small amounts only)";
        return d;
    }
    auto clamp = [](uint64_t configured, uint64_t def, uint64_t ceiling) {
        return configured == 0 ? def : std::min(configured, ceiling);
    };
    d.max_btc_sat = clamp(configured_max_btc_sat, kBetaDefaultMaxBtcSat, kBetaHardMaxBtcSat);
    d.max_din_una = clamp(configured_max_din_una, kBetaDefaultMaxDinUna, kBetaHardMaxDinUna);
    return d;
}

}  // namespace dinero::swap

namespace dinero::swap {

std::optional<std::string> FeeConfigProblem(int64_t din_fee_una, int64_t din_fee_urgent_una, int64_t btc_fee_sat) {
    if (din_fee_una <= 0 || din_fee_urgent_una <= 0 || btc_fee_sat <= 0) return "swap fees must be positive";
    if (din_fee_urgent_una < din_fee_una) return "swap.din_fee_urgent_una must be at least swap.din_fee_una";
    if (din_fee_urgent_una > 10 * 100'000'000LL) return "swap.din_fee_urgent_una is above 10 DIN";
    if (btc_fee_sat > 100'000) return "swap.btc_fee_sat is above 0.001 BTC";
    return std::nullopt;
}

}  // namespace dinero::swap
