#include "wallet/swap/swap_manager.h"

#include "bech32/bech32.hpp"
#include "crypto/sha256.h"
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

Bytes32 Derive(const KeyDeriver& derive, const std::vector<uint32_t>& path) {
    const auto k = derive(path);
    if (!k) throw std::runtime_error("wallet locked: unlock it to use swaps");
    return *k;
}

std::map<std::string, std::string> ReadKeyValues(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("swap: cannot read " + path);
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
    XOnlyOf(m.keys.din_secret_key);  // throws on an invalid scalar (negligible probability)
    CompressedOf(m.keys.btc_secret_key);
    m.secret = HmacSha256(m.keys.din_secret_key, "dinero/swap-secret/v1");
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
    Live live;
    live.store = std::make_unique<EncryptedFileSwapStore>(SwapPath(index), store_key);
    live.io = std::make_unique<RpcSwapChainIo>(din_, btc_, session, rc);
    if (tower_sink_) live.io->SetTowerSink(tower_sink_);
    live.runner = std::make_unique<SwapRunner>(std::move(session), mat.keys, rc, *live.io, *live.store);
    live_[index] = std::move(live);
}

std::string SwapManager::MakeOffer(const OfferRequest& r, uint32_t now) {
    std::lock_guard<std::mutex> lock(mu_);
    SwapStoreKeyFromSeed(derive_, config_.network);  // refuses a locked wallet before using an index
    if (r.din_lock_hours < r.btc_lock_hours + 24) throw std::invalid_argument("DIN lock must be >= BTC lock + 24 h");
    const auto din_payout = PayoutScriptFromAddress(r.din_refund_address, config_.runner.din_hrp);
    const auto btc_payout = PayoutScriptFromAddress(r.btc_claim_address, config_.runner.btc_hrp);
    const uint32_t index = AllocateIndex();
    const auto mat = SwapKeysForIndex(derive_, config_.network, index);

    SwapOffer o;
    o.network = config_.network;
    o.din_amount_una = r.din_amount_una;
    o.btc_amount_sat = r.btc_amount_sat;
    crypto::CSHA256().Write(mat.secret.data(), mat.secret.size()).Finalize(o.payment_hash.data());
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
    WriteFileAtomically(OfferPath(index), f.str());
    return text;
}

SwapManager::AcceptResult SwapManager::Accept(const std::string& text, const std::string& din_payout_address,
                                              const std::string& btc_refund_address, uint32_t now) {
    std::lock_guard<std::mutex> lock(mu_);
    const Bytes32 store_key = SwapStoreKeyFromSeed(derive_, config_.network);
    const auto btc_tip = btc_("getblockcount", Json::Value(Json::arrayValue));
    if (!btc_tip || !btc_tip->isNumeric()) throw std::runtime_error("Bitcoin node unreachable (swap.btc_rpc_*)");
    const uint32_t scan_from = btc_tip->asUInt() > kScanMargin ? btc_tip->asUInt() - kScanMargin : 0;

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
        const std::string id = SwapId(offer);
        if (already_started(id)) throw std::runtime_error("swap " + id + " already exists");
        SwapSession s;
        s.din_payout_script = PayoutScriptFromAddress(din_payout_address, config_.runner.din_hrp);
        s.btc_payout_script = PayoutScriptFromAddress(btc_refund_address, config_.runner.btc_hrp);
        const uint32_t index = AllocateIndex();
        const auto mat = SwapKeysForIndex(derive_, config_.network, index);
        s.record.role = Role::BtcSeller;
        s.record.offer = offer;
        s.record.accept.offer_id = OfferId(offer);
        s.record.accept.din_claim_pubkey = XOnlyOf(mat.keys.din_secret_key);
        s.record.accept.btc_refund_pubkey = CompressedOf(mat.keys.btc_secret_key);
        s.record.state = SwapState::Accepted;
        s.record.state_since_unix = now;
        s.btc_scan_from_height = scan_from;
        const std::string accept_text = EncodeAccept(s.record.accept);
        EncryptedFileSwapStore(SwapPath(index), store_key).Save(s);
        WriteFileAtomically(SwapPath(index) + ".id", id + "\n");  // public: readable while locked
        StartSession(index, std::move(s), store_key);
        return {id, accept_text};
    }
    if (text.rfind(kAcceptPrefix, 0) == 0) {  // Alice receives Bob's accept
        const SwapAccept accept = DecodeAccept(text);
        for (const auto& [index, path] : ScanDir(".offer")) {
            auto kv = ReadKeyValues(path);
            const SwapOffer offer = DecodeOffer(kv["offer"]);
            if (OfferId(offer) != accept.offer_id) continue;
            const std::string id = SwapId(offer);
            if (already_started(id)) throw std::runtime_error("swap " + id + " already started");
            const auto mat = SwapKeysForIndex(derive_, config_.network, index);
            SwapSession s;
            s.record.role = Role::DinSeller;
            s.record.offer = offer;
            s.record.accept = accept;
            s.record.secret = mat.secret;
            s.record.state = SwapState::Accepted;
            s.record.state_since_unix = now;
            s.btc_scan_from_height = scan_from;
            s.din_payout_script = FromHex(kv["din_payout"]);
            s.btc_payout_script = FromHex(kv["btc_payout"]);
            MakeDinTerms(offer, accept);  // throws on a mismatched accept
            EncryptedFileSwapStore(SwapPath(index), store_key).Save(s);
            WriteFileAtomically(SwapPath(index) + ".id", id + "\n");  // public: readable while locked
            fs::remove(path);
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
    m.last_events = events;
    return m;
}

std::vector<SwapSummary> SwapManager::List() {
    std::lock_guard<std::mutex> lock(mu_);
    std::optional<Bytes32> store_key;
    try {
        store_key = SwapStoreKeyFromSeed(derive_, config_.network);
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
        try {
            const SwapOffer o = DecodeOffer(ReadKeyValues(path)["offer"]);
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
        if (SwapId(DecodeOffer(ReadKeyValues(path)["offer"])) == id) {
            fs::remove(path);  // nothing was locked for an unanswered offer
            return;
        }
    }
    const Bytes32 store_key = SwapStoreKeyFromSeed(derive_, config_.network);
    for (const auto& [index, path] : ScanDir(".swap")) {
        SwapSession s = live_.count(index) ? live_[index].runner->session() : LoadSession(index, store_key);
        if (SwapId(s.record.offer) != id) continue;
        if (s.record.state != SwapState::Accepted) {
            throw std::runtime_error(std::string("cannot cancel in state ") + StateName(s.record.state) +
                                     ": funds may be locked; the swap will finish or refund");
        }
        s.record.state = SwapState::Aborted;
        EncryptedFileSwapStore(SwapPath(index), store_key).Save(s);
        live_.erase(index);
        return;
    }
    throw std::runtime_error("unknown swap " + id);
}

bool SwapManager::TickAll(uint32_t now) {
    std::lock_guard<std::mutex> lock(mu_);
    Bytes32 store_key;
    try {
        store_key = SwapStoreKeyFromSeed(derive_, config_.network);
    } catch (const std::runtime_error&) {
        return false;  // paused, not failed
    }
    for (const auto& [index, path] : ScanDir(".swap")) {
        if (live_.count(index)) continue;
        try {
            SwapSession s = LoadSession(index, store_key);
            if (!Terminal(s.record.state)) StartSession(index, std::move(s), store_key);
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
        it = Terminal(live.runner->session().record.state) ? live_.erase(it) : std::next(it);
    }
    return true;
}

}  // namespace dinero::swap
