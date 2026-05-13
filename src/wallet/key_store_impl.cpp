#include "wallet/key_store_impl.h"
#include "wallet/bip86_descriptor.h"
#include "wallet/taproot_address.h"
#include "wallet/taproot_keys.h"
#include "crypto/evp_secp256k1.h"
#include "crypto/hd_keychain.h"
#include "crypto/hash160.h"
#include "crypto/secure_memory.h"
#include "consensus/coin_type.h"
#include <secp256k1.h>
#include <secp256k1_schnorrsig.h>
#include <secp256k1_extrakeys.h>
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <cstring>
#include <sstream>
#include <iomanip>

namespace din {

KeyStoreImpl::KeyStoreImpl(const std::string& wallet_name)
    : wallet_name_(wallet_name)
    , watch_only_(false)
    , encrypted_(true)
    , master_seed_locked_(false)
    , coin_type_(dinero::consensus::DINERO_COIN_TYPE)
{}

KeyStoreImpl::~KeyStoreImpl() {
    clearPrivateKeyCache();
    clearMasterSeed();
}

bool KeyStoreImpl::initializeFromSeed(const std::vector<uint8_t>& seed, uint32_t coin_type) {
    if (seed.size() != 64) {
        return false; // BIP39 seeds are 64 bytes
    }

    clearPrivateKeyCache();
    clearMasterSeed();
    xpub_cache_.clear();

    master_seed_ = seed;
    master_seed_locked_ = tryLockBytes(master_seed_);
    coin_type_ = coin_type;
    watch_only_ = false;
    encrypted_ = true;

    // Derive master key to get fingerprint
    try {
        auto master = dinero::crypto::HDKeychain::fromSeed(master_seed_);
        master_fingerprint_ = computeFingerprint(
            std::vector<uint8_t>(master.public_key.begin(), master.public_key.end())
        );
    } catch (const std::exception& e) {
        clearMasterSeed();
        return false;
    }

    return true;
}

bool KeyStoreImpl::initializeFromXPub(const std::string& xpub, const std::string& fingerprint) {
    // Watch-only mode
    clearPrivateKeyCache();
    clearMasterSeed();
    xpub_cache_.clear();

    watch_only_ = true;
    encrypted_ = true;
    master_fingerprint_ = fingerprint;

    // Cache the xpub
    xpub_cache_["m"] = xpub;

    return true;
}

std::optional<std::string> KeyStoreImpl::getXPub(const std::string& path) const {
    // Check cache first
    auto it = xpub_cache_.find(path);
    if (it != xpub_cache_.end()) {
        return it->second;
    }

    if (watch_only_) {
        return std::nullopt; // Can't derive xpubs from xpub in watch-only
    }

    // Derive xpub from master seed
    std::string xpub = deriveXPub(path);
    if (!xpub.empty()) {
        xpub_cache_[path] = xpub;
        return xpub;
    }

    return std::nullopt;
}

std::optional<std::string> KeyStoreImpl::getXPriv(const std::string& path) const {
    // Never expose xpriv for security
    return std::nullopt;
}

std::optional<std::vector<uint8_t>> KeyStoreImpl::sign(
    const std::vector<uint8_t>& hash,
    const std::string& key_path)
{
    if (watch_only_) {
        return std::nullopt; // Can't sign in watch-only mode
    }

    if (hash.size() != 32) {
        return std::nullopt; // Invalid hash size
    }

    try {
        // Derive private key for the path
        std::vector<uint8_t> privkey = derivePrivateKey(key_path);
        if (privkey.empty() || privkey.size() != 32) {
            return std::nullopt;
        }

        dinero::crypto::LockedMemory privkey_lock(privkey.data(), privkey.size());
        secp256k1_context* ctx = dinero::crypto::GetSecp256k1ContextSign();

        secp256k1_ecdsa_signature sig;
        if (!secp256k1_ecdsa_sign(ctx, &sig, hash.data(), privkey.data(), nullptr, nullptr)) {
            OPENSSL_cleanse(privkey.data(), privkey.size());
            return std::nullopt;
        }

        // Serialize to DER
        std::vector<uint8_t> der_sig(72); // Max DER signature size
        size_t der_len = der_sig.size();

        if (!secp256k1_ecdsa_signature_serialize_der(ctx, der_sig.data(), &der_len, &sig)) {
            OPENSSL_cleanse(privkey.data(), privkey.size());
            return std::nullopt;
        }

        der_sig.resize(der_len);

        OPENSSL_cleanse(privkey.data(), privkey.size());

        return der_sig;

    } catch (const std::exception& e) {
        return std::nullopt;
    }
}

bool KeyStoreImpl::canSign(const std::string& key_path) const {
    return !watch_only_;
}

bool KeyStoreImpl::hasKey(const std::string& key_path) const {
    if (watch_only_) {
        // In watch-only mode, we only have the master xpub
        return key_path == "m";
    }

    // In full mode, we can derive any path
    return true;
}

std::vector<std::string> KeyStoreImpl::listKeyPaths() const {
    if (watch_only_) {
        return {"m"};
    }

    // Return commonly used paths
    const std::string coin = std::to_string(coin_type_) + "h";
    return {
        "m",
        "m/84h/" + coin + "/0h",  // BIP84 account 0
        "m/86h/" + coin + "/0h"   // BIP86 account 0
    };
}

std::string KeyStoreImpl::deriveBIP84Address(uint32_t account, bool is_change, uint32_t index) const {
    if (watch_only_) {
        return ""; // Can't derive addresses in watch-only mode yet
    }

    try {
        auto master = dinero::crypto::HDKeychain::fromSeed(master_seed_);
        auto key = dinero::crypto::HDKeychain::deriveBIP84(
            master,
            coin_type_,
            account,
            is_change ? 1 : 0,
            index
        );

        return key.getAddress("din");

    } catch (const std::exception& e) {
        return "";
    }
}

std::string KeyStoreImpl::getBIP84Descriptor(uint32_t account, bool is_change) const {
    std::ostringstream oss;

    // Format: wpkh([fingerprint/84h/1448h/0h]xpub/change/*)
    oss << "wpkh([" << master_fingerprint_ << "/84h/" << coin_type_ << "h/" << account << "h]";

    // Get account xpub
    std::string path = "m/84h/" + std::to_string(coin_type_) + "h/" + std::to_string(account) + "h";
    auto xpub_opt = getXPub(path);
    if (!xpub_opt) {
        return "";
    }

    oss << *xpub_opt << "/" << (is_change ? "1" : "0") << "/*)";

    return oss.str();
}

std::string KeyStoreImpl::deriveBIP86Address(uint32_t account, bool is_change, uint32_t index) const {
    if (watch_only_) {
        return ""; // Can't derive addresses in watch-only mode yet
    }

    try {
        auto master = dinero::crypto::HDKeychain::fromSeed(master_seed_);

        // BIP86 path: m/86h/coin_type'/account'/change/index
        auto derived = master;
        derived = derived.derive(86 + dinero::crypto::HDKeychain::HARDENED_KEY_START);
        derived = derived.derive(coin_type_ + dinero::crypto::HDKeychain::HARDENED_KEY_START);
        derived = derived.derive(account + dinero::crypto::HDKeychain::HARDENED_KEY_START);
        derived = derived.derive(is_change ? 1 : 0);
        derived = derived.derive(index);

        // Extract x-only pubkey (first 32 bytes of 33-byte compressed pubkey)
        std::vector<uint8_t> xonly_pubkey(
            derived.public_key.begin() + 1,
            derived.public_key.begin() + 33
        );

        // Generate P2TR address
        std::string network = (coin_type_ == dinero::consensus::DINERO_COIN_TYPE) ? "mainnet" : "testnet";
        return TaprootAddress::fromPubkey(xonly_pubkey, network);

    } catch (const std::exception& e) {
        return "";
    }
}

std::string KeyStoreImpl::getBIP86Descriptor(uint32_t account, bool is_change) const {
    std::ostringstream oss;

    // Format: tr([fingerprint/86h/1448h/0h]xpub/change/*)
    oss << "tr([" << master_fingerprint_ << "/86h/" << coin_type_ << "h/" << account << "h]";

    // Get account xpub
    std::string path = "m/86h/" + std::to_string(coin_type_) + "h/" + std::to_string(account) + "h";
    auto xpub_opt = getXPub(path);
    if (!xpub_opt) {
        return "";
    }

    oss << *xpub_opt << "/" << (is_change ? "1" : "0") << "/*)";

    return oss.str();
}

// Private helper methods

std::string KeyStoreImpl::deriveXPub(const std::string& path) const {
    if (master_seed_.empty()) {
        return "";
    }

    try {
        auto master = dinero::crypto::HDKeychain::fromSeed(master_seed_);

        // Parse path and derive
        std::vector<uint32_t> path_vec = pathFromString(path);

        auto derived = master;
        for (uint32_t index : path_vec) {
            derived = derived.derive(index);
        }

        return derived.serialize(true); // mainnet

    } catch (const std::exception& e) {
        return "";
    }
}

std::vector<uint8_t> KeyStoreImpl::derivePrivateKey(const std::string& path) const {
    if (master_seed_.empty()) {
        return {};
    }

    // Check cache first
    auto it = private_key_cache_.find(path);
    if (it != private_key_cache_.end()) {
        return it->second.bytes;
    }

    try {
        auto master = dinero::crypto::HDKeychain::fromSeed(master_seed_);

        // Parse path and derive
        std::vector<uint32_t> path_vec = pathFromString(path);

        auto derived = master;
        for (uint32_t index : path_vec) {
            derived = derived.derive(index);
        }

        std::vector<uint8_t> privkey(
            derived.private_key.begin(),
            derived.private_key.end()
        );

        CachedPrivateKey cached;
        cached.bytes = privkey;
        cached.locked = tryLockBytes(cached.bytes);
        private_key_cache_[path] = std::move(cached);

        return privkey;

    } catch (const std::exception& e) {
        return {};
    }
}

std::string KeyStoreImpl::pathToString(const std::vector<uint32_t>& path) const {
    std::ostringstream oss;
    oss << "m";

    for (uint32_t index : path) {
        oss << "/";
        if (index >= dinero::crypto::HDKeychain::HARDENED_KEY_START) {
            oss << (index - dinero::crypto::HDKeychain::HARDENED_KEY_START) << "h";
        } else {
            oss << index;
        }
    }

    return oss.str();
}

std::vector<uint32_t> KeyStoreImpl::pathFromString(const std::string& path) const {
    std::vector<uint32_t> result;

    if (path.empty() || path[0] != 'm') {
        return result;
    }

    std::istringstream iss(path.substr(1)); // Skip 'm'
    std::string component;

    while (std::getline(iss, component, '/')) {
        if (component.empty()) continue;

        bool hardened = false;
        if (component.back() == 'h' || component.back() == '\'') {
            hardened = true;
            component.pop_back();
        }

        uint32_t index = std::stoul(component);
        if (hardened) {
            index |= dinero::crypto::HDKeychain::HARDENED_KEY_START;
        }

        result.push_back(index);
    }

    return result;
}

std::vector<uint8_t> KeyStoreImpl::hmacSHA512(
    const std::vector<uint8_t>& key,
    const std::vector<uint8_t>& data) const
{
    std::vector<uint8_t> result(64);

    unsigned int len = 64;
    HMAC(EVP_sha512(),
         key.data(), key.size(),
         data.data(), data.size(),
         result.data(), &len);

    return result;
}

std::string KeyStoreImpl::computeFingerprint(const std::vector<uint8_t>& pubkey) const {
    // Fingerprint = first 4 bytes of HASH160(pubkey)
    std::vector<uint8_t> hash160(20);
    HASH160(pubkey.data(), pubkey.size(), hash160.data());

    std::ostringstream oss;
    for (size_t i = 0; i < 4; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(hash160[i]);
    }

    return oss.str();
}

void KeyStoreImpl::clearMasterSeed() {
    secureClearBytes(master_seed_, master_seed_locked_);
    master_seed_locked_ = false;
}

void KeyStoreImpl::clearPrivateKeyCache() {
    for (auto& [_, cached] : private_key_cache_) {
        secureClearBytes(cached.bytes, cached.locked);
        cached.locked = false;
    }
    private_key_cache_.clear();
}

bool KeyStoreImpl::tryLockBytes(std::vector<uint8_t>& data) {
    if (data.empty()) {
        return false;
    }
    return dinero::crypto::lockMemory(data.data(), data.size());
}

void KeyStoreImpl::secureClearBytes(std::vector<uint8_t>& data, bool was_locked) {
    if (!data.empty()) {
        OPENSSL_cleanse(data.data(), data.size());
        if (was_locked) {
            dinero::crypto::unlockMemory(data.data(), data.size());
        }
    }
    data.clear();
    data.shrink_to_fit();
}

} // namespace din
