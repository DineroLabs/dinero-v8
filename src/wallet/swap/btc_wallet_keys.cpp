#include "wallet/swap/btc_wallet_keys.h"

#include "wallet/swap/btc_deposit.h"
#include "wallet/swap/swap_crypto.h"

#include <cstring>
#include <stdexcept>

// The one HMAC-SHA512 of the code base (dinero_crypto_minimal.cpp), as BIP32Deriver uses it.
extern void hmac_sha512(const uint8_t* key, size_t keylen, const uint8_t* data, size_t datalen, uint8_t out64[64]);

namespace dinero::swap {
namespace {

void RequireChild(uint32_t branch, uint32_t index) {
    if (branch > 1) throw std::invalid_argument("Bitcoin wallet branch must be 0 (receive) or 1 (change)");
    if (index >= 0x80000000U) throw std::invalid_argument("Bitcoin wallet address index out of range");
}

secp256k1_pubkey Parse(const std::array<uint8_t, 33>& pubkey) {
    secp256k1_pubkey pk;
    if (secp256k1_ec_pubkey_parse(crypto::GetSecp256k1ContextSignVerify(), &pk, pubkey.data(), pubkey.size()) != 1) {
        throw std::invalid_argument("not a public key");
    }
    return pk;
}

std::array<uint8_t, 33> Serialize(const secp256k1_pubkey& pk) {
    std::array<uint8_t, 33> out{};
    size_t len = out.size();
    secp256k1_ec_pubkey_serialize(crypto::GetSecp256k1ContextSignVerify(), out.data(), &len, &pk, SECP256K1_EC_COMPRESSED);
    return out;
}

// BIP32 CKDpub: one non-hardened step. Returns the child's key and chain code.
BtcWalletAccount ChildPublic(const BtcWalletAccount& parent, uint32_t index) {
    uint8_t data[37];
    std::memcpy(data, parent.pubkey.data(), 33);
    data[33] = uint8_t(index >> 24);
    data[34] = uint8_t(index >> 16);
    data[35] = uint8_t(index >> 8);
    data[36] = uint8_t(index);
    uint8_t mac[64];
    ::hmac_sha512(parent.chain_code.data(), parent.chain_code.size(), data, sizeof(data), mac);
    secp256k1_pubkey pk = Parse(parent.pubkey);
    // Fails when the left half is not below the curve order or the sum is the point at infinity.
    if (secp256k1_ec_pubkey_tweak_add(crypto::GetSecp256k1ContextSignVerify(), &pk, mac) != 1) {
        throw std::invalid_argument("this Bitcoin wallet index has no key (BIP32): use the next one");
    }
    BtcWalletAccount child;
    child.pubkey = Serialize(pk);
    std::memcpy(child.chain_code.data(), mac + 32, 32);
    return child;
}

// BIP32 CKDpriv, non-hardened.
void ChildPrivate(Bytes32& key, Bytes32& chain_code, uint32_t index) {
    const auto pubkey = detail::CompressedOf(key);
    uint8_t data[37];
    std::memcpy(data, pubkey.data(), 33);
    data[33] = uint8_t(index >> 24);
    data[34] = uint8_t(index >> 16);
    data[35] = uint8_t(index >> 8);
    data[36] = uint8_t(index);
    uint8_t mac[64];
    ::hmac_sha512(chain_code.data(), chain_code.size(), data, sizeof(data), mac);
    if (secp256k1_ec_seckey_tweak_add(crypto::GetSecp256k1ContextSignVerify(), key.data(), mac) != 1) {
        throw std::invalid_argument("this Bitcoin wallet index has no key (BIP32): use the next one");
    }
    std::memcpy(chain_code.data(), mac + 32, 32);
    std::memset(mac, 0, sizeof(mac));
}

}  // namespace

BtcWalletAccount BtcWalletAccountFromSecret(const Bytes32& account_secret_key, const Bytes32& chain_code) {
    BtcWalletAccount a;
    a.pubkey = detail::CompressedOf(account_secret_key);
    a.chain_code = chain_code;
    return a;
}

std::array<uint8_t, 33> BtcWalletPubkey(const BtcWalletAccount& account, uint32_t branch, uint32_t index) {
    RequireChild(branch, index);
    return ChildPublic(ChildPublic(account, branch), index).pubkey;
}

Bytes32 BtcWalletSecretKey(const Bytes32& account_secret_key, const Bytes32& chain_code, uint32_t branch, uint32_t index) {
    RequireChild(branch, index);
    Bytes32 key = account_secret_key, cc = chain_code;
    ChildPrivate(key, cc, branch);
    ChildPrivate(key, cc, index);
    cc.fill(0);
    return key;
}

std::vector<uint8_t> BtcWalletScript(const BtcWalletAccount& account, uint32_t branch, uint32_t index) {
    return BtcP2wpkhScript(BtcWalletPubkey(account, branch, index));
}

std::string BtcWalletAddress(const BtcWalletAccount& account, uint32_t branch, uint32_t index, const std::string& hrp) {
    return BtcP2wpkhAddress(BtcWalletPubkey(account, branch, index), hrp);
}

std::vector<uint8_t> EncodeBtcWalletAccount(const BtcWalletAccount& account) {
    std::vector<uint8_t> out(account.pubkey.begin(), account.pubkey.end());
    out.insert(out.end(), account.chain_code.begin(), account.chain_code.end());
    return out;
}

BtcWalletAccount DecodeBtcWalletAccount(const std::vector<uint8_t>& bytes) {
    if (bytes.size() != 65) throw std::invalid_argument("not a Bitcoin wallet account (65 bytes expected)");
    BtcWalletAccount a;
    std::copy(bytes.begin(), bytes.begin() + 33, a.pubkey.begin());
    std::copy(bytes.begin() + 33, bytes.end(), a.chain_code.begin());
    Parse(a.pubkey);  // throws unless it is a point on the curve
    return a;
}

}  // namespace dinero::swap
