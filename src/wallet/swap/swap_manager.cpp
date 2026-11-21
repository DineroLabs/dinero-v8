#include "wallet/swap/swap_manager.h"

#include "bech32/bech32.hpp"
#include "crypto/sha256.h"
#include "crypto/secure_random.h"
#include "primitives/amount.h"
#include "wallet/swap/swap_crypto.h"
#include "wallet/swap/tower.h"

#include <openssl/crypto.h>

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

DerivedKeyBatch Derive(const KeyDeriver& derive, const std::vector<std::vector<uint32_t>>& paths,
                       bool require_funding_identity = false) {
    auto batch = derive(paths, require_funding_identity);
    if (!batch) throw std::runtime_error("wallet locked: unlock it to use swaps");
    // A malformed result is not an ordinary relock and cannot use the cache.
    if (batch->keys.size() != paths.size()) throw std::invalid_argument("incomplete swap key batch");
    return std::move(*batch);
}

SwapKeyMaterial BoundSwapKeysForIndex(const KeyDeriver& derive, SwapNetwork network,
                                     uint32_t index, const Bytes32& store_key, bool require_funding_identity = false,
                                     CounterChain chain = CounterChain::Bitcoin) {
    auto material = SwapKeysForIndex(derive, network, index, chain, require_funding_identity);
    if (CRYPTO_memcmp(material.store_key.data(), store_key.data(), store_key.size()) != 0)
        throw std::runtime_error("swap key batch belongs to a different wallet");
    return material;
}

// Bob's Zcash deposit key, derived in one batch with the store key so it is
// never taken from another wallet than the one that owns this manager's swaps.
// `branch`: 6' for Zcash, 7' for Bitcoin (kBtcDepositBranch).
constexpr uint32_t kZecDepositBranch = 6;
constexpr uint32_t kBtcDepositBranch = 7;
Bytes32 BoundZecDepositKey(const KeyDeriver& derive, SwapNetwork network, uint32_t index, const Bytes32& store_key,
                           uint32_t branch = kZecDepositBranch) {
    if (index >= 0x80000000U) throw std::invalid_argument("swap index out of range");
    const auto batch = Derive(derive, {{kSwapKeyPurpose, uint32_t(network), 0},
                                       {kSwapKeyPurpose, uint32_t(network), branch, index}});
    struct Scoped {
        Bytes32 value{};
        ~Scoped() { OPENSSL_cleanse(value.data(), value.size()); }
    } owner{DeriveSwapStoreKey(batch.keys[0])};
    if (CRYPTO_memcmp(owner.value.data(), store_key.data(), store_key.size()) != 0)
        throw std::runtime_error("swap key batch belongs to a different wallet");
    CompressedOf(batch.keys[1]);
    return batch.keys[1];
}

std::map<std::string, std::string> ParseKeyValues(const std::string& text) {
    std::istringstream in(text);
    std::map<std::string, std::string> kv;
    for (std::string line; std::getline(in, line);) {
        const auto eq = line.find('=');
        if (eq != std::string::npos && !kv.emplace(line.substr(0, eq), line.substr(eq + 1)).second)
            throw std::invalid_argument("duplicate encrypted swap offer field");
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

// Alice's DIN refund must wait for T_din, or she could take the BTC and refund
// her DIN before Bob's claim confirms. CLTV only compares against the spend's
// own nLockTime; the chain makes that binding only once it enforces
// transaction time locks (contextual locks; never on Dinero testnet today).
void RequireDinTimeLocks(const DinRpc& din) {
    const auto info = din("getblockchaininfo", Json::Value(Json::arrayValue));
    if (!info || !info->isObject()) throw std::runtime_error("Dinero node unreachable");
    const Json::Value& active = (*info)["contextual_locks_active"];
    if (!active.isBool() || !active.asBool()) {
        throw std::runtime_error("this Dinero network does not enforce transaction time locks yet, "
                                 "so a swap's refund lock would not hold");
    }
}

}  // namespace

// ---- Keys -------------------------------------------------------------------

DerivedKeyBatch::DerivedKeyBatch(DerivedKeyBatch&& other) noexcept : wallet_id(other.wallet_id) {
    keys.swap(other.keys);
    other.wallet_id = {};
}
DerivedKeyBatch::~DerivedKeyBatch() {
    for (auto& key : keys) OPENSSL_cleanse(key.data(), key.size());
}
SwapKeyMaterial::~SwapKeyMaterial() {
    OPENSSL_cleanse(keys.din_secret_key.data(), keys.din_secret_key.size());
    OPENSSL_cleanse(keys.btc_secret_key.data(), keys.btc_secret_key.size());
    OPENSSL_cleanse(keys.din_sweep_secret_key.data(), keys.din_sweep_secret_key.size());
    OPENSSL_cleanse(store_key.data(), store_key.size());
}

Bytes32 SwapStoreKeyFromSeed(const KeyDeriver& derive, SwapNetwork network) {
    const auto batch = Derive(derive, {{kSwapKeyPurpose, uint32_t(network), 0}});
    return DeriveSwapStoreKey(batch.keys[0]);
}

SwapKeyMaterial SwapKeysForIndex(const KeyDeriver& derive, SwapNetwork network, uint32_t index, CounterChain chain,
                                 bool require_funding_identity) {
    if (index >= 0x80000000U) throw std::invalid_argument("swap index out of range");
    const uint32_t counter_branch = chain == CounterChain::Zcash ? 5 : 2;
    const auto batch = Derive(derive, {
        {kSwapKeyPurpose, uint32_t(network), 0},
        {kSwapKeyPurpose, uint32_t(network), 1, index},
        {kSwapKeyPurpose, uint32_t(network), counter_branch, index},
        {kSwapKeyPurpose, uint32_t(network), 3, index}}, require_funding_identity);
    SwapKeyMaterial m;
    m.store_key = DeriveSwapStoreKey(batch.keys[0]);
    m.wallet_id = batch.wallet_id;
    m.keys.din_secret_key = batch.keys[1];
    m.keys.btc_secret_key = batch.keys[2];
    m.keys.din_sweep_secret_key = batch.keys[3];
    XOnlyOf(m.keys.din_secret_key);
    CompressedOf(m.keys.btc_secret_key);
    XOnlyOf(m.keys.din_sweep_secret_key);
    return m;
}

SwapKeyMaterial SwapKeysForIndex(const KeyDeriver& derive, SwapNetwork network, uint32_t index, bool require_funding_identity) {
    return SwapKeysForIndex(derive, network, index, CounterChain::Bitcoin, require_funding_identity);
}

Bytes32 ZecDepositKey(const KeyDeriver& derive, SwapNetwork network, uint32_t index) {
    if (index >= 0x80000000U) throw std::invalid_argument("swap index out of range");
    const auto batch = Derive(derive, {{kSwapKeyPurpose, uint32_t(network), 6, index}});
    CompressedOf(batch.keys[0]);
    return batch.keys[0];
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

namespace {

// Base58Check payload (version byte + data), or nullopt on a bad character,
// length or checksum.
std::optional<std::vector<uint8_t>> Base58CheckDecode(const std::string& text) {
    static const std::string kAlphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    if (text.empty() || text.size() > 64) return std::nullopt;
    std::vector<uint8_t> bytes;  // little-endian
    for (char c : text) {
        const size_t digit = kAlphabet.find(c);
        if (digit == std::string::npos) return std::nullopt;
        uint32_t carry = uint32_t(digit);
        for (auto& b : bytes) {
            carry += uint32_t(b) * 58;
            b = uint8_t(carry);
            carry >>= 8;
        }
        for (; carry; carry >>= 8) bytes.push_back(uint8_t(carry));
    }
    for (size_t i = 0; i < text.size() && text[i] == '1'; ++i) bytes.push_back(0);
    std::reverse(bytes.begin(), bytes.end());
    if (bytes.size() < 5) return std::nullopt;
    uint8_t once[32], twice[32];
    crypto::CSHA256().Write(bytes.data(), bytes.size() - 4).Finalize(once);
    crypto::CSHA256().Write(once, 32).Finalize(twice);
    if (!std::equal(twice, twice + 4, bytes.end() - 4)) return std::nullopt;
    bytes.resize(bytes.size() - 4);
    return bytes;
}

}  // namespace

std::vector<uint8_t> BtcPayoutScriptFromAddress(const std::string& address, const std::string& hrp) {
    if (bech32::Decode(hrp, address)) return PayoutScriptFromAddress(address, hrp);
    const bool mainnet = hrp == "bc";
    const uint8_t p2pkh = mainnet ? 0x00 : 0x6f, p2sh = mainnet ? 0x05 : 0xc4;
    const auto payload = Base58CheckDecode(address);
    if (payload && payload->size() == 21 && ((*payload)[0] == p2pkh || (*payload)[0] == p2sh)) {
        std::vector<uint8_t> spk = (*payload)[0] == p2pkh ? std::vector<uint8_t>{0x76, 0xa9, 0x14}
                                                          : std::vector<uint8_t>{0xa9, 0x14};
        spk.insert(spk.end(), payload->begin() + 1, payload->end());
        if ((*payload)[0] == p2pkh) spk.insert(spk.end(), {0x88, 0xac});
        else spk.push_back(0x87);
        return spk;
    }
    throw std::invalid_argument("not a Bitcoin address of this network (" + hrp + "1…, " +
                                (mainnet ? "1… or 3…" : "m…, n… or 2…") + "): " + address);
}

std::string SwapId(const SwapOffer& offer) {
    const auto id = OfferId(offer);
    return ToHex(std::vector<uint8_t>(id.begin(), id.begin() + 8));
}

// ---- Manager ----------------------------------------------------------------

SwapManager::SwapManager(SwapManagerConfig config, KeyDeriver derive, DinRpc din, BtcRpc btc, DinFundingOwner funding)
    : config_(std::move(config)), derive_(std::move(derive)), din_(std::move(din)), btc_(std::move(btc)), funding_(std::move(funding)) {}

void SwapManager::SetTowerSink(std::function<void(const std::string&)> sink) {
    std::lock_guard<std::mutex> lock(mu_);
    tower_sink_ = std::move(sink);
}

void SwapManager::SetTowerAck(std::function<bool(const std::string&, const std::string&)> ack) {
    std::lock_guard<std::mutex> lock(mu_);
    tower_ack_ = std::move(ack);
}

Bytes32 SwapManager::StoreKey() {
    struct CandidateKey {
        Bytes32 value{};
        ~CandidateKey() { OPENSSL_cleanse(value.data(), value.size()); }
    } candidate;
    try {
        candidate.value = SwapStoreKeyFromSeed(derive_, config_.network);
    } catch (const std::runtime_error&) {
        if (!store_key_) throw;  // locked and never unlocked since start: paused
        return *store_key_;     // established live swaps survive an ordinary relock
    }
    // A successfully derived different key is not a relock. Refuse outside
    // the unavailable-key catch so it cannot overwrite or bypass the owner
    // of this manager's cached sessions.
    if (store_key_ && CRYPTO_memcmp(store_key_->data(), candidate.value.data(), candidate.value.size()) != 0) {
        throw std::runtime_error("swap wallet changed: restore the original wallet before using this manager");
    }
    if (!store_key_) store_key_ = candidate.value;
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

void SwapManager::SetLightBitcoin(LightBtcSupport light) {
    if (!light.make_io || !light.tip_height) throw std::invalid_argument("incomplete light Bitcoin support");
    std::lock_guard<std::mutex> lock(mu_);
    light_btc_ = std::move(light);
}

bool SwapManager::SupportsZec() const { return zec_.has_value() && config_.network != SwapNetwork::Mainnet; }

struct SwapManager::DepositOps {
    std::function<std::string(const Bytes32&)> address;
    std::function<ZecDepositStatus(const Bytes32&)> status;
    std::function<std::string(const Bytes32&, const std::vector<uint8_t>&)> sweep;
    uint64_t funding_fee{};       // added to the swap amount: what the deposit must hold
    uint32_t key_branch{};
    uint32_t confirmations{};     // a payment this deep counts towards the lock
    const char* coin{""};         // "ZEC" / "BTC", for messages
};

SwapManager::DepositOps SwapManager::DepositOpsFor(CounterChain chain) const {
    DepositOps ops;
    if (chain == CounterChain::Zcash) {
        RequireZec();
        ops.address = zec_->deposit_address;
        ops.status = zec_->deposit_status;
        ops.sweep = zec_->sweep_deposit;
        ops.funding_fee = zec_->funding_fee_zat;
        ops.key_branch = kZecDepositBranch;
        ops.confirmations = 10;
        ops.coin = "ZEC";
        return ops;
    }
    if (!light_btc_) {
        throw std::runtime_error("this node pays BTC from Bitcoin Core: accept the offer directly (swap.accept)");
    }
    if (!light_btc_->CanPay()) {
        throw std::runtime_error("this node uses Bitcoin without Bitcoin Core: it can sell DIN for BTC, but buying "
                                 "DIN with BTC still needs Bitcoin Core (swap.btc_rpc)");
    }
    ops.address = light_btc_->deposit_address;
    ops.status = light_btc_->deposit_status;
    ops.sweep = light_btc_->sweep_deposit;
    ops.funding_fee = light_btc_->funding_fee_sat;
    ops.key_branch = kBtcDepositBranch;
    ops.confirmations = 2;
    ops.coin = "BTC";
    return ops;
}

bool SwapManager::PaysFromDeposit(const SwapSession& s) const {
    if (s.record.role != Role::BtcSeller) return false;
    if (s.record.offer.chain == CounterChain::Zcash) return true;
    return light_btc_ && light_btc_->CanPay();
}

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
    const auto mat = BoundSwapKeysForIndex(derive_, config_.network, index, store_key, false, chain);
    RunnerConfig rc = config_.runner;
    rc.use_tower = static_cast<bool>(tower_sink_);
    rc.require_tower = config_.require_tower_for_bob;
    Live live;
    live.store = std::make_unique<EncryptedFileSwapStore>(SwapPath(index), store_key);
    if (chain == CounterChain::Zcash) {
        RequireZec();
        // dinero-swap-tower is Bitcoin-only: it would refuse a Zcash package and
        // never ack, so a Zcash swap runs without it (Bob stays online).
        rc.use_tower = false;
        const Bytes32 deposit_key = session.record.role == Role::BtcSeller
                                        ? BoundZecDepositKey(derive_, config_.network, index, store_key)
                                        : Bytes32{};
        live.io = zec_->make_io(session, rc, deposit_key);
    } else {
        if (light_btc_) {
            const Bytes32 deposit_key = PaysFromDeposit(session)
                                            ? BoundZecDepositKey(derive_, config_.network, index, store_key, kBtcDepositBranch)
                                            : Bytes32{};
            live.io = light_btc_->make_io(session, rc, deposit_key);
        } else {
            live.io = std::make_unique<RpcSwapChainIo>(din_, btc_, session, rc);
        }
        if (tower_sink_) live.io->SetTowerSink(tower_sink_);
        if (tower_ack_) live.io->SetTowerAck(tower_ack_);
    }
    live.io->SetDinFundingOwner(funding_);  // either chain: Alice's DIN lock is paid by the wallet
    live.runner = std::make_unique<SwapRunner>(std::move(session), mat.keys, rc, *live.io, *live.store);
    live_[index] = std::move(live);
}

std::string SwapManager::MakeOffer(const OfferRequest& r, uint32_t now) {
    std::lock_guard<std::mutex> lock(mu_);
    const Bytes32 store_key = StoreKey();  // bind this operation to the established store key
    RequireDinTimeLocks(din_);
    // Validate explicit fees before address parsing/index allocation. The real
    // payer is captured with the complete derivation batch below.
    if (!r.din_funding_fee_rate_hint || r.din_funding_fee_rate_hint > MAX_SUPPLY_UNA_CONST ||
        !r.din_funding_maximum_fee_una || r.din_funding_maximum_fee_una > MAX_SUPPLY_UNA_CONST)
        throw std::invalid_argument("new swap offer requires explicit bounded funding fee terms");
    DinFundingBinding funding;
    funding.fee_rate_hint = r.din_funding_fee_rate_hint;
    funding.maximum_fee_una = r.din_funding_maximum_fee_una;
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
                                : BtcPayoutScriptFromAddress(r.btc_claim_address, config_.runner.btc_hrp);
    // The cached store key outlives a relock: ask the wallet itself, so a
    // locked wallet is refused before an index is used up.
    SwapStoreKeyFromSeed(derive_, config_.network);
    const uint32_t index = AllocateIndex();
    const auto mat = BoundSwapKeysForIndex(derive_, config_.network, index, store_key, true, r.chain);
    funding.wallet_id = mat.wallet_id;
    RequireDinFundingBinding(funding);  // never synthesize a missing payer
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
    f << "din_funding_wallet_id=" << ToHex(std::vector<uint8_t>(funding.wallet_id.begin(), funding.wallet_id.end())) << "\n"
      << "din_funding_fee_rate_hint=" << funding.fee_rate_hint << "\n"
      << "din_funding_maximum_fee_una=" << funding.maximum_fee_una << "\n";
    SealToFile(OfferPath(index), store_key, f.str());
    WriteFileAtomically(OfferPath(index) + ".id", SwapId(o) + "\n");  // public: readable while locked
    return text;
}

SwapManager::AcceptResult SwapManager::Accept(const std::string& text, const PayoutAddressSource& din_payout_address,
                                              const std::string& btc_refund_address, uint32_t now) {
    std::lock_guard<std::mutex> lock(mu_);
    const Bytes32 store_key = StoreKey();
    RequireDinTimeLocks(din_);
    // Where the counter-chain watcher starts: a few blocks below the tip.
    auto counter_scan_from = [&](CounterChain chain) -> uint32_t {
        uint32_t tip = 0;
        if (chain == CounterChain::Zcash) {
            RequireZec();
            tip = zec_->tip_height();
        } else if (light_btc_) {
            tip = light_btc_->tip_height();
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

    // Without Bitcoin Core a BTC offer is paid from a deposit, like a Zcash one.
    const bool btc_from_deposit = text.rfind(kOfferPrefix, 0) == 0 && light_btc_.has_value();
    if (text.rfind(kOfferPrefix, 0) == 0 && !btc_from_deposit) {  // Bob accepts Alice's offer, paying from Bitcoin Core
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
        s.din_payout_script = PayoutScriptFromAddress(din_payout_address(), config_.runner.din_hrp);
        s.btc_payout_script = BtcPayoutScriptFromAddress(btc_refund_address, config_.runner.btc_hrp);
        SwapStoreKeyFromSeed(derive_, config_.network);  // a relocked wallet: refused before an index is used
        const uint32_t index = AllocateIndex();
        const auto mat = BoundSwapKeysForIndex(derive_, config_.network, index, store_key);
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
    if (zec_text || btc_from_deposit) {  // Bob accepts from the deposit he paid (PrepareDeposit)
        const SwapOffer offer = DecodeOffer(text);
        if (offer.network != config_.network) throw std::invalid_argument("offer is for another network");
        const bool zec = offer.chain == CounterChain::Zcash;
        const DepositOps ops = DepositOpsFor(offer.chain);
        RequireAcceptableNow(offer, now);
        if (zec) {
            zec_->require_no_upgrade(offer, now);
            RequireSaneZec(offer.btc_amount_sat, zec_->spend_fee_zat);
            RequireSane(config_, offer.din_amount_una, 20 * config_.runner.btc_fee_sat, offer.n_btc_confirmations);
        } else {
            if (config_.require_tower_for_bob && (!tower_sink_ || !tower_ack_)) {
                throw std::runtime_error("mainnet beta: buying DIN needs a watchtower (set swap.tower_inbox and run "
                                         "dinero-swap-tower)");
            }
            RequireWithinCaps(config_, offer.din_amount_una, offer.btc_amount_sat);
            RequireSane(config_, offer.din_amount_una, offer.btc_amount_sat, offer.n_btc_confirmations);
        }
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
        const auto status =
            ops.status(BoundZecDepositKey(derive_, config_.network, *index, store_key, ops.key_branch));
        const uint64_t due = offer.btc_amount_sat + ops.funding_fee;
        if (status.confirmed_zat < due) {
            throw std::runtime_error("deposit is short by " + ZecAmount(due - status.confirmed_zat) + " " + ops.coin + " (" +
                                     ZecAmount(status.confirmed_zat) + " of " + ZecAmount(due) + " " + ops.coin + " has " +
                                     std::to_string(ops.confirmations) + " confirmations)");
        }
        const auto mat = BoundSwapKeysForIndex(derive_, config_.network, *index, store_key, false, offer.chain);
        SwapSession s;
        s.din_payout_script = PayoutScriptFromAddress(din_payout_address(), config_.runner.din_hrp);
        s.btc_payout_script = refund;
        s.record.role = Role::BtcSeller;
        s.record.offer = offer;
        s.record.accept.offer_id = OfferId(offer);
        s.record.accept.din_claim_pubkey = XOnlyOf(mat.keys.din_secret_key);
        s.record.accept.btc_refund_pubkey = CompressedOf(mat.keys.btc_secret_key);
        s.din_sweep_pubkey = XOnlyOf(mat.keys.din_sweep_secret_key);
        s.record.state = SwapState::Accepted;
        s.record.state_since_unix = now;
        s.btc_scan_from_height = counter_scan_from(offer.chain);
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
            const auto mat = BoundSwapKeysForIndex(derive_, config_.network, index, store_key, true, offer.chain);
            SwapSession s;
            s.record.role = Role::DinSeller;
            // An old pending offer is not permission to use today's selected
            // wallet or fee defaults. Preserve it for explicit reconciliation.
            s.din_funding_binding = ParseDinFundingBinding(kv.at("din_funding_wallet_id"),
                kv.at("din_funding_fee_rate_hint"), kv.at("din_funding_maximum_fee_una"));
            if (s.din_funding_binding->wallet_id != mat.wallet_id)
                throw std::runtime_error("pending swap offer belongs to a different payer database");
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

DepositInfo SwapManager::DepositOf(uint32_t index, const SwapOffer& offer, const Bytes32& store_key) {
    DepositInfo d;
    d.id = SwapId(offer);
    d.index = index;
    const DepositOps ops = DepositOpsFor(offer.chain);
    const Bytes32 key = BoundZecDepositKey(derive_, config_.network, index, store_key, ops.key_branch);
    d.address = ops.address(key);
    d.amount_due_zat = offer.btc_amount_sat + ops.funding_fee;
    try {
        d.status = ops.status(key);
    } catch (const std::exception&) {
        // the counter chain is unreachable: the address and amount still stand
    }
    return d;
}

DepositInfo SwapManager::PrepareDeposit(const std::string& offer_text, const std::string& zec_refund_address,
                                        uint32_t now) {
    std::lock_guard<std::mutex> lock(mu_);
    const Bytes32 store_key = StoreKey();
    const SwapOffer offer = DecodeOffer(offer_text);
    if (offer.network != config_.network) throw std::invalid_argument("offer is for another network");
    const bool zec = offer.chain == CounterChain::Zcash;
    DepositOpsFor(offer.chain);  // refuses a chain this node has no deposit flow for
    const std::string id = SwapId(offer);
    for (const auto& [i, path] : ScanDir(".deposit")) {  // the same offer again: the same deposit
        std::string did;
        if (std::ifstream in(path + ".id"); in) in >> did;
        if (did == id) return DepositOf(i, offer, store_key);
    }
    RequireAcceptableNow(offer, now);
    if (zec) {
        zec_->require_no_upgrade(offer, now);
        RequireSaneZec(offer.btc_amount_sat, zec_->spend_fee_zat);
        RequireSane(config_, offer.din_amount_una, 20 * config_.runner.btc_fee_sat, offer.n_btc_confirmations);
    } else {
        // The same rules as accepting a BTC offer: nobody is asked to pay a deposit for a swap that cannot start.
        if (config_.require_tower_for_bob && (!tower_sink_ || !tower_ack_)) {
            throw std::runtime_error("mainnet beta: buying DIN needs a watchtower (set swap.tower_inbox and run "
                                     "dinero-swap-tower)");
        }
        RequireWithinCaps(config_, offer.din_amount_una, offer.btc_amount_sat);
        RequireSane(config_, offer.din_amount_una, offer.btc_amount_sat, offer.n_btc_confirmations);
    }
    for (const auto& [i, path] : ScanDir(".swap")) {
        std::string sid;
        if (std::ifstream in(path + ".id"); in) in >> sid;
        if (sid == id) throw std::runtime_error("swap " + id + " already exists");
    }
    const auto refund = zec ? zec_->payout_script(zec_refund_address)
                            : BtcPayoutScriptFromAddress(zec_refund_address, config_.runner.btc_hrp);
    SwapStoreKeyFromSeed(derive_, config_.network);  // a relocked wallet: refused before an index is used
    const uint32_t index = AllocateIndex();
    std::ostringstream f;
    f << "offer=" << offer_text << "\nrefund=" << ToHex(refund) << "\n";
    SealToFile(DepositPath(index), store_key, f.str());
    // Public, readable while locked: the swap id, then the chain the deposit is on.
    WriteFileAtomically(DepositPath(index) + ".id", id + "\n" + (zec ? "zec" : "btc") + "\n");
    return DepositOf(index, offer, store_key);
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
    if (!s.din_funding_raw.empty())
        m.din_funding_submission_acknowledged = s.din_funding_submitted;
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
            const SwapSession s = LoadSession(index, *store_key);
            std::vector<std::string> events;
            if (s.record.offer.chain == CounterChain::Zcash && !zec_ && !Finished(s)) {
                // TickAll cannot start it: say so loudly, its funds may be locked.
                events.push_back("NOT being watched: this Zcash swap needs a dinerod built with DINERO_SWAP_ZEC "
                                 "and swap.zec_rpc set");
            }
            out.push_back(Summarize(index, s, events));
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
        std::string deposit_chain;  // absent in files written before Bitcoin deposits: Zcash
        if (std::ifstream in(path + ".id"); in) in >> m.id >> deposit_chain;
        if (deposit_chain == "btc") m.chain = CounterChain::Bitcoin;
        const bool supported = m.chain == CounterChain::Zcash ? zec_.has_value() : light_btc_ && light_btc_->CanPay();
        if (!store_key || !supported) {
            m.wallet_locked = !store_key;
            out.push_back(m);
            continue;
        }
        try {
            const SwapOffer o = DecodeOffer(ParseKeyValues(OpenSealedFile(path, *store_key))["offer"]);
            const DepositInfo d = DepositOf(index, o, *store_key);
            m.din_amount_una = o.din_amount_una;
            m.btc_amount_sat = o.btc_amount_sat;
            m.t_btc_unix = o.t_btc_unix;
            m.t_din_unix = o.t_din_unix;
            m.deposit_address = d.address;
            m.deposit_offer = EncodeOffer(o);
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
            // The public sidecar is only a locator. Authenticate the existing
            // encrypted offer and its identity before deleting either file.
            struct ScopedKey {
                Bytes32 value;
                ~ScopedKey() { OPENSSL_cleanse(value.data(), value.size()); }
            } key{StoreKey()};
            struct ScopedPlaintext {
                std::string value;
                ~ScopedPlaintext() { OPENSSL_cleanse(value.data(), value.size()); }
            } plaintext{OpenSealedFile(path, key.value)};
            std::string offer_text;  // public offer only; do not copy the secret
            bool found_offer = false;
            for (size_t begin = 0; begin < plaintext.value.size();) {
                const auto end = plaintext.value.find('\n', begin);
                const auto length = (end == std::string::npos ? plaintext.value.size() : end) - begin;
                if (length >= 6 && plaintext.value.compare(begin, 6, "offer=") == 0) {
                    if (found_offer) throw std::runtime_error("ambiguous encrypted swap offer");
                    found_offer = true;
                    offer_text = plaintext.value.substr(begin + 6, length - 6);
                }
                if (end == std::string::npos) break;
                begin = end + 1;
            }
            if (!found_offer) throw std::runtime_error("missing encrypted swap offer");
            const auto offer = DecodeOffer(offer_text);
            if (offer.network != config_.network || SwapId(offer) != id)
                throw std::runtime_error("encrypted swap offer identity mismatch");
            fs::remove(path);  // authenticated unanswered offer; nothing was locked
            fs::remove(path + ".id");
            return;
        }
    }
    for (const auto& [index, path] : ScanDir(".deposit")) {
        std::string did;
        if (std::ifstream in(path + ".id"); in) in >> did;
        if (did != id) continue;
        // Bob's unused deposit: send it back to his refund address.
        const Bytes32 deposit_store_key = StoreKey();
        const auto kv = ParseKeyValues(OpenSealedFile(path, deposit_store_key));
        const DepositOps ops = DepositOpsFor(DecodeOffer(kv.at("offer")).chain);
        const Bytes32 key = BoundZecDepositKey(derive_, config_.network, index, deposit_store_key, ops.key_branch);
        const auto status = ops.status(key);
        if (status.seen_zat > 0) ops.sweep(key, FromHex(kv.at("refund")));  // throws: nothing removed
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
                zio = zec_->make_io(s, config_.runner, BoundZecDepositKey(derive_, config_.network, index, store_key));
            } else if (light_btc_) {
                zio = light_btc_->make_io(s, config_.runner,
                                          BoundZecDepositKey(derive_, config_.network, index, store_key, kBtcDepositBranch));
            }
            RpcSwapChainIo bio(din_, btc_, s, config_.runner);
            RpcSwapChainIo& io = zio ? *zio : bio;
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
        if (PaysFromDeposit(s)) {
            // Nothing locked: Bob's deposit goes back to his refund address.
            const DepositOps ops = DepositOpsFor(s.record.offer.chain);
            const Bytes32 key = BoundZecDepositKey(derive_, config_.network, index, store_key, ops.key_branch);
            if (ops.status(key).seen_zat > 0) ops.sweep(key, s.btc_payout_script);
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

SwapManager::RecoveredDeposits SwapManager::RecoverBtcDeposits(const std::string& btc_address, uint32_t extra_indexes) {
    std::lock_guard<std::mutex> lock(mu_);
    const Bytes32 store_key = StoreKey();
    const DepositOps ops = DepositOpsFor(CounterChain::Bitcoin);  // refuses a node that has no Bitcoin deposits
    const auto to_script = BtcPayoutScriptFromAddress(btc_address, config_.runner.btc_hrp);
    if (extra_indexes > 1000) throw std::invalid_argument("at most 1000 indexes beyond the ones in use");

    // Indexes whose deposit is still spoken for.
    std::map<uint32_t, std::string> busy;
    for (const auto& [index, path] : ScanDir(".deposit")) busy[index] = "its deposit is waiting for you to accept or cancel";
    uint32_t highest = 0;
    bool any = false;
    for (const char* ext : {".swap", ".offer", ".deposit"}) {
        const auto files = ScanDir(ext);
        if (!files.empty()) {
            highest = std::max(highest, files.rbegin()->first);
            any = true;
        }
    }
    for (const auto& [index, path] : ScanDir(".swap")) {
        try {
            const SwapSession s = live_.count(index) ? live_[index].runner->session() : LoadSession(index, store_key);
            if (s.record.role != Role::BtcSeller || s.record.offer.chain != CounterChain::Bitcoin) continue;
            // Until the lock is in a block its inputs are only spent in mempools: a
            // sweep now could compete with our own lock. From BtcLocked on it is confirmed.
            if (s.record.state == SwapState::Accepted || s.record.state == SwapState::BtcLockBroadcast) {
                busy[index] = "its swap has not locked yet";
            }
        } catch (const std::exception&) {
            busy[index] = "its swap record cannot be read";
        }
    }

    RecoveredDeposits out;
    const uint32_t last = (any ? highest : 0) + extra_indexes;
    for (uint32_t index = 0; index <= last; ++index) {
        ++out.indexes_checked;
        if (const auto it = busy.find(index); it != busy.end()) {
            out.skipped.push_back("index " + std::to_string(index) + ": " + it->second);
            continue;
        }
        const Bytes32 key = BoundZecDepositKey(derive_, config_.network, index, store_key, ops.key_branch);
        // Throws if no server answers: nothing is half done silently.
        const uint64_t found = light_btc_->recoverable_sat ? light_btc_->recoverable_sat(key) : ops.status(key).seen_zat;
        if (found == 0) continue;
        try {
            const std::string txid = ops.sweep(key, to_script);
            out.sweeps.push_back({index, found, txid});
        } catch (const std::exception& e) {
            out.skipped.push_back("index " + std::to_string(index) + ": " + std::to_string(found) +
                                  " sat found but not sent (" + e.what() + ")");
        }
    }
    return out;
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
