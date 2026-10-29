#include "wallet/swap/swap_manager.h"

#include "bech32/bech32.hpp"
#include "crypto/sha256.h"
#include "crypto/secure_random.h"
#include "wallet/swap/swap_crypto.h"
#include "wallet/swap/tower.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace dinero::swap {
namespace {

namespace fs = std::filesystem;
using namespace detail;

constexpr uint32_t kScanMargin = 6;  // start BTC scans a few blocks back (reorg slack)
constexpr size_t kKeepEvents = 12;
// Set aside per promised DIN funding on top of its amount: a bound on the
// wallet's own fee for the one HTLC funding transaction (0.01 DIN).
constexpr uint64_t kDinFundingFeeAllowanceUna = 1'000'000;

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

// Zcash's counterpart of RequireSane's BTC rule: the amount covers its fees.
void RequireSaneZec(uint64_t zat, uint64_t spend_fee_zat) {
    if (zat < 20 * spend_fee_zat) {
        throw std::invalid_argument("ZEC amount too small for the swap fees (minimum " + std::to_string(20 * spend_fee_zat) +
                                    " zat)");
    }
}

std::string ZecAmount(uint64_t zat) {
    char b[32];
    std::snprintf(b, sizeof b, "%llu.%08llu", static_cast<unsigned long long>(zat / 100'000'000),
                  static_cast<unsigned long long>(zat % 100'000'000));
    std::string s = b;
    while (s.back() == '0') s.pop_back();
    if (s.back() == '.') s.pop_back();
    return s;
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

SwapKeyMaterial SwapKeysForIndex(const KeyDeriver& derive, SwapNetwork network, uint32_t index, CounterChain chain) {
    SwapKeyMaterial m = SwapKeysForIndex(derive, network, index);
    if (chain == CounterChain::Zcash) {
        m.keys.btc_secret_key = Derive(derive, {kSwapKeyPurpose, uint32_t(network), 5, index});
        CompressedOf(m.keys.btc_secret_key);
    }
    return m;
}

Bytes32 ZecDepositKey(const KeyDeriver& derive, SwapNetwork network, uint32_t index) {
    if (index >= 0x80000000U) throw std::invalid_argument("swap index out of range");
    const auto k = Derive(derive, {kSwapKeyPurpose, uint32_t(network), 6, index});
    CompressedOf(k);
    return k;
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
std::string SwapManager::DepositPath(uint32_t i) const {
    return config_.dir + "/deposit-" + std::to_string(i) + ".deposit";
}

void SwapManager::SetZecSupport(ZecSupport zec) {
    std::lock_guard<std::mutex> lock(mu_);
    zec_ = std::move(zec);
}

bool SwapManager::SupportsZec() const { return zec_.has_value() && config_.network != SwapNetwork::Mainnet; }

void SwapManager::RequireZec() const {
    if (!zec_) throw std::runtime_error("this dinerod was built without Zcash swaps (DINERO_SWAP_ZEC)");
    if (config_.network == SwapNetwork::Mainnet) {
        throw std::runtime_error("Zcash swaps are not enabled on mainnet yet (testnet beta first)");
    }
}

std::map<uint32_t, std::string> SwapManager::ScanDir(const std::string& ext) const {
    std::map<uint32_t, std::string> out;
    std::error_code ec;
    if (!fs::is_directory(config_.dir, ec)) return out;
    const std::string prefix = ext == ".swap" ? "swap-" : ext == ".offer" ? "offer-" : "deposit-";
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
    for (const char* ext : {".swap", ".offer", ".deposit"}) {
        const auto files = ScanDir(ext);
        if (!files.empty()) next = std::max(next, files.rbegin()->first + 1);
    }
    if (fs::exists(SwapPath(next)) || fs::exists(OfferPath(next)) || fs::exists(DepositPath(next))) {
        throw std::runtime_error("swap index already in use");
    }
    WriteFileAtomically(config_.dir + "/next_index", std::to_string(next + 1) + "\n");
    return next;
}

SwapSession SwapManager::LoadSession(uint32_t index, const Bytes32& store_key) const {
    return EncryptedFileSwapStore::Load(SwapPath(index), store_key);
}

uint64_t SwapManager::CommittedDinUna(const Bytes32& store_key) {
    uint64_t total = 0;
    // Pending offers stay committed until cancelled: Alice's accept does not
    // check the offer's expiry, so an expired offer can still start a swap.
    for (const auto& [index, path] : ScanDir(".offer")) {
        try {
            const SwapOffer o = DecodeOffer(ParseKeyValues(OpenSealedFile(path, store_key))["offer"]);
            total += o.din_amount_una + kDinFundingFeeAllowanceUna;
        } catch (const std::exception&) {
        }
    }
    for (const auto& [index, path] : ScanDir(".swap")) {
        try {
            const SwapSession s = live_.count(index) ? live_[index].runner->session() : LoadSession(index, store_key);
            const bool unfunded =
                s.record.state == SwapState::Accepted || s.record.state == SwapState::DinLockBroadcast;
            if (s.record.role == Role::DinSeller && unfunded) {
                total += s.record.offer.din_amount_una + kDinFundingFeeAllowanceUna;
            }
        } catch (const std::exception&) {
        }
    }
    return total;
}

void SwapManager::RequireDinBalanceFor(uint64_t din_amount_una, const Bytes32& store_key) {
    const auto bal = din_("wallet.getbalance", Json::Value(Json::objectValue));
    if (!bal || !bal->isObject() || !(*bal)["spendable"].isNumeric()) {
        throw std::runtime_error("cannot read the wallet balance");
    }
    const double spendable_din = (*bal)["spendable"].asDouble();  // DIN, not una
    if (!std::isfinite(spendable_din) || spendable_din < 0) throw std::runtime_error("cannot read the wallet balance");
    const uint64_t spendable = static_cast<uint64_t>(std::llround(spendable_din * 1e8));
    const uint64_t committed = CommittedDinUna(store_key);
    const uint64_t needed = din_amount_una + kDinFundingFeeAllowanceUna;
    if (committed > spendable || needed > spendable - committed) {
        throw std::invalid_argument("offer exceeds available balance (" + std::to_string(spendable) +
                                    " una spendable, " + std::to_string(committed) +
                                    " una already committed to open offers and swaps; this offer needs " +
                                    std::to_string(needed) + " una)");
    }
}

void SwapManager::StartSession(uint32_t index, SwapSession session, const Bytes32& store_key) {
    const CounterChain chain = session.record.offer.chain;
    const auto mat = SwapKeysForIndex(derive_, config_.network, index, chain);
    RunnerConfig rc = config_.runner;
    rc.use_tower = static_cast<bool>(tower_sink_);
    rc.require_tower = config_.require_tower_for_bob;
    Live live;
    live.store = std::make_unique<EncryptedFileSwapStore>(SwapPath(index), store_key);
    if (chain == CounterChain::Zcash) {
        RequireZec();
        const Bytes32 deposit_key =
            session.record.role == Role::BtcSeller ? ZecDepositKey(derive_, config_.network, index) : Bytes32{};
        live.io = zec_->make_io(session, rc, deposit_key);
    } else {
        live.io = std::make_unique<RpcSwapChainIo>(din_, btc_, session, rc);
    }
    if (tower_sink_) live.io->SetTowerSink(tower_sink_);
    if (tower_ack_) live.io->SetTowerAck(tower_ack_);
    live.runner = std::make_unique<SwapRunner>(std::move(session), mat.keys, rc, *live.io, *live.store);
    live_[index] = std::move(live);
}

std::string SwapManager::MakeOffer(const OfferRequest& r, uint32_t now) {
    std::lock_guard<std::mutex> lock(mu_);
    StoreKey();  // refuses a locked wallet before using an index
    if (r.din_lock_hours < r.btc_lock_hours + 24) throw std::invalid_argument("DIN lock must be >= BTC lock + 24 h");
    const bool zec = r.chain == CounterChain::Zcash;
    if (zec) {
        RequireZec();
        RequireSaneZec(r.btc_amount_sat, zec_->spend_fee_zat);
        RequireSane(config_, r.din_amount_una, 20 * config_.runner.btc_fee_sat, r.n_btc_confirmations);  // the DIN rule
    } else {
        RequireWithinCaps(config_, r.din_amount_una, r.btc_amount_sat);
        RequireSane(config_, r.din_amount_una, r.btc_amount_sat, r.n_btc_confirmations);
    }
    RequireDinBalanceFor(r.din_amount_una, StoreKey());
    const auto din_payout = PayoutScriptFromAddress(r.din_refund_address, config_.runner.din_hrp);
    const auto btc_payout = zec ? zec_->payout_script(r.btc_claim_address)
                                : PayoutScriptFromAddress(r.btc_claim_address, config_.runner.btc_hrp);
    // The cached store key outlives a relock: ask the wallet itself, so a
    // locked wallet is refused before an index is used up.
    SwapStoreKeyFromSeed(derive_, config_.network);
    const uint32_t index = AllocateIndex();
    const auto mat = SwapKeysForIndex(derive_, config_.network, index, r.chain);
    // Fresh per offer, never derived from the index: see the header.
    const auto random = secure_random_bytes(32);
    Bytes32 secret{};
    std::copy(random.begin(), random.end(), secret.begin());

    SwapOffer o;
    o.network = config_.network;
    o.chain = r.chain;
    o.din_amount_una = r.din_amount_una;
    o.btc_amount_sat = r.btc_amount_sat;
    crypto::CSHA256().Write(secret.data(), secret.size()).Finalize(o.payment_hash.data());
    o.din_refund_pubkey = XOnlyOf(mat.keys.din_secret_key);
    o.btc_claim_pubkey = CompressedOf(mat.keys.btc_secret_key);
    o.t_btc_unix = now + r.btc_lock_hours * 3600;
    o.t_din_unix = now + r.din_lock_hours * 3600;
    o.expires_unix = now + r.expires_minutes * 60;
    o.n_din_confirmations = r.n_din_confirmations;
    o.n_btc_confirmations = zec ? std::max(r.n_btc_confirmations, kMinZecConfirmations) : r.n_btc_confirmations;
    const std::string text = EncodeOffer(o);  // throws on any rule violation
    if (zec) zec_->require_no_upgrade(o, now);

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
    // Where the counter-chain watcher starts: a few blocks below the tip.
    auto counter_scan_from = [&](CounterChain chain) -> uint32_t {
        uint32_t tip = 0;
        if (chain == CounterChain::Zcash) {
            RequireZec();
            tip = zec_->tip_height();
        } else {
            const auto btc_tip = btc_("getblockcount", Json::Value(Json::arrayValue));
            if (!btc_tip || !btc_tip->isNumeric()) throw std::runtime_error("Bitcoin node unreachable (swap.btc_rpc_*)");
            tip = btc_tip->asUInt();
        }
        return tip > kScanMargin ? tip - kScanMargin : 0;
    };
    const bool zec_text = text.rfind(kOffer2Prefix, 0) == 0;
    const uint32_t scan_from = zec_text || text.rfind(kAcceptPrefix, 0) == 0 ? 0 : counter_scan_from(CounterChain::Bitcoin);
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
    if (zec_text) {  // Bob accepts Alice's Zcash offer, from the deposit he paid (PrepareDeposit)
        const SwapOffer offer = DecodeOffer(text);
        if (offer.network != config_.network) throw std::invalid_argument("offer is for another network");
        RequireZec();
        RequireAcceptableNow(offer, now);
        zec_->require_no_upgrade(offer, now);
        RequireSaneZec(offer.btc_amount_sat, zec_->spend_fee_zat);
        RequireSane(config_, offer.din_amount_una, 20 * config_.runner.btc_fee_sat, offer.n_btc_confirmations);
        const std::string id = SwapId(offer);
        if (already_started(id)) throw std::runtime_error("swap " + id + " already exists");
        std::optional<uint32_t> index;
        for (const auto& [i, path] : ScanDir(".deposit")) {
            std::string did;
            if (std::ifstream in(path + ".id"); in) in >> did;
            if (did == id) index = i;
        }
        if (!index) throw std::runtime_error("pay the deposit first: swap.deposit gives its address");
        auto kv = ParseKeyValues(OpenSealedFile(DepositPath(*index), store_key));
        const auto refund = FromHex(kv["refund"]);
        if (refund.empty()) throw std::runtime_error("deposit file has no refund address");
        const auto status = zec_->deposit_status(ZecDepositKey(derive_, config_.network, *index));
        const uint64_t due = offer.btc_amount_sat + zec_->funding_fee_zat;
        if (status.confirmed_zat < due) {
            throw std::runtime_error("deposit is short by " + ZecAmount(due - status.confirmed_zat) + " ZEC (" +
                                     ZecAmount(status.confirmed_zat) + " of " + ZecAmount(due) +
                                     " ZEC has 10 confirmations)");
        }
        const auto mat = SwapKeysForIndex(derive_, config_.network, *index, CounterChain::Zcash);
        SwapSession s;
        s.din_payout_script = PayoutScriptFromAddress(din_payout_address, config_.runner.din_hrp);
        s.btc_payout_script = refund;
        s.record.role = Role::BtcSeller;
        s.record.offer = offer;
        s.record.accept.offer_id = OfferId(offer);
        s.record.accept.din_claim_pubkey = XOnlyOf(mat.keys.din_secret_key);
        s.record.accept.btc_refund_pubkey = CompressedOf(mat.keys.btc_secret_key);
        s.din_sweep_pubkey = XOnlyOf(mat.keys.din_sweep_secret_key);
        s.record.state = SwapState::Accepted;
        s.record.state_since_unix = now;
        s.btc_scan_from_height = counter_scan_from(CounterChain::Zcash);
        s.din_scan_from_height = din_scan_from;
        const std::string accept_text = EncodeAccept(s.record.accept);
        EncryptedFileSwapStore(SwapPath(*index), store_key).Save(s);
        WriteFileAtomically(SwapPath(*index) + ".id", id + "\n");
        fs::remove(DepositPath(*index));
        fs::remove(DepositPath(*index) + ".id");
        StartSession(*index, std::move(s), store_key);
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
            const auto mat = SwapKeysForIndex(derive_, config_.network, index, offer.chain);
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
            s.btc_scan_from_height = counter_scan_from(offer.chain);
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

DepositInfo SwapManager::DepositOf(uint32_t index, const SwapOffer& offer) {
    DepositInfo d;
    d.id = SwapId(offer);
    d.index = index;
    const Bytes32 key = ZecDepositKey(derive_, config_.network, index);
    d.address = zec_->deposit_address(key);
    d.amount_due_zat = offer.btc_amount_sat + zec_->funding_fee_zat;
    try {
        d.status = zec_->deposit_status(key);
    } catch (const std::exception&) {
        // Zebra unreachable: the address and amount still stand
    }
    return d;
}

DepositInfo SwapManager::PrepareDeposit(const std::string& offer_text, const std::string& zec_refund_address,
                                        uint32_t now) {
    std::lock_guard<std::mutex> lock(mu_);
    const Bytes32 store_key = StoreKey();
    RequireZec();
    const SwapOffer offer = DecodeOffer(offer_text);
    if (offer.chain != CounterChain::Zcash) throw std::invalid_argument("not a Zcash offer");
    if (offer.network != config_.network) throw std::invalid_argument("offer is for another network");
    const std::string id = SwapId(offer);
    for (const auto& [i, path] : ScanDir(".deposit")) {  // the same offer again: the same deposit
        std::string did;
        if (std::ifstream in(path + ".id"); in) in >> did;
        if (did == id) return DepositOf(i, offer);
    }
    RequireAcceptableNow(offer, now);
    zec_->require_no_upgrade(offer, now);
    RequireSaneZec(offer.btc_amount_sat, zec_->spend_fee_zat);
    RequireSane(config_, offer.din_amount_una, 20 * config_.runner.btc_fee_sat, offer.n_btc_confirmations);
    for (const auto& [i, path] : ScanDir(".swap")) {
        std::string sid;
        if (std::ifstream in(path + ".id"); in) in >> sid;
        if (sid == id) throw std::runtime_error("swap " + id + " already exists");
    }
    const auto refund = zec_->payout_script(zec_refund_address);
    SwapStoreKeyFromSeed(derive_, config_.network);  // a relocked wallet: refused before an index is used
    const uint32_t index = AllocateIndex();
    std::ostringstream f;
    f << "offer=" << offer_text << "\nrefund=" << ToHex(refund) << "\n";
    SealToFile(DepositPath(index), store_key, f.str());
    WriteFileAtomically(DepositPath(index) + ".id", id + "\n");  // public: readable while locked
    return DepositOf(index, offer);
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
    m.chain = s.record.offer.chain;
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
            m.chain = o.chain;
            out.push_back(m);
        } catch (const std::exception&) {
        }
    }
    for (const auto& [index, path] : ScanDir(".deposit")) {  // Bob's Zcash deposits awaiting his accept
        SwapSummary m;
        m.index = index;
        m.role = Role::BtcSeller;
        m.chain = CounterChain::Zcash;
        m.pending_deposit = true;
        m.id = "deposit-" + std::to_string(index);
        if (std::ifstream in(path + ".id"); in) in >> m.id;
        if (!store_key || !zec_) {
            m.wallet_locked = !store_key;
            out.push_back(m);
            continue;
        }
        try {
            const SwapOffer o = DecodeOffer(ParseKeyValues(OpenSealedFile(path, *store_key))["offer"]);
            const DepositInfo d = DepositOf(index, o);
            m.din_amount_una = o.din_amount_una;
            m.btc_amount_sat = o.btc_amount_sat;
            m.t_btc_unix = o.t_btc_unix;
            m.t_din_unix = o.t_din_unix;
            m.deposit_address = d.address;
            m.deposit_due_zat = d.amount_due_zat;
            m.deposit = d.status;
            out.push_back(m);
        } catch (const std::exception& e) {
            m.last_events = {std::string("unreadable: ") + e.what()};
            out.push_back(m);
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
    for (const auto& [index, path] : ScanDir(".deposit")) {
        std::string did;
        if (std::ifstream in(path + ".id"); in) in >> did;
        if (did != id) continue;
        // Bob's unused deposit: send it back to his refund address.
        RequireZec();
        const auto kv = ParseKeyValues(OpenSealedFile(path, StoreKey()));
        const Bytes32 key = ZecDepositKey(derive_, config_.network, index);
        const auto status = zec_->deposit_status(key);
        if (status.seen_zat > 0) zec_->sweep_deposit(key, FromHex(kv.at("refund")));  // throws: nothing removed
        fs::remove(path);
        fs::remove(path + ".id");
        return;
    }
    const Bytes32 store_key = StoreKey();
    for (const auto& [index, path] : ScanDir(".swap")) {
        SwapSession s = live_.count(index) ? live_[index].runner->session() : LoadSession(index, store_key);
        if (SwapId(s.record.offer) != id) continue;
        const bool zec = s.record.offer.chain == CounterChain::Zcash;
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
            std::unique_ptr<RpcSwapChainIo> zio;
            if (zec) {
                RequireZec();
                zio = zec_->make_io(s, config_.runner, ZecDepositKey(derive_, config_.network, index));
            }
            RpcSwapChainIo bio(din_, btc_, s, config_.runner);
            RpcSwapChainIo& io = zec ? *zio : bio;
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
        if (zec && s.record.role == Role::BtcSeller) {
            // Nothing locked: Bob's deposit goes back to his refund address.
            RequireZec();
            const Bytes32 key = ZecDepositKey(derive_, config_.network, index);
            if (zec_->deposit_status(key).seen_zat > 0) zec_->sweep_deposit(key, s.btc_payout_script);
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
