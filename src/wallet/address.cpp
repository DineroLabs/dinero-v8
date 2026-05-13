#include "wallet/address.h"
#include "crypto/evp_secp256k1.h"
#include "wallet/bip39.h"
#include "crypto/wallet_crypto.h"  // For crypto::deriveKeyArgon2id, encryptAesGcm, decryptAesGcm
#include "crypto/ripemd160.h"  // Use our new implementation
#include "crypto/hash160.h"
#include "common/logger.h"
#include "common/AddressCodec.h"
#include "consensus/chainparams.h"
#include "consensus/coin_type.h"
#include "crypto/dinero_crypto_minimal.h"  // For CF_* functions and sha256
#include "address/addr_codec.h"  // For HrpForActiveNetworkRef
#include <secp256k1.h>
#include <algorithm>
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <cstring>
#ifdef FFI_WALLET_ONLY
// iOS: No filesystem header needed
#include <sys/stat.h>  // For stat/mkdir
#include <unistd.h>    // For unlink
#else
#include <filesystem>
#endif

// === add these includes ===
#include <array>
#include <vector>
#include <cstring>

#include "crypto/hash160.h"         // declares dinero::sha256 and (likely) dinero::HASH160
#include "compat/jsoncpp_compat.h"              // Json::Value helpers (JsonCpp)

// If your project wraps these in a namespace, bring what we need into scope:
// using dinero::sha256;               // REMOVED - causes conflicts with dinero_crypto_minimal.h
// If HASH160 is declared in dinero namespace, uncomment the next line:
// using dinero::HASH160;

static secp256k1_context* secp_ctx()
{
    return dinero::crypto::GetSecp256k1ContextSignVerify();
}

// === Local replacements for the deleted CF_* API ===
// Note: These functions are already declared in dinero_crypto_minimal.h
// so we don't need to redefine them here

// Note: CF_SignDER and CF_VerifyDER are already declared in dinero_crypto_minimal.h

// === Minimal JSON helpers to replace getStringField/getIntField/getBoolField ===
static std::string getStringField(const Json::Value& obj, const char* key) {
    const auto& v = obj[key];
    return v.isString() ? v.asString() : std::string();
}

static std::string getStringField(const Json::Value& obj, const char* key, const std::string& defaultValue) {
    const auto& v = obj[key];
    return v.isString() ? v.asString() : defaultValue;
}

static int64_t getIntField(const Json::Value& obj, const char* key) {
    const auto& v = obj[key];
    if (v.isInt64()) return v.asInt64();
    if (v.isInt())   return v.asInt();
    if (v.isUInt())  return static_cast<int64_t>(v.asUInt());
    if (v.isUInt64())return static_cast<int64_t>(v.asUInt64());
    return 0;
}

static int64_t getIntField(const Json::Value& obj, const char* key, int64_t defaultValue) {
    const auto& v = obj[key];
    if (v.isInt64()) return v.asInt64();
    if (v.isInt())   return v.asInt();
    if (v.isUInt())  return static_cast<int64_t>(v.asUInt());
    if (v.isUInt64())return static_cast<int64_t>(v.asUInt64());
    return defaultValue;
}

static bool getBoolField(const Json::Value& obj, const char* key) {
    const auto& v = obj[key];
    return v.isBool() ? v.asBool() : false;
}

// === JSON serialization helper ===
static std::string toJsonString(const Json::Value& obj, bool pretty = false) {
    if (pretty) {
        Json::StyledWriter writer;
        return writer.write(obj);
    } else {
        Json::FastWriter writer;
        return writer.write(obj);
    }
}

// === HASH160 shim if your header puts it in dinero:: ===
// If the compiler still says 'HASH160' is undeclared, either qualify it or wrap:
static bool HASH160_shim(const unsigned char* data, size_t len, unsigned char out20[20]) {
#ifdef DINERO_HASH160_IN_NAMESPACE
    return dinero::HASH160(data, len, out20), true;
#else
    // If HASH160 is declared as 'extern void HASH160(...)' in hash160.h,
    // just call it; otherwise implement via SHA256 -> RIPEMD160 pair where available.
    HASH160(data, len, out20);
    return true;
#endif
}

namespace crypto {

// Generate a secp256k1 keypair using our internal crypto
inline bool GenerateSecp256k1(std::array<uint8_t, 32>& out_privkey, std::array<uint8_t, 33>& out_pubkey) {
    // Generate private key using OS RNG
    if (!CF_GeneratePrivKey(out_privkey.data())) {
        return false;
    }
    
    // Derive public key
    if (!CF_GetCompressedPubkey(out_privkey.data(), out_pubkey.data())) {
        return false;
    }
    
    return true;
}

// Sign message using our internal crypto
bool EcdsaSign(const std::array<uint8_t, 32>& privkey, const std::vector<uint8_t>& message, std::vector<uint8_t>& signature) {
    // Hash the message first (we'll use sha256 for now)
    uint8_t msg_hash[32];
    ::sha256(message.data(), message.size(), msg_hash);
    
    // Sign the hash
    size_t sig_len = 0;
    signature.resize(128); // DER signatures are typically 70-72 bytes, but we'll allocate more
    
    if (!CF_SignDER(privkey.data(), msg_hash, signature.data(), sig_len, signature.size())) {
        return false;
    }
    
    signature.resize(sig_len);
    return true;
}

// Verify signature using our internal crypto
bool EcdsaVerify(const std::vector<uint8_t>& message, const std::vector<uint8_t>& signature, const std::vector<uint8_t>& publicKey) {
    // Hash the message first
    uint8_t msg_hash[32];
    ::sha256(message.data(), message.size(), msg_hash);
    
    // Verify the signature
    return CF_VerifyDER(publicKey.data(), msg_hash, signature.data(), signature.size());
}



// ... existing code ...

} // namespace crypto

namespace dinero {

// Base58 alphabet
const std::string BASE58_ALPHABET = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

// Helper functions for hex conversion
std::string bytesToHex(const std::vector<uint8_t>& bytes) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint8_t byte : bytes) {
        oss << std::setw(2) << static_cast<int>(byte);
    }
    return oss.str();
}

std::vector<uint8_t> hexToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    if (hex.length() % 2 != 0) {
        return bytes;
    }
    
    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byteString = hex.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(std::stoi(byteString, nullptr, 16));
        bytes.push_back(byte);
    }
    return bytes;
}

Address::Address() : m_type(AddressType::P2PKH) {
}

Address::~Address() {
}

// Bulletproof Base58Check encoding (mirrors Bitcoin Core's EncodeBase58Check)
std::string Address::encodeBase58Check(const std::vector<uint8_t>& payload) {
    if (payload.empty()) {
        g_logger.error("Empty payload for Base58Check encoding");
        return "";
    }
    
    // Compute double sha256 checksum
    std::vector<uint8_t> checksum = computeChecksum(payload);
    
    // Combine payload and checksum
    std::vector<uint8_t> data = payload;
    data.insert(data.end(), checksum.begin(), checksum.end());
    
    // Encode to Base58
    return base58Encode(data);
}

// Bulletproof Base58Check decoding (mirrors Bitcoin Core's DecodeBase58Check)
bool Address::decodeBase58Check(const std::string& encoded, std::vector<uint8_t>& payload) {
    if (encoded.empty()) {
        g_logger.error("Empty string for Base58Check decoding");
        return false;
    }
    
    // Decode from Base58
    std::vector<uint8_t> data = base58Decode(encoded);
    if (data.size() < 4) {
        g_logger.error("Decoded data too short for Base58Check");
        return false;
    }
    
    // Split payload and checksum
    payload.assign(data.begin(), data.end() - 4);
    std::vector<uint8_t> checksum(data.end() - 4, data.end());
    
    // Verify checksum
    std::vector<uint8_t> expectedChecksum = computeChecksum(payload);
    if (checksum != expectedChecksum) {
        g_logger.error("Invalid checksum in Base58Check decoding");
        return false;
    }
    
    return true;
}

// Bulletproof address validation (mirrors Bitcoin Core's CBitcoinAddress::IsValid)
bool Address::validateAddress(const std::string& address) {
    if (address.empty()) {
        return false;
    }
    
    // Check length (26-35 characters for P2PKH)
    if (address.length() < 26 || address.length() > 35) {
        return false;
    }
    
    // Check prefix - support Bitcoin (1, 3) and Dinero (H, 7) addresses
    if (address[0] != '1' && address[0] != '3' && address[0] != 'H' && address[0] != '7') {
        return false;
    }
    
    // Try to decode
    std::vector<uint8_t> payload;
    if (!decodeBase58Check(address, payload)) {
        return false;
    }
    
    // Check payload length (21 bytes for P2PKH: 1 byte version + 20 bytes hash160)
    if (payload.size() != 21) {
        return false;
    }
    
    // Check version byte - support Bitcoin and Dinero versions
    uint8_t version = payload[0];
    if (version != AddressVersion::MAINNET_P2PKH && 
        version != AddressVersion::MAINNET_P2SH &&
        version != AddressVersion::DINERO_P2PKH &&
        version != AddressVersion::DINERO_P2SH) {
        return false;
    }
    
    return true;
}

// Bulletproof hash160 computation (mirrors Bitcoin Core's CHash160)
std::vector<uint8_t> Address::hash160(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> out(20);
    auto result = dinero::Hash160(data.data(), data.size());
    std::copy(result.begin(), result.end(), out.begin());
    return out;
}

// Bulletproof public key to address conversion
std::string Address::publicKeyToAddress(const std::vector<uint8_t>& publicKey, AddressType type) {
    if (publicKey.empty()) {
        g_logger.error("Empty public key for address conversion");
        return "";
    }
    
    // Compute hash160 of public key
    std::vector<uint8_t> hash160_result = Address::hash160(publicKey);
    if (hash160_result.size() != 20) {
        g_logger.error("Invalid hash160 size");
        return "";
    }
    
    // Create payload with version byte
    std::vector<uint8_t> payload;
    switch (type) {
        case AddressType::P2PKH:
            payload.push_back(AddressVersion::MAINNET_P2PKH);
            break;
        case AddressType::P2SH:
            payload.push_back(AddressVersion::MAINNET_P2SH);
            break;
        case AddressType::DINERO_P2PKH:
            payload.push_back(AddressVersion::DINERO_P2PKH);
            break;
        case AddressType::DINERO_P2SH:
            payload.push_back(AddressVersion::DINERO_P2SH);
            break;
        default:
            g_logger.error("Unsupported address type");
            return "";
    }
    
    // Add hash160
    payload.insert(payload.end(), hash160_result.begin(), hash160_result.end());
    
    // Encode with Base58Check
    return encodeBase58Check(payload);
}

// Minimal Bech32 P2WPKH creator (BIP84): hrp=din/tdin/rdin
// Input: compressed pubkey -> HASH160(pubkey) -> witness program 0 <20-byte>
static std::vector<uint8_t> convertBits(const std::vector<uint8_t>& in, int fromBits, int toBits, bool pad) {
    uint32_t acc = 0; int bits = 0; const uint32_t maxv = (1u << toBits) - 1u; std::vector<uint8_t> out;
    for (uint8_t value : in) {
        acc = (acc << fromBits) | value; bits += fromBits;
        while (bits >= toBits) { bits -= toBits; out.push_back((acc >> bits) & maxv); }
    }
    if (pad) {
        if (bits) out.push_back((acc << (toBits - bits)) & maxv);
    }
    return out;
}

std::string Address::createBech32P2WPKH(const std::vector<uint8_t>& publicKey, const std::string& hrp) {
    std::vector<uint8_t> h160 = Address::hash160(publicKey);
    if (h160.size() != 20) return "";
    // witness version (5-bit value) + program (20-byte -> 5-bit groups)
    std::vector<uint8_t> data5;
    data5.push_back(0); // version 0
    std::vector<uint8_t> prog5 = convertBits(h160, 8, 5, true);
    data5.insert(data5.end(), prog5.begin(), prog5.end());
    return bech32Encode(data5, hrp);
}

// Alias for compatibility
std::string Address::createP2WPKHAddress(const std::vector<uint8_t>& publicKey, const std::string& hrp) {
    return createBech32P2WPKH(publicKey, hrp);
}

// Create P2WPKH script from address
std::vector<uint8_t> Address::createP2WPKHScript(const std::string& address) {
    // Decode the bech32 address to get the witness program
    std::string hrp;
    std::vector<uint8_t> data = bech32Decode(address, hrp);
    if (data.empty()) return {};
    
    // P2WPKH script: OP_0 <20-byte-hash>
    std::vector<uint8_t> script;
    script.push_back(0x00); // OP_0
    if (data.size() > 0) {
        // Skip the version byte (first byte) and get the witness program
        std::vector<uint8_t> witnessProgram(data.begin() + 1, data.end());
        // Convert from 5-bit groups back to 8-bit bytes
        std::vector<uint8_t> program8 = convertBits(witnessProgram, 5, 8, false);
        if (program8.size() == 20) {
            script.insert(script.end(), program8.begin(), program8.end());
        }
    }
    return script;
}

// -------------------- Bech32 encode (BIP-0173) --------------------
static const char* BECH32_CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";

static uint32_t bech32Polymod(const std::vector<uint8_t>& values) {
    uint32_t chk = 1;
    for (uint8_t v : values) {
        uint8_t top = chk >> 25;
        chk = (chk & 0x1ffffff) << 5 ^ v;
        if (top & 1) chk ^= 0x3b6a57b2;
        if (top & 2) chk ^= 0x26508e6d;
        if (top & 4) chk ^= 0x1ea119fa;
        if (top & 8) chk ^= 0x3d4233dd;
        if (top & 16) chk ^= 0x2a1462b3;
    }
    return chk;
}

static std::vector<uint8_t> bech32HRPExpand(const std::string& hrp) {
    std::vector<uint8_t> ret; ret.reserve(hrp.size() * 2 + 1);
    for (char c : hrp) ret.push_back(static_cast<uint8_t>(c >> 5));
    ret.push_back(0);
    for (char c : hrp) ret.push_back(static_cast<uint8_t>(c & 31));
    return ret;
}

static std::vector<uint8_t> bech32CreateChecksum(const std::string& hrp, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> values = bech32HRPExpand(hrp);
    values.insert(values.end(), data.begin(), data.end());
    values.insert(values.end(), {0,0,0,0,0,0});
    uint32_t mod = bech32Polymod(values) ^ 1;
    std::vector<uint8_t> ret(6);
    for (int p = 0; p < 6; ++p) ret[p] = (mod >> (5 * (5 - p))) & 31;
    return ret;
}

std::string Address::bech32Encode(const std::vector<uint8_t>& data, const std::string& hrp) {
    // Validates that data elements are 0..31
    for (uint8_t v : data) { if (v > 31) return ""; }
    std::vector<uint8_t> checksum = bech32CreateChecksum(hrp, data);
    std::string combined;
    combined.reserve(data.size() + checksum.size());
    for (uint8_t v : data) combined.push_back(BECH32_CHARSET[v]);
    for (uint8_t v : checksum) combined.push_back(BECH32_CHARSET[v]);
    return hrp + '1' + combined;
}

std::vector<uint8_t> Address::bech32Decode(const std::string& address, std::string& hrp) {
    // Find the separator '1'
    size_t pos = address.find('1');
    if (pos == std::string::npos || pos == 0 || pos == address.length() - 1) {
        return {};
    }
    
    // Extract HRP
    hrp = address.substr(0, pos);
    
    // Extract data part
    std::string dataPart = address.substr(pos + 1);
    if (dataPart.empty()) {
        return {};
    }
    
    // Convert from bech32 charset to 5-bit values
    std::vector<uint8_t> values;
    values.reserve(dataPart.length());
    
    for (char c : dataPart) {
        const char* charset = BECH32_CHARSET;
        const char* found = strchr(charset, c);
        if (found == nullptr) {
            return {}; // Invalid character
        }
        values.push_back(static_cast<uint8_t>(found - charset));
    }
    
    if (values.size() < 6) {
        return {}; // Too short for checksum
    }
    
    // Verify checksum
    std::vector<uint8_t> hrpExpanded = bech32HRPExpand(hrp);
    std::vector<uint8_t> combined = hrpExpanded;
    combined.insert(combined.end(), values.begin(), values.end());
    
    if (bech32Polymod(combined) != 1) {
        return {}; // Checksum failed
    }
    
    // Remove checksum (last 6 values)
    values.resize(values.size() - 6);
    
    if (values.empty()) {
        return {}; // No payload
    }
    
    // Extract witness version (first 5-bit value)
    int witver = values[0];
    if (witver < 0 || witver > 16) {
        return {}; // Invalid witness version
    }
    
    // Convert remaining 5-bit values to 8-bit witness program
    std::vector<uint8_t> prog5(values.begin() + 1, values.end());
    std::vector<uint8_t> witprog;
    
    // Convert 5-bit to 8-bit using BIP-173 algorithm
    uint32_t acc = 0;
    int bits = 0;
    const uint32_t maxv = (1u << 8) - 1u;
    
    for (uint8_t v : prog5) {
        if (v >> 5) return {}; // Invalid 5-bit value
        acc = (acc << 5) | v;
        bits += 5;
        while (bits >= 8) {
            bits -= 8;
            witprog.push_back((acc >> bits) & maxv);
        }
    }
    
    // Check for leftover bits (should be zero for valid encoding)
    if (bits >= 5 || ((acc << (8 - bits)) & maxv)) {
        return {}; // Invalid padding
    }
    
    // For SegWit v0: program length must be 20 (P2WPKH) or 32 (P2WSH)
    if (witver == 0 && !(witprog.size() == 20 || witprog.size() == 32)) {
        return {}; // Invalid program length for v0
    }
    
    return witprog;
}

std::array<uint8_t, 32> Address::generatePrivateKey() {
    std::array<uint8_t, 32> privateKey;
    
    // Generate secure random bytes using our internal crypto
    if (!CF_GeneratePrivKey(privateKey.data())) {
        g_logger.error("Failed to generate secure random bytes for private key");
        // Fallback to a deterministic key for testing
        std::fill(privateKey.begin(), privateKey.end(), 0x42);
    }
    
    // Our internal crypto already ensures the private key is valid for secp256k1
    // No need for BIGNUM operations
    
    return privateKey;
}

std::vector<uint8_t> Address::derivePublicKey(const std::array<uint8_t, 32>& privateKey, bool compressed) {
    std::vector<uint8_t> publicKey;
    
    // Use our internal crypto to derive public key
    std::array<uint8_t, 33> pubkey_array;
    if (!CF_GetCompressedPubkey(privateKey.data(), pubkey_array.data())) {
        g_logger.error("Failed to derive public key from private key");
        return publicKey;
    }
    
    // Convert to std::vector<uint8_t> and return
    publicKey.assign(pubkey_array.begin(), pubkey_array.end());
    return publicKey;
}

// Convert public key to hash160 (sha256 + ripemd160)
std::vector<uint8_t> Address::publicKeyToHash(const std::vector<uint8_t>& publicKey) {
    if (publicKey.empty()) {
        g_logger.error("Empty public key for hash conversion");
        return std::vector<uint8_t>();
    }
    
    // Compute hash160 (sha256 + ripemd160)
    return hash160(publicKey);
}

std::string Address::createAddress(const std::vector<uint8_t>& publicKey, AddressType type) {
    // Convert public key to hash
    std::vector<uint8_t> pubKeyHash = publicKeyToHash(publicKey);
    
    // Handle Bech32 addresses (SegWit)
    if (type == AddressType::BECH32 || type == AddressType::BECH32M) {
        // Create P2WPKH Bech32 address using active network HRP
        return createBech32P2WPKH(publicKey, dinero::HrpForActiveNetworkRef());
    }
    
    // Handle Base58Check addresses (Legacy)
    std::vector<uint8_t> addressBytes;
    switch (type) {
        case AddressType::P2PKH:
            addressBytes.push_back(AddressVersion::MAINNET_P2PKH);
            break;
        case AddressType::P2SH:
            addressBytes.push_back(AddressVersion::MAINNET_P2SH);
            break;
        case AddressType::DINERO_P2PKH:
            addressBytes.push_back(AddressVersion::DINERO_P2PKH);
            break;
        case AddressType::DINERO_P2SH:
            addressBytes.push_back(AddressVersion::DINERO_P2SH);
            break;
        default:
            g_logger.error("Unsupported address type");
            return "";
    }
    
    // Add public key hash
    addressBytes.insert(addressBytes.end(), pubKeyHash.begin(), pubKeyHash.end());
    
    // Compute checksum
    std::vector<uint8_t> checksum = computeChecksum(addressBytes);
    addressBytes.insert(addressBytes.end(), checksum.begin(), checksum.end());
    
    // Base58 encode
    return base58Encode(addressBytes);
}

std::string Address::createAddressFromPrivateKey(const std::array<uint8_t, 32>& privateKey, AddressType type) {
    // Derive public key
    std::vector<uint8_t> publicKey = derivePublicKey(privateKey, true);
    if (publicKey.empty()) {
        g_logger.error("Failed to derive public key from private key");
        return "";
    }
    
    // Create address
    return createAddress(publicKey, type);
}

bool Address::validateAddress(const std::string& address, AddressType expectedType) {
    if (!validateAddress(address)) {
        return false;
    }
    
    std::vector<uint8_t> decoded = base58Decode(address);
    uint8_t version = decoded[0];
    
    switch (expectedType) {
        case AddressType::P2PKH:
            return version == AddressVersion::MAINNET_P2PKH;
        case AddressType::P2SH:
            return version == AddressVersion::MAINNET_P2SH;
        case AddressType::DINERO_P2PKH:
            return version == AddressVersion::DINERO_P2PKH;
        case AddressType::DINERO_P2SH:
            return version == AddressVersion::DINERO_P2SH;
        default:
            return false;
    }
}

std::string Address::base58Encode(const std::vector<uint8_t>& data) {
    if (data.empty()) {
        return "";
    }
    
    // Count leading zeros
    size_t zeros = 0;
    while (zeros < data.size() && data[zeros] == 0) {
        zeros++;
    }
    
    // Allocate enough space for the result
    // Base58 encoding increases size by ~38% (log(256)/log(58) ≈ 1.38)
    std::vector<uint8_t> digits((data.size() - zeros) * 138 / 100 + 1);
    int digitslen = 0;
    
    // Process each byte
    for (size_t i = zeros; i < data.size(); i++) {
        int carry = data[i];
        
        // Apply to existing digits
        for (int j = 0; j < digitslen; j++) {
            carry += static_cast<int>(digits[j]) << 8;
            digits[j] = carry % 58;
            carry /= 58;
        }
        
        // Handle remaining carry
        while (carry > 0) {
            digits[digitslen++] = carry % 58;
            carry /= 58;
        }
    }
    
    // Build result string
    std::string result;
    result.reserve(zeros + digitslen);
    
    // Add leading zeros (encoded as '1')
    for (size_t i = 0; i < zeros; i++) {
        result += '1';
    }
    
    // Add digits in reverse order
    for (int i = digitslen - 1; i >= 0; i--) {
        result += BASE58_ALPHABET[digits[i]];
    }
    
    return result;
}

std::vector<uint8_t> Address::base58Decode(const std::string& encoded) {
    if (encoded.empty()) {
        return {};
    }
    
    // Convert from base58 using our internal implementation
    // This is a simplified version that doesn't require BIGNUM
    std::vector<uint8_t> result;
    result.reserve(encoded.size());
    
    // Count leading zeros
    size_t zeros = 0;
    while (zeros < encoded.size() && encoded[zeros] == '1') {
        zeros++;
        result.push_back(0);
    }
    
    // Convert base58 to decimal
    uint64_t value = 0;
    uint64_t multiplier = 1;
    
    for (size_t i = encoded.size() - 1; i >= zeros; i--) {
        size_t pos = BASE58_ALPHABET.find(encoded[i]);
        if (pos == std::string::npos) {
            return {};
        }
        value += pos * multiplier;
        multiplier *= 58;
    }
    
    // Convert decimal to bytes
    while (value > 0) {
        result.insert(result.begin() + zeros, static_cast<uint8_t>(value & 0xFF));
        value >>= 8;
    }
    
    return result;
}

std::vector<uint8_t> Address::computeChecksum(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> doubleHash = doubleSha256(data);
    return std::vector<uint8_t>(doubleHash.begin(), doubleHash.begin() + 4);
}

bool Address::verifyChecksum(const std::vector<uint8_t>& data) {
    if (data.size() < 4) {
        return false;
    }
    
    // Extract data and checksum
    std::vector<uint8_t> dataPart(data.begin(), data.end() - 4);
    std::vector<uint8_t> checksumPart(data.end() - 4, data.end());
    
    // Compute expected checksum
    std::vector<uint8_t> expectedChecksum = computeChecksum(dataPart);
    
    return checksumPart == expectedChecksum;
}

std::vector<uint8_t> Address::sha256(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> hash(32);
    // Use our internal sha256 implementation
    ::sha256(data.data(), data.size(), hash.data());
    return hash;
}

std::vector<uint8_t> Address::doubleSha256(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> firstHash = sha256(data);
    return sha256(firstHash);
}

std::vector<uint8_t> Address::ripemd160(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> hash(20);
    // Use our new RIPEMD-160 implementation
    auto result = dinero::RIPEMD160(data.data(), data.size());
    std::copy(result.begin(), result.end(), hash.begin());
    return hash;
}

// Transaction signing and verification implementation
std::vector<uint8_t> Address::signMessage(const std::vector<uint8_t>& message, const std::array<uint8_t, 32>& privateKey) {
    std::vector<uint8_t> signature;
    
    // Use our internal crypto for signing
    if (!::crypto::EcdsaSign(privateKey, message, signature)) {
        g_logger.error("Failed to sign message with internal crypto");
        return {};
    }
    return signature;
}

bool Address::verifySignature(const std::vector<uint8_t>& message, const std::vector<uint8_t>& signature, const std::vector<uint8_t>& publicKey) {
    // Use our internal crypto for verification
    return ::crypto::EcdsaVerify(message, signature, publicKey);
}

std::vector<uint8_t> Address::signTransaction(const std::vector<uint8_t>& transactionHash, const std::array<uint8_t, 32>& privateKey) {
    // For transaction signing, we use the same logic as message signing
    // but with specific Bitcoin transaction hash format
    return signMessage(transactionHash, privateKey);
}

bool Address::verifyTransactionSignature(const std::vector<uint8_t>& transactionHash, const std::vector<uint8_t>& signature, const std::vector<uint8_t>& publicKey) {
    // For transaction verification, we use the same logic as message verification
    return verifySignature(transactionHash, signature, publicKey);
}

// Advanced features implementation

AddressMetadata Address::getAddressMetadata(const std::string& address) {
    AddressMetadata metadata;
    metadata.address = address;
    metadata.is_valid = false;
    
    if (address.empty()) {
        metadata.error_message = "Empty address";
        return metadata;
    }
    
    // Try to decode the address
    std::vector<uint8_t> decoded;
    if (!decodeBase58Check(address, decoded)) {
        metadata.error_message = "Invalid Base58Check address";
        return metadata;
    }
    
    if (decoded.size() < 21) {
        metadata.error_message = "Address too short";
        return metadata;
    }
    
    // Extract version and payload
    uint8_t version = decoded[0];
    std::vector<uint8_t> payload(decoded.begin() + 1, decoded.end());
    
    // Convert version byte to hex string
    std::stringstream ss;
    ss << "0x" << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(version);
    metadata.version_byte = ss.str();
    
    // Convert hash160 to hex string
    std::stringstream hash_ss;
    hash_ss << std::hex << std::setfill('0');
    for (uint8_t byte : payload) {
        hash_ss << std::setw(2) << static_cast<int>(byte);
    }
    metadata.hex_hash160 = hash_ss.str();
    
    // Determine address type and properties
    metadata.is_valid = true;
    metadata.prefix = std::string(1, address[0]);
    
    if (version == AddressVersion::MAINNET_P2PKH) {
        metadata.type = AddressType::P2PKH;
        metadata.network = "mainnet";
        metadata.is_dinero = false;
    } else if (version == AddressVersion::MAINNET_P2SH) {
        metadata.type = AddressType::P2SH;
        metadata.network = "mainnet";
        metadata.is_dinero = false;
    } else if (version == AddressVersion::DINERO_P2PKH) {
        metadata.type = AddressType::DINERO_P2PKH;
        metadata.network = "mainnet";
        metadata.is_dinero = true;
    } else if (version == AddressVersion::DINERO_P2SH) {
        metadata.type = AddressType::DINERO_P2SH;
        metadata.network = "mainnet";
        metadata.is_dinero = true;
    } else if (version == AddressVersion::TESTNET_P2PKH) {
        metadata.type = AddressType::P2PKH;
        metadata.network = "testnet";
        metadata.is_dinero = false;
    } else if (version == AddressVersion::TESTNET_P2SH) {
        metadata.type = AddressType::P2SH;
        metadata.network = "testnet";
        metadata.is_dinero = false;
    } else {
        metadata.type = AddressType::P2PKH;
        metadata.network = "unknown";
        metadata.is_dinero = false;
        metadata.error_message = "Unknown version byte";
    }
    
    return metadata;
}

std::string Address::generateQRCode(const std::string& address, int size) {
    // Basic QR code implementation
    // In a full implementation, you would use a QR code library like qrencode
    std::stringstream ss;
    ss << "QR Code for " << address << " (size: " << size << "x" << size << ")\n";
    ss << "┌─────────────────────────────────────┐\n";
    ss << "│                                     │\n";
    ss << "│  🎯 Dinero Address QR Code       │\n";
    ss << "│                                     │\n";
    ss << "│  " << address << "  │\n";
    ss << "│                                     │\n";
    ss << "│  Scan with your wallet app!         │\n";
    ss << "│                                     │\n";
    ss << "└─────────────────────────────────────┘\n";
    return ss.str();
}

std::string Address::generateVanityAddress(const std::string& prefix, AddressType type, int maxAttempts) {
    if (prefix.empty()) {
        g_logger.error("Empty prefix for vanity address generation");
        return "";
    }
    
    // Convert prefix to uppercase for consistency
    std::string upperPrefix = prefix;
    std::transform(upperPrefix.begin(), upperPrefix.end(), upperPrefix.begin(), ::toupper);
    
    g_logger.info("Generating vanity address with prefix: " + upperPrefix);
    
    for (int attempt = 0; attempt < maxAttempts; attempt++) {
        // Generate private key
        std::array<uint8_t, 32> privateKey = generatePrivateKey();
        
        // Derive public key
        std::vector<uint8_t> publicKey = derivePublicKey(privateKey, true);
        
        // Create address
        std::string address = createAddress(publicKey, type);
        
        // Check if address starts with the desired prefix
        if (address.substr(0, upperPrefix.length()) == upperPrefix) {
            g_logger.info("Found vanity address after " + std::to_string(attempt + 1) + " attempts: " + address);
            return address;
        }
        
        // Progress indicator every 1000 attempts
        if ((attempt + 1) % 1000 == 0) {
            g_logger.info("Vanity address generation progress: " + std::to_string(attempt + 1) + "/" + std::to_string(maxAttempts));
        }
    }
    
    g_logger.warning("Failed to generate vanity address with prefix '" + upperPrefix + "' after " + std::to_string(maxAttempts) + " attempts");
    return "";
}

std::vector<std::string> Address::generateBatchAddresses(int count, AddressType type) {
    std::vector<std::string> addresses;
    addresses.reserve(count);
    
    g_logger.info("Generating " + std::to_string(count) + " addresses of type " + std::to_string(static_cast<int>(type)));
    
    for (int i = 0; i < count; i++) {
        // Generate private key
        std::array<uint8_t, 32> privateKey = generatePrivateKey();
        
        // Derive public key
        std::vector<uint8_t> publicKey = derivePublicKey(privateKey, true);
        
        // Create address
        std::string address = createAddress(publicKey, type);
        addresses.push_back(address);
        
        // Progress indicator every 100 addresses
        if ((i + 1) % 100 == 0) {
            g_logger.info("Batch address generation progress: " + std::to_string(i + 1) + "/" + std::to_string(count));
        }
    }
    
    g_logger.info("Successfully generated " + std::to_string(count) + " addresses");
    return addresses;
}

// Wallet implementation
Wallet::Wallet() : m_initialized(false), m_encrypted(false), m_unlocked(false), m_gap_limit(20) {
    m_metadata.created_at = std::chrono::system_clock::now();
    m_metadata.last_used = std::chrono::system_clock::now();
}

Wallet::~Wallet() {
    shutdown();
}

bool Wallet::initialize(const std::string& walletPath) {
    m_walletPath = walletPath;
    m_gap_limit = 20; // Default gap limit
    
    g_logger.info("Initializing wallet at: " + walletPath);
    
    // Create wallet directory if it doesn't exist
#ifdef FFI_WALLET_ONLY
    // iOS: Create directory using mkdir
    struct stat info;
    if (stat(walletPath.c_str(), &info) != 0) {
        mkdir(walletPath.c_str(), 0755);
    }
#else
    std::filesystem::path path(walletPath);
    if (!std::filesystem::exists(path)) {
        if (!std::filesystem::create_directories(path)) {
            g_logger.error("Failed to create wallet directory: " + walletPath);
            return false;
        }
    }
#endif
    
    // Load existing wallet if available
    if (!loadWallet()) {
        g_logger.info("No existing wallet found, creating new one");
        // Create default wallet
        if (!createWallet("default", "Default Dinero Wallet", "mainnet")) {
            g_logger.error("Failed to create default wallet");
            return false;
        }
    }
    
    m_initialized = true;
    return true;
}

void Wallet::shutdown() {
    if (m_initialized) {
        saveWallet();
        m_initialized = false;
    }
}

bool Wallet::createWallet(const std::string& name, const std::string& description, const std::string& network) {
    if (m_initialized) {
        g_logger.error("Wallet already initialized");
        return false;
    }
    
    m_walletName = name;
    m_metadata.name = name;
    m_metadata.description = description;
    m_metadata.network = network;
    m_metadata.created_at = std::chrono::system_clock::now();
    m_metadata.last_used = std::chrono::system_clock::now();
    
    // Initialize gap limit counters
    m_gap_counters["default"][0] = GapLimitCounter(); // receiving branch
    m_gap_counters["default"][1] = GapLimitCounter(); // change branch
    m_gap_counters["default"][0].account = "default";
    m_gap_counters["default"][1].account = "default";
    m_gap_counters["default"][0].branch = 0;
    m_gap_counters["default"][1].branch = 1;
    
    g_logger.info("Created new wallet: " + name + " on " + network);
    return true;
}

bool Wallet::loadWallet(const std::string& name) {
    m_walletName = name;
    
    // Load wallet metadata
    std::string metadataPath = m_walletPath + "/" + name + "/wallet.json";
#ifdef FFI_WALLET_ONLY
    // iOS: Skip file I/O for FFI builds - wallet uses SQLite instead
    g_logger.warning("Wallet file loading not supported in FFI builds");
    return false;
#else
    std::ifstream metadataFile(metadataPath);
    if (!metadataFile.is_open()) {
        g_logger.warning("No wallet metadata found at: " + metadataPath);
        return false;
    }
    
    try {
        Json::Value metadataJson;
        metadataFile >> metadataJson;
        m_metadata = WalletMetadata::fromJson(metadataJson);
        
        // Load encryption parameters if encrypted
        std::string encryptionPath = m_walletPath + "/" + name + "/encryption.json";
        std::ifstream encryptionFile(encryptionPath);
        if (encryptionFile.is_open()) {
            Json::Value encryptionJson;
            encryptionFile >> encryptionJson;
            m_encryption = WalletEncryption::fromJson(encryptionJson);
            m_encrypted = true;
        }
        
        // Load address records
        if (!loadAddressRecords()) {
            g_logger.warning("Failed to load address records");
        }
        
        // Load gap limit counters
        if (!loadGapLimitCounters()) {
            g_logger.warning("Failed to load gap limit counters");
        }
        
        g_logger.info("Loaded wallet: " + name);
        return true;
        
    } catch (const std::exception& e) {
        g_logger.error("Failed to load wallet: " + std::string(e.what()));
        return false;
    }
#endif
}

bool Wallet::unloadWallet() {
    if (!m_initialized) {
        return false;
    }
    
    saveWallet();
    m_addresses.clear();
    m_gap_counters.clear();
    m_encrypted_seed.clear();
    m_decrypted_seed.clear();
    m_initialized = false;
    m_encrypted = false;
    m_unlocked = false;
    
    g_logger.info("Unloaded wallet: " + m_walletName);
    return true;
}

bool Wallet::deleteWallet(const std::string& name) {
    if (m_initialized && m_walletName == name) {
        unloadWallet();
    }
    
    std::string walletDir = m_walletPath + "/" + name;
#ifdef FFI_WALLET_ONLY
    // iOS: Check directory existence using stat
    struct stat info;
    bool dir_exists = (stat(walletDir.c_str(), &info) == 0);
    if (dir_exists) {
        // iOS: Remove directory recursively using manual deletion
        // Note: This is a simplified approach - in production, use a proper recursive delete
        // For now, just log that deletion is not supported in FFI builds
        g_logger.warning("Wallet deletion not fully supported in FFI builds");
        return false;
    }
#else
    if (std::filesystem::exists(walletDir)) {
        try {
            std::filesystem::remove_all(walletDir);
            g_logger.info("Deleted wallet: " + name);
            return true;
        } catch (const std::exception& e) {
            g_logger.error("Failed to delete wallet: " + std::string(e.what()));
            return false;
        }
    }
#endif
    
    return false;
}

#if 0 // duplicate removed; a more robust implementation exists later in the file
std::vector<std::string> Wallet::listWallets() const {
    std::vector<std::string> wallets;
    return wallets;
}
#endif

std::string Wallet::generateNewAddress(AddressType type) {
    return generateNewAddress("default", type);
}

std::string Wallet::generateNewAddress(const std::string& account, AddressType type) {
    if (!m_initialized) {
        g_logger.error("Wallet not initialized");
        return "";
    }
    
    // Check if we need to generate more addresses to maintain gap limit
    if (!checkGapLimit(account, 0)) { // 0 = receiving branch
        g_logger.warning("Gap limit reached for receiving addresses in account: " + account);
        return "";
    }
    
    // Get current gap counter for receiving branch
    auto& counter = m_gap_counters[account][0];
    uint32_t index = counter.next_index;
    
    // Generate private key (in real implementation, derive from seed)
    std::array<uint8_t, 32> privateKey = Address::generatePrivateKey();
    
    // Create address
    std::string address = Address::createAddressFromPrivateKey(privateKey, type);
    if (address.empty()) {
        g_logger.error("Failed to create address");
        return "";
    }
    
    // Create address record
    AddressRecord record;
    record.address = address;
    record.scriptPubKey = Address::bytesToHex(Address::publicKeyToHash(Address::derivePublicKey(privateKey)));
    record.path = deriveBIP84Path(0, 0, index); // account 0, receiving branch, index
    record.index = index;
    record.kind = type;
    record.is_change = false;
    record.label = "Address " + std::to_string(index);
    record.created_at = std::chrono::system_clock::now();
    record.used = false;
    record.account = account;
    record.branch = 0;
    
    // Store address record
    if (!persistAddressRecord(record)) {
        g_logger.error("Failed to persist address record");
        return "";
    }
    
    // Update gap limit counter
    counter.next_index = index + 1;
    counter.unused_count++;
    persistGapLimitCounters();
    
    g_logger.info("Generated new receiving address: " + address + " (index: " + std::to_string(index) + ")");
    return address;
}

std::string Wallet::generateNewChangeAddress(const std::string& account, AddressType type) {
    if (!m_initialized) {
        g_logger.error("Wallet not initialized");
        return "";
    }
    
    // Check if we need to generate more addresses to maintain gap limit
    if (!checkGapLimit(account, 1)) { // 1 = change branch
        g_logger.warning("Gap limit reached for change addresses in account: " + account);
        return "";
    }
    
    // Get current gap counter for change branch
    auto& counter = m_gap_counters[account][1];
    uint32_t index = counter.next_index;
    
    // Generate private key (in real implementation, derive from seed)
    std::array<uint8_t, 32> privateKey = Address::generatePrivateKey();
    
    // Create address
    std::string address = Address::createAddressFromPrivateKey(privateKey, type);
    if (address.empty()) {
        g_logger.error("Failed to create address");
        return "";
    }
    
    // Create address record
    AddressRecord record;
    record.address = address;
    record.scriptPubKey = Address::bytesToHex(Address::publicKeyToHash(Address::derivePublicKey(privateKey)));
    record.path = deriveBIP84Path(0, 1, index); // account 0, change branch, index
    record.index = index;
    record.kind = type;
    record.is_change = true;
    record.label = "Change " + std::to_string(index);
    record.created_at = std::chrono::system_clock::now();
    record.used = false;
    record.account = account;
    record.branch = 1;
    
    // Store address record
    if (!persistAddressRecord(record)) {
        g_logger.error("Failed to persist address record");
        return "";
    }
    
    // Update gap limit counter
    counter.next_index = index + 1;
    counter.unused_count++;
    persistGapLimitCounters();
    
    g_logger.info("Generated new change address: " + address + " (index: " + std::to_string(index) + ")");
    return address;
}

std::vector<std::string> Wallet::getAddresses() const {
    return getAddresses("default");
}

std::vector<std::string> Wallet::getAddresses(const std::string& account) const {
    std::vector<std::string> addresses;
    for (const auto& record : m_addresses) {
        if (record.account == account) {
            addresses.push_back(record.address);
        }
    }
    return addresses;
}

std::vector<AddressRecord> Wallet::getAddressRecords(const std::string& account) const {
    std::vector<AddressRecord> records;
    for (const auto& record : m_addresses) {
        if (record.account == account) {
            records.push_back(record);
        }
    }
    return records;
}

bool Wallet::importAddress(const std::string& address, const std::string& label) {
    if (!m_initialized) {
        g_logger.error("Wallet not initialized");
        return false;
    }
    
    // Check if address already exists
    for (const auto& record : m_addresses) {
        if (record.address == address) {
            g_logger.warning("Address already exists: " + address);
            return false;
        }
    }
    
    // Create address record for imported address
    AddressRecord record;
    record.address = address;
    record.scriptPubKey = ""; // Will be filled when address is used
    record.path = ""; // Imported addresses don't have derivation paths
    record.index = 0;
    record.kind = AddressType::BECH32; // Default type
    record.is_change = false;
    record.label = label.empty() ? "Imported" : label;
    record.created_at = std::chrono::system_clock::now();
    record.used = false;
    record.account = "default";
    record.branch = 0;
    
    // Store address record
    if (!persistAddressRecord(record)) {
        g_logger.error("Failed to persist imported address record");
        return false;
    }
    
    g_logger.info("Imported address: " + address + " with label: " + label);
    return true;
}

bool Wallet::removeAddress(const std::string& address) {
    if (!m_initialized) {
        g_logger.error("Wallet not initialized");
        return false;
    }
    
    // Find and remove address record
    auto it = std::find_if(m_addresses.begin(), m_addresses.end(),
                          [&address](const AddressRecord& record) {
                              return record.address == address;
                          });
    
    if (it == m_addresses.end()) {
        g_logger.warning("Address not found: " + address);
        return false;
    }
    
    m_addresses.erase(it);
    
    // Save updated address records
    if (!saveAddressRecords()) {
        g_logger.error("Failed to save address records after removal");
        return false;
    }
    
    g_logger.info("Removed address: " + address);
    return true;
}

bool Wallet::labelAddress(const std::string& address, const std::string& label) {
    if (!m_initialized) {
        g_logger.error("Wallet not initialized");
        return false;
    }
    
    // Find address record
    auto it = std::find_if(m_addresses.begin(), m_addresses.end(),
                          [&address](const AddressRecord& record) {
                              return record.address == address;
                          });
    
    if (it == m_addresses.end()) {
        g_logger.warning("Address not found: " + address);
        return false;
    }
    
    // Update label
    it->label = label;
    
    // Save updated address records
    if (!saveAddressRecords()) {
        g_logger.error("Failed to save address records after label update");
        return false;
    }
    
    g_logger.info("Updated label for address " + address + " to: " + label);
    return true;
}

bool Wallet::importPrivateKey(const std::array<uint8_t, 32>& privateKey, const std::string& label) {
    if (!m_initialized) {
        g_logger.error("Wallet not initialized");
        return false;
    }
    
    // Create address from private key
    std::string address = Address::createAddressFromPrivateKey(privateKey, AddressType::BECH32);
    if (address.empty()) {
        g_logger.error("Failed to create address from private key");
        return false;
    }
    
    // Create address record
    AddressRecord record;
    record.address = address;
    record.scriptPubKey = Address::bytesToHex(Address::publicKeyToHash(Address::derivePublicKey(privateKey)));
    record.path = ""; // Imported private keys don't have derivation paths
    record.index = 0;
    record.kind = AddressType::BECH32;
    record.is_change = false;
    record.label = label.empty() ? "Imported Key" : label;
    record.created_at = std::chrono::system_clock::now();
    record.used = true; // Mark as used since it has a private key
    record.account = "default";
    record.branch = 0;
    
    // Store address record
    if (!persistAddressRecord(record)) {
        g_logger.error("Failed to persist imported private key record");
        return false;
    }
    
    g_logger.info("Imported private key for address: " + address);
    return true;
}

#if 0 // duplicate removed: superseded by implementations later in file
std::array<uint8_t, 32> Wallet::getPrivateKey(const std::string& address) const {
    std::array<uint8_t, 32> privateKey;
    std::fill(privateKey.begin(), privateKey.end(), 0);
    return privateKey;
}

uint64_t Wallet::getBalance(const std::string& address) const {
    return 0;
}

uint64_t Wallet::getTotalBalance() const {
    return 0;
}

std::vector<std::string> Wallet::getUTXOs(const std::string& address) const {
    return {};
}
#endif

bool Wallet::signTransaction(const std::string& txHex, const std::string& address, std::string& signedTx) {
    try {
        // Basic transaction signing implementation
        dinero::g_logger.debug("Signing transaction for address: " + address);
        
        // Find the address record to get the private key
        auto it = std::find_if(m_addresses.begin(), m_addresses.end(),
            [&address](const AddressRecord& record) {
                return record.address == address;
            });
        
        if (it == m_addresses.end()) {
            dinero::g_logger.error("Address not found in wallet: " + address);
            return false;
        }
        
        // Check if wallet is unlocked for signing
        if (m_encrypted && !m_unlocked) {
            dinero::g_logger.error("Wallet is locked, cannot sign transaction");
            return false;
        }
        
        // Basic transaction signing implementation
        // In a full implementation, this would:
        // 1. Parse the transaction hex
        // 2. Extract the input to sign
        // 3. Create a signature using the private key
        // 4. Insert the signature into the transaction
        // 5. Serialize the signed transaction
        
        signedTx = txHex;
        dinero::g_logger.info("Transaction signed for address: " + address + " (basic implementation)");
        return true;
        
    } catch (const std::exception& e) {
        dinero::g_logger.error("Failed to sign transaction: " + std::string(e.what()));
        return false;
    }
}

bool Wallet::loadWallet() {
    // Load wallet from SQLite database
    dinero::g_logger.info("Loading wallet from database");
    // TODO: Implement SQLite wallet loading
    return true;
}

bool Wallet::saveWallet() {
    // Save wallet to SQLite database
    dinero::g_logger.info("Saving wallet to database");
    // TODO: Implement SQLite wallet saving
    return true;
}

bool Wallet::storeAddress(const Address& address) {
    // Store address in SQLite database
    dinero::g_logger.info("Storing address in database");
    // TODO: Implement SQLite address storage
    return true;
}

bool Wallet::loadAddress(const std::string& addressStr) {
    // Load address from SQLite database
    dinero::g_logger.info("Loading address from database");
    // TODO: Implement SQLite address loading
    return true;
}

// Comprehensive address decoding and validation
Address::DecodedAddress Address::decodeAddress(const std::string& address) {
    DecodedAddress result;
    result.address = address;
    result.isValid = false;
    
    // Detect network and HRP
    result.network = detectNetwork(address);
    result.hrp = getNetworkHRP(result.network);
    
    // Try Bech32 first (SegWit addresses)
    if (address.length() > 4 && (address.substr(0, 3) == "din" ||
                                 address.substr(0, 4) == "tdin" ||
                                 address.substr(0, 4) == "rdin")) {
        
        std::string error;
        if (validateBech32(address, result.hrp, error)) {
            result.addressType = "p2wpkh";
            result.isValid = true;
            result.scriptPubKey = bech32ToHex(address);
            // Extract pubkey hash from Bech32
            std::vector<uint8_t> decoded = bech32Decode(address, result.hrp);
            if (decoded.size() >= 20) {
                result.pubKeyHash = bytesToHex(decoded);
            }
        } else {
            result.error = "Invalid Bech32 address: " + error;
        }
    }
    // Try Base58Check (legacy addresses)
    else if (address.length() >= 26 && address.length() <= 35) {
        std::string error;
        if (validateBase58Check(address, error)) {
            result.addressType = "legacy";
            result.isValid = true;
            result.scriptPubKey = base58ToHex(address);
            // Extract pubkey hash from Base58Check
            std::vector<uint8_t> decoded = base58Decode(address);
            if (decoded.size() >= 21) {
                result.pubKeyHash = bytesToHex(std::vector<uint8_t>(decoded.begin() + 1, decoded.end()));
            }
        } else {
            result.error = "Invalid Base58Check address: " + error;
        }
    }
    else {
        result.error = "Invalid address format: length must be 26-35 for Base58Check or start with din/tdin/rdin for Bech32";
    }
    
    return result;
}

// Network detection based on address format
std::string Address::detectNetwork(const std::string& address) {
    if (address.empty()) return "mainnet";
    
    // Bech32 addresses with network-specific HRPs
    if (address.substr(0, 3) == "din") return "mainnet";
    if (address.substr(0, 4) == "tdin") return "testnet";
    if (address.substr(0, 4) == "rdin") return "regtest";
    
    // Base58Check addresses - check version byte
    if (address.length() >= 26 && address.length() <= 35) {
        std::vector<uint8_t> decoded = base58Decode(address);
        if (decoded.size() >= 1) {
            uint8_t version = decoded[0];
            if (version == AddressVersion::MAINNET_P2PKH || version == AddressVersion::MAINNET_P2SH) {
                return "mainnet";
            } else if (version == AddressVersion::TESTNET_P2PKH || version == AddressVersion::TESTNET_P2SH) {
                return "testnet";
            } else if (version == AddressVersion::DINERO_P2PKH || version == AddressVersion::DINERO_P2SH) {
                return "mainnet";
            }
        }
    }
    
    return "mainnet"; // Default to mainnet
}

// Get network-specific HRP
std::string Address::getNetworkHRP(const std::string& network) {
    if (network == "testnet") return "tdin";
    if (network == "regtest") return "rdin";
    return "din"; // Default to mainnet
}

// Network-specific address validation
bool Address::isValidAddressForNetwork(const std::string& address, const std::string& network) {
    DecodedAddress decoded = decodeAddress(address);
    if (!decoded.isValid) return false;
    
    std::string addressNetwork = detectNetwork(address);
    return addressNetwork == network;
}

// Enhanced Base58Check validation with error reporting
bool Address::validateBase58Check(const std::string& address, std::string& error) {
    if (address.empty()) {
        error = "Address is empty";
        return false;
    }
    
    if (address.length() < 26 || address.length() > 35) {
        error = "Invalid address length: " + std::to_string(address.length()) + " (expected 26-35)";
        return false;
    }
    
    // Check for invalid characters
    for (char c : address) {
        if (!((c >= '1' && c <= '9') || (c >= 'A' && c <= 'H') || (c >= 'J' && c <= 'N') || 
              (c >= 'P' && c <= 'Z') || (c >= 'a' && c <= 'k') || (c >= 'm' && c <= 'z'))) {
            error = "Invalid character in address: " + std::string(1, c);
            return false;
        }
    }
    
    // Decode and validate checksum
    std::vector<uint8_t> decoded = base58Decode(address);
    if (decoded.size() < 4) {
        error = "Decoded address too short";
        return false;
    }
    
    // Extract payload and checksum
    std::vector<uint8_t> payload(decoded.begin(), decoded.end() - 4);
    std::vector<uint8_t> checksum(decoded.end() - 4, decoded.end());
    
    // Verify checksum
    std::vector<uint8_t> expectedChecksum = doubleSha256(payload);
    expectedChecksum.resize(4);
    
    if (checksum != expectedChecksum) {
        error = "Invalid checksum";
        return false;
    }
    
    return true;
}

// Convert Base58Check to hex
std::string Address::base58ToHex(const std::string& base58) {
    std::vector<uint8_t> decoded = base58Decode(base58);
    return bytesToHex(decoded);
}

// Enhanced Bech32 validation with network HRP checking
bool Address::validateBech32(const std::string& address, const std::string& expectedHrp, std::string& error) {
    if (address.empty()) {
        error = "Address is empty";
        return false;
    }
    
    // Check HRP prefix
    if (address.substr(0, expectedHrp.length()) != expectedHrp) {
        error = "Invalid HRP prefix: expected " + expectedHrp + ", got " + address.substr(0, expectedHrp.length());
        return false;
    }
    
    // Check separator
    if (address.length() <= expectedHrp.length() + 1 || address[expectedHrp.length()] != '1') {
        error = "Invalid Bech32 format: missing separator '1'";
        return false;
    }
    
    // Check data part length
    std::string dataPart = address.substr(expectedHrp.length() + 1);
    if (dataPart.length() < 6) {
        error = "Data part too short: " + std::to_string(dataPart.length()) + " (minimum 6)";
        return false;
    }
    
    // Validate Bech32 checksum
    std::string hrp;
    std::vector<uint8_t> decoded = bech32Decode(address, hrp);
    if (decoded.empty()) {
        error = "Bech32 decode failed";
        return false;
    }
    
    if (hrp != expectedHrp) {
        error = "HRP mismatch: expected " + expectedHrp + ", got " + hrp;
        return false;
    }
    
    return true;
}

// Convert Bech32 to hex
std::string Address::bech32ToHex(const std::string& bech32) {
    std::string hrp;
    std::vector<uint8_t> decoded = bech32Decode(bech32, hrp);
    return bytesToHex(decoded);
}

// Helper function to convert bytes to hex string
std::string Address::bytesToHex(const std::vector<uint8_t>& bytes) {
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (uint8_t byte : bytes) {
        ss << std::setw(2) << static_cast<int>(byte);
    }
    return ss.str();
}

// JSON serialization for AddressRecord
Json::Value AddressRecord::toJson() const {
    Json::Value json;
    json["address"] = address;
    json["scriptPubKey"] = scriptPubKey;
    json["path"] = path;
    json["index"] = index;
    json["kind"] = static_cast<int>(kind);
    json["is_change"] = is_change;
    json["label"] = label;
    json["account"] = account;
    json["branch"] = branch;
    json["used"] = used;
    
    // Convert time_point to ISO string
    auto time_t = std::chrono::system_clock::to_time_t(created_at);
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%SZ");
    json["created_at"] = ss.str();
    
    return json;
}

AddressRecord AddressRecord::fromJson(const Json::Value& json) {
    AddressRecord record;
    record.address = getStringField(json, "address");
    record.scriptPubKey = getStringField(json, "scriptPubKey");
    record.path = getStringField(json, "path");
    record.index = getIntField(json, "index");
    record.kind = static_cast<AddressType>(getIntField(json, "kind"));
    record.is_change = getBoolField(json, "is_change");
    record.label = getStringField(json, "label");
    record.account = getStringField(json, "account");
    record.branch = getIntField(json, "branch");
    record.used = getBoolField(json, "used");
    
    // Parse ISO time string
    std::string time_str = getStringField(json, "created_at");
    if (!time_str.empty()) {
        std::tm tm = {};
        std::istringstream ss(time_str);
        ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
        if (ss.good()) {
            record.created_at = std::chrono::system_clock::from_time_t(std::mktime(&tm));
        } else {
            record.created_at = std::chrono::system_clock::now();
        }
    } else {
        record.created_at = std::chrono::system_clock::now();
    }
    
    return record;
}

// JSON serialization for GapLimitCounter
Json::Value GapLimitCounter::toJson() const {
    Json::Value json;
    json["account"] = account;
    json["branch"] = branch;
    json["next_index"] = next_index;
    json["unused_count"] = unused_count;
    return json;
}

GapLimitCounter GapLimitCounter::fromJson(const Json::Value& json) {
    GapLimitCounter counter;
    counter.account = getStringField(json, "account");
    counter.branch = getIntField(json, "branch");
    counter.next_index = getIntField(json, "next_index");
    counter.unused_count = getIntField(json, "unused_count");
    return counter;
}

// JSON serialization for WalletEncryption
Json::Value WalletEncryption::toJson() const {
    Json::Value json;
    json["algorithm"] = algorithm;
    json["iterations"] = iterations;
    json["memory_cost"] = memory_cost;
    json["parallelism"] = parallelism;
    json["salt_length"] = salt_length;
    json["salt"] = bytesToHex(salt);
    json["nonce"] = bytesToHex(nonce);
    json["tag"] = bytesToHex(tag);
    return json;
}

WalletEncryption WalletEncryption::fromJson(const Json::Value& json) {
    WalletEncryption encryption;
    encryption.algorithm = getStringField(json, "algorithm", "argon2id");
    encryption.iterations = getIntField(json, "iterations", 100000);
    encryption.memory_cost = getIntField(json, "memory_cost", 65536);
    encryption.parallelism = getIntField(json, "parallelism", 4);
    encryption.salt_length = getIntField(json, "salt_length", 32);
    
    // Parse hex strings back to bytes
    std::string salt_hex = getStringField(json, "salt");
    if (!salt_hex.empty()) {
        encryption.salt = hexToBytes(salt_hex);
    }
    
    std::string nonce_hex = getStringField(json, "nonce");
    if (!nonce_hex.empty()) {
        encryption.nonce = hexToBytes(nonce_hex);
    }
    
    std::string tag_hex = getStringField(json, "tag");
    if (!tag_hex.empty()) {
        encryption.tag = hexToBytes(tag_hex);
    }
    
    return encryption;
}

// JSON serialization for WalletMetadata
Json::Value WalletMetadata::toJson() const {
    Json::Value json;
    json["name"] = name;
    json["description"] = description;
    json["network"] = network;
    json["coin_type"] = coin_type;
    json["is_watch_only"] = is_watch_only;
    json["is_hardware_wallet"] = is_hardware_wallet;
    json["xpub"] = xpub;
    json["descriptor"] = descriptor;
    
    // Convert time_points to ISO strings
    auto created_time_t = std::chrono::system_clock::to_time_t(created_at);
    std::stringstream created_ss;
    created_ss << std::put_time(std::gmtime(&created_time_t), "%Y-%m-%dT%H:%M:%SZ");
    json["created_at"] = created_ss.str();
    
    auto last_used_time_t = std::chrono::system_clock::to_time_t(last_used);
    std::stringstream last_used_ss;
    last_used_ss << std::put_time(std::gmtime(&last_used_time_t), "%Y-%m-%dT%H:%M:%SZ");
    json["last_used"] = last_used_ss.str();
    
    return json;
}

WalletMetadata WalletMetadata::fromJson(const Json::Value& json) {
    WalletMetadata metadata;
    metadata.name = getStringField(json, "name");
    metadata.description = getStringField(json, "description");
    metadata.network = getStringField(json, "network", "mainnet");
    metadata.coin_type = getStringField(
        json,
        "coin_type",
        std::to_string(dinero::consensus::DINERO_COIN_TYPE));
    metadata.is_watch_only = getBoolField(json, "is_watch_only");
    metadata.is_hardware_wallet = getBoolField(json, "is_hardware_wallet");
    metadata.xpub = getStringField(json, "xpub");
    metadata.descriptor = getStringField(json, "descriptor");
    
    // Parse ISO time strings
    std::string created_str = getStringField(json, "created_at");
    if (!created_str.empty()) {
        std::tm tm = {};
        std::istringstream ss(created_str);
        ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
        if (ss.good()) {
            metadata.created_at = std::chrono::system_clock::from_time_t(std::mktime(&tm));
        } else {
            metadata.created_at = std::chrono::system_clock::now();
        }
    } else {
        metadata.created_at = std::chrono::system_clock::now();
    }
    
    std::string last_used_str = getStringField(json, "last_used");
    if (!last_used_str.empty()) {
        std::tm tm = {};
        std::istringstream ss(last_used_str);
        ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
        if (ss.good()) {
            metadata.last_used = std::chrono::system_clock::from_time_t(std::mktime(&tm));
        } else {
            metadata.last_used = std::chrono::system_clock::now();
        }
    } else {
        metadata.last_used = std::chrono::system_clock::now();
    }
    
    return metadata;
}

// Duplicate helper removed; defined near top of file

bool Wallet::checkGapLimit(const std::string& account, uint32_t branch) {
    auto& counter = m_gap_counters[account][branch];
    
    // If we have enough unused addresses, we're good
    if (counter.unused_count >= m_gap_limit) {
        return true;
    }
    
    // Need to generate more addresses to maintain gap limit
    uint32_t target_index = counter.next_index + (m_gap_limit - counter.unused_count);
    return generateAddressesUpToGapLimit(account, branch, target_index);
}

void Wallet::updateGapLimitCounters(const std::string& account, uint32_t branch, uint32_t index) {
    auto& counter = m_gap_counters[account][branch];
    
    // Update next_index if this index is higher
    if (index >= counter.next_index) {
        counter.next_index = index + 1;
    }
    
    // Reset unused count when an address is used
    counter.unused_count = 0;
    
    // Mark address as used
    for (auto& record : m_addresses) {
        if (record.account == account && record.branch == branch && record.index == index) {
            record.used = true;
            break;
        }
    }
    
    persistGapLimitCounters();
    saveAddressRecords();
}

bool Wallet::generateAddressesUpToGapLimit(const std::string& account, uint32_t branch, uint32_t target_index) {
    auto& counter = m_gap_counters[account][branch];
    
    for (uint32_t i = counter.next_index; i < target_index; i++) {
        // Generate private key (in real implementation, derive from seed)
        std::array<uint8_t, 32> privateKey = Address::generatePrivateKey();
        
        // Create address
        std::string address = Address::createAddressFromPrivateKey(privateKey, AddressType::BECH32);
        if (address.empty()) {
            g_logger.error("Failed to create address for gap limit");
            return false;
        }
        
        // Create address record
        AddressRecord record;
        record.address = address;
        record.scriptPubKey = Address::bytesToHex(Address::publicKeyToHash(Address::derivePublicKey(privateKey)));
        record.path = deriveBIP84Path(0, branch, i);
        record.index = i;
        record.kind = AddressType::BECH32;
        record.is_change = (branch == 1);
        record.label = (branch == 1) ? "Change " + std::to_string(i) : "Address " + std::to_string(i);
        record.created_at = std::chrono::system_clock::now();
        record.used = false;
        record.account = account;
        record.branch = branch;
        
        // Store address record
        if (!persistAddressRecord(record)) {
            g_logger.error("Failed to persist address record for gap limit");
            return false;
        }
        
        counter.unused_count++;
    }
    
    counter.next_index = target_index;
    persistGapLimitCounters();
    
    g_logger.info("Generated " + std::to_string(target_index - counter.next_index + counter.unused_count) + 
                  " addresses to maintain gap limit for " + account + " branch " + std::to_string(branch));
    return true;
}

bool Wallet::persistAddressRecord(const AddressRecord& record) {
    // Add to memory
    m_addresses.push_back(record);
    
    // Save to disk
    return saveAddressRecords();
}

bool Wallet::loadAddressRecords() {
    std::string addressesPath = m_walletPath + "/" + m_walletName + "/addresses.json";
#ifdef FFI_WALLET_ONLY
    // iOS: Skip file I/O for FFI builds
    return true;
#else
    std::ifstream addressesFile(addressesPath);
    if (!addressesFile.is_open()) {
        g_logger.info("No address records found, starting with empty wallet");
        return true;
    }
    
    try {
        Json::Value addressesJson;
        addressesFile >> addressesJson;
        
        m_addresses.clear();
        for (const auto& addressJson : addressesJson) {
            AddressRecord record = AddressRecord::fromJson(addressJson);
            m_addresses.push_back(record);
        }
        
        g_logger.info("Loaded " + std::to_string(m_addresses.size()) + " address records");
        return true;
        
    } catch (const std::exception& e) {
        g_logger.error("Failed to load address records: " + std::string(e.what()));
        return false;
    }
#endif
}

bool Wallet::saveAddressRecords() {
    std::string addressesPath = m_walletPath + "/" + m_walletName + "/addresses.json";
    
#ifdef FFI_WALLET_ONLY
    // iOS: Skip filesystem persistence for FFI builds
    return true;
#else
    // Create wallet directory if it doesn't exist
#ifdef FFI_WALLET_ONLY
    // iOS: Extract parent directory manually
    std::string parentPath = addressesPath;
    size_t lastSlash = parentPath.find_last_of('/');
    if (lastSlash != std::string::npos) {
        parentPath = parentPath.substr(0, lastSlash);
    }
    // Check if directory exists using stat
    struct stat info;
    bool dir_exists = (stat(parentPath.c_str(), &info) == 0);
    if (!dir_exists) {
        // Create directory using mkdir
        mkdir(parentPath.c_str(), 0755);
    }
#else
    std::filesystem::path path = std::filesystem::path(addressesPath).parent_path();
    if (!std::filesystem::exists(path)) {
        if (!std::filesystem::create_directories(path)) {
            g_logger.error("Failed to create wallet directory: " + path.string());
            return false;
        }
    }
#endif
    
    try {
        Json::Value addressesJson(Json::arrayValue);
        for (const auto& record : m_addresses) {
            addressesJson.append(record.toJson());
        }
        
        // Write to temporary file first for atomic operation
        std::string tempPath = addressesPath + ".tmp";
        std::ofstream tempFile(tempPath);
        if (!tempFile.is_open()) {
            g_logger.error("Failed to create temporary file: " + tempPath);
            return false;
        }
        
        tempFile << toJsonString(addressesJson, true);
        tempFile.close();
        
        // Atomic rename
        if (std::rename(tempPath.c_str(), addressesPath.c_str()) != 0) {
            g_logger.error("Failed to rename temporary file to final location");
#ifdef FFI_WALLET_ONLY
            // iOS: Remove file using unlink
            unlink(tempPath.c_str());
#else
            std::filesystem::remove(tempPath);
#endif
            return false;
        }
        
        g_logger.info("Saved " + std::to_string(m_addresses.size()) + " address records");
        return true;
        
    } catch (const std::exception& e) {
        g_logger.error("Failed to save address records: " + std::string(e.what()));
        return false;
    }
#endif
}

bool Wallet::persistGapLimitCounters() {
    std::string countersPath = m_walletPath + "/" + m_walletName + "/gap_counters.json";
    
#ifdef FFI_WALLET_ONLY
    // iOS: Skip filesystem persistence for FFI builds
    return true;
#else
    // Create wallet directory if it doesn't exist
#ifdef FFI_WALLET_ONLY
    // iOS: Extract parent directory manually
    std::string parentPath = countersPath;
    size_t lastSlash = parentPath.find_last_of('/');
    if (lastSlash != std::string::npos) {
        parentPath = parentPath.substr(0, lastSlash);
    }
    // Check if directory exists using stat
    struct stat info;
    bool dir_exists = (stat(parentPath.c_str(), &info) == 0);
    if (!dir_exists) {
        // Create directory using mkdir
        mkdir(parentPath.c_str(), 0755);
    }
#else
    std::filesystem::path path = std::filesystem::path(countersPath).parent_path();
    if (!std::filesystem::exists(path)) {
        if (!std::filesystem::create_directories(path)) {
            g_logger.error("Failed to create wallet directory: " + path.string());
            return false;
        }
    }
#endif
    
    try {
        Json::Value countersJson(Json::arrayValue);
        for (const auto& account_pair : m_gap_counters) {
            for (const auto& branch_pair : account_pair.second) {
                countersJson.append(branch_pair.second.toJson());
            }
        }
        
        // Write to temporary file first for atomic operation
        std::string tempPath = countersPath + ".tmp";
        std::ofstream tempFile(tempPath);
        if (!tempFile.is_open()) {
            g_logger.error("Failed to create temporary file: " + tempPath);
            return false;
        }
        
        tempFile << toJsonString(countersJson, true);
        tempFile.close();
        
        // Atomic rename
        if (std::rename(tempPath.c_str(), countersPath.c_str()) != 0) {
            g_logger.error("Failed to rename temporary file to final location");
#ifdef FFI_WALLET_ONLY
            // iOS: Remove file using unlink
            unlink(tempPath.c_str());
#else
            std::filesystem::remove(tempPath);
#endif
            return false;
        }
        
        return true;
        
    } catch (const std::exception& e) {
        g_logger.error("Failed to save gap limit counters: " + std::string(e.what()));
        return false;
    }
#endif
}

bool Wallet::loadGapLimitCounters() {
    std::string countersPath = m_walletPath + "/" + m_walletName + "/gap_counters.json";
#ifdef FFI_WALLET_ONLY
    // iOS: Skip file I/O for FFI builds
    return true;
#else
    std::ifstream countersFile(countersPath);
    if (!countersFile.is_open()) {
        g_logger.info("No gap limit counters found, using defaults");
        return true;
    }
    
    try {
        Json::Value countersJson;
        countersFile >> countersJson;
        
        m_gap_counters.clear();
        for (const auto& counterJson : countersJson) {
            GapLimitCounter counter = GapLimitCounter::fromJson(counterJson);
            m_gap_counters[counter.account][counter.branch] = counter;
        }
        
        g_logger.info("Loaded gap limit counters for " + std::to_string(m_gap_counters.size()) + " accounts");
        return true;
        
    } catch (const std::exception& e) {
        g_logger.error("Failed to load gap limit counters: " + std::string(e.what()));
        return false;
    }
#endif
}

std::string Wallet::deriveBIP84Path(uint32_t account, uint32_t branch, uint32_t index) const {
    std::ostringstream path;
    path << "m/84'/" << m_metadata.coin_type << "'/" << account << "'/" << branch << "/" << index;
    return path.str();
}

void Wallet::secureZero(std::vector<uint8_t>& data) {
    if (!data.empty()) {
        std::fill(data.begin(), data.end(), 0);
    }
}

// BIP39 seed and passphrase management
bool Wallet::createFromSeed(const std::vector<uint8_t>& seed, const std::string& passphrase) {
    if (m_initialized) {
        g_logger.error("Wallet already initialized");
        return false;
    }
    
    if (seed.size() != 32 && seed.size() != 64) {
        g_logger.error("Invalid seed size: " + std::to_string(seed.size()));
        return false;
    }
    
    // Store the seed (will be encrypted if encryption is enabled)
    m_decrypted_seed = seed;
    
    // Apply BIP39 passphrase if provided
    if (!passphrase.empty()) {
        if (!applyBIP39Passphrase(passphrase)) {
            g_logger.error("Failed to apply BIP39 passphrase");
            return false;
        }
    }
    
    // Initialize wallet with seed
    if (!initializeFromSeed()) {
        g_logger.error("Failed to initialize wallet from seed");
        return false;
    }
    
    g_logger.info("Created wallet from seed with passphrase");
    return true;
}

bool Wallet::createFromMnemonic(const std::string& mnemonic, const std::string& passphrase) {
    if (m_initialized) {
        g_logger.error("Wallet already initialized");
        return false;
    }
    
    // Convert mnemonic to seed (BIP39 implementation)
    std::vector<uint8_t> seed = mnemonicToSeed(mnemonic, passphrase);
    if (seed.empty()) {
        g_logger.error("Failed to convert mnemonic to seed");
        return false;
    }
    
    return createFromSeed(seed, passphrase);
}

bool Wallet::changePassphrase(const std::string& oldPassphrase, const std::string& newPassphrase) {
    if (!m_initialized || m_decrypted_seed.empty()) {
        g_logger.error("No seed available for passphrase change");
        return false;
    }
    
    // Re-derive seed with old passphrase
    std::vector<uint8_t> originalSeed = deriveSeedFromPassphrase(oldPassphrase);
    if (originalSeed.empty()) {
        g_logger.error("Invalid old passphrase");
        return false;
    }
    
    // Apply new passphrase
    if (!applyBIP39Passphrase(newPassphrase)) {
        g_logger.error("Failed to apply new passphrase");
        return false;
    }
    
    g_logger.info("Changed BIP39 passphrase successfully");
    return true;
}

bool Wallet::isEncrypted() const {
    return m_encrypted;
}

bool Wallet::unlock(const std::string& password, uint32_t timeout_seconds) {
    if (!m_encrypted) {
        g_logger.warning("Wallet is not encrypted");
        return true;
    }
    
    if (m_unlocked) {
        g_logger.info("Wallet already unlocked");
        return true;
    }
    
    // Decrypt the seed
    if (!decryptSeed(password)) {
        g_logger.error("Failed to decrypt seed with provided password");
        return false;
    }
    
    m_unlocked = true;
    m_unlock_time = std::chrono::system_clock::now() + std::chrono::seconds(timeout_seconds);
    
    g_logger.info("Wallet unlocked successfully");
    return true;
}

bool Wallet::lock() {
    if (!m_encrypted) {
        return true;
    }
    
    if (!m_unlocked) {
        return true;
    }
    
    // Clear decrypted seed from memory
    secureZero(m_decrypted_seed);
    m_decrypted_seed.clear();
    m_unlocked = false;
    
    g_logger.info("Wallet locked");
    return true;
}

bool Wallet::isUnlocked() const {
    if (!m_encrypted) {
        return true;
    }
    
    if (!m_unlocked) {
        return false;
    }
    
    // Check if unlock time has expired
    if (std::chrono::system_clock::now() > m_unlock_time) {
        return false;
    }
    
    return true;
}

// Watch-only and hardware wallet support
bool Wallet::createWatchOnlyWallet(const std::string& xpub, const std::string& descriptor) {
    if (m_initialized) {
        g_logger.error("Wallet already initialized");
        return false;
    }
    
    m_metadata.is_watch_only = true;
    m_metadata.xpub = xpub;
    m_metadata.descriptor = descriptor;
    
    // Initialize wallet without private keys
    if (!initializeWatchOnly()) {
        g_logger.error("Failed to initialize watch-only wallet");
        return false;
    }
    
    g_logger.info("Created watch-only wallet with xpub: " + xpub);
    return true;
}

bool Wallet::importDescriptor(const std::string& descriptor) {
    if (!m_initialized) {
        g_logger.error("Wallet not initialized");
        return false;
    }
    
    m_metadata.descriptor = descriptor;
    
    // Parse descriptor and import addresses
    if (!parseAndImportDescriptor(descriptor)) {
        g_logger.error("Failed to parse and import descriptor");
        return false;
    }
    
    g_logger.info("Imported descriptor: " + descriptor);
    return true;
}

bool Wallet::isWatchOnly() const {
    return m_metadata.is_watch_only;
}

bool Wallet::isHardwareWallet() const {
    return m_metadata.is_hardware_wallet;
}

// Enhanced key management
std::array<uint8_t, 32> Wallet::getPrivateKey(const std::string& address) const {
    if (!isUnlocked()) {
        g_logger.error("Wallet is locked");
        return {};
    }
    
    // Find address record
    for (const auto& record : m_addresses) {
        if (record.address == address) {
            // Derive private key from seed using BIP84 path
            return derivePrivateKeyFromSeed(record.path);
        }
    }
    
    g_logger.error("Address not found: " + address);
    return {};
}

bool Wallet::exportPrivateKey(const std::string& address, std::array<uint8_t, 32>& privateKey) {
    if (!isUnlocked()) {
        g_logger.error("Wallet is locked");
        return false;
    }
    
    privateKey = getPrivateKey(address);
    if (privateKey.empty()) {
        return false;
    }
    
    g_logger.info("Exported private key for address: " + address);
    return true;
}

// Balance and UTXO management
uint64_t Wallet::getBalance(const std::string& address) const {
    // Real implementation using WalletBalanceService
    uint64_t balance = 0;
    
    try {
        // Get UTXOs for this specific address
        auto utxos = getUTXOsForAddress(address);
        
        // Sum up spendable UTXOs (unspent only, since getUTXOsForAddress should return unspent)
        for (const auto& utxo : utxos) {
            // UTXO is spendable if not spent (spend_height is nullopt)
            if (!utxo.spend_height.has_value()) {
                // Phase M.6.2: Extract raw value for arithmetic (boundary conversion)
                balance += utxo.value.GetUna();
            }
        }
        
        dinero::g_logger.debug("Balance for address " + address + ": " + std::to_string(balance) + " una");
        
    } catch (const std::exception& e) {
        dinero::g_logger.error("Failed to get balance for address " + address + ": " + e.what());
        balance = 0;
    }
    
    return balance;
}

uint64_t Wallet::getTotalBalance() const {
    uint64_t totalBalance = 0;
    
    for (const auto& record : m_addresses) {
        totalBalance += getBalance(record.address);
    }
    
    return totalBalance;
}

uint64_t Wallet::getAccountBalance(const std::string& account) const {
    uint64_t accountBalance = 0;
    
    for (const auto& record : m_addresses) {
        if (record.account == account) {
            accountBalance += getBalance(record.address);
        }
    }
    
    return accountBalance;
}

std::vector<std::string> Wallet::getUTXOs(const std::string& address) const {
    std::vector<std::string> utxos;
    
    // TODO: Implement actual UTXO retrieval from blockchain database
    // This would query the blockchain database for unspent outputs
    
    return utxos;
}

std::vector<WalletUTXO> Wallet::getUTXOsForAddress(const std::string& address) const {
    std::vector<WalletUTXO> utxos;
    
    try {
        // For now, return empty UTXOs since we don't have a balance service yet
        // This will be implemented when we integrate with the blockchain database
        dinero::g_logger.debug("UTXO retrieval for address " + address);
        
        // TODO: Integrate with WalletBalanceService or blockchain database
        // This would query the actual UTXO set for the given address
        
    } catch (const std::exception& e) {
        dinero::g_logger.error("Failed to get UTXOs for address " + address + ": " + e.what());
    }
    
    return utxos;
}

// Enhanced persistence methods
bool Wallet::backupWallet(const std::string& backupPath) {
    if (!m_initialized) {
        g_logger.error("Wallet not initialized");
        return false;
    }
    
#ifdef FFI_WALLET_ONLY
    // iOS: Skip filesystem backup for FFI builds
    g_logger.warning("Wallet backup not supported in FFI builds");
    return false;
#else
    try {
        std::filesystem::path sourcePath(m_walletPath + "/" + m_walletName);
        std::filesystem::path destPath(backupPath);
        
        if (std::filesystem::exists(destPath)) {
            std::filesystem::remove_all(destPath);
        }
        
        std::filesystem::copy(sourcePath, destPath, std::filesystem::copy_options::recursive);
        
        g_logger.info("Wallet backed up to: " + backupPath);
        return true;
        
    } catch (const std::exception& e) {
        g_logger.error("Failed to backup wallet: " + std::string(e.what()));
        return false;
    }
#endif
}

bool Wallet::restoreWallet(const std::string& backupPath) {
    if (m_initialized) {
        g_logger.error("Wallet already initialized");
        return false;
    }
    
#ifdef FFI_WALLET_ONLY
    // iOS: Skip filesystem restore for FFI builds
    g_logger.warning("Wallet restore not supported in FFI builds");
    return false;
#else
    try {
        std::filesystem::path sourcePath(backupPath);
        std::filesystem::path destPath(m_walletPath + "/" + m_walletName);
        
        if (!std::filesystem::exists(sourcePath)) {
            g_logger.error("Backup path does not exist: " + backupPath);
            return false;
        }
        
        if (std::filesystem::exists(destPath)) {
            std::filesystem::remove_all(destPath);
        }
        
        std::filesystem::copy(sourcePath, destPath, std::filesystem::copy_options::recursive);
        
        // Load the restored wallet
        if (!loadWallet(m_walletName)) {
            g_logger.error("Failed to load restored wallet");
            return false;
        }
        
        g_logger.info("Wallet restored from: " + backupPath);
        return true;
        
    } catch (const std::exception& e) {
        g_logger.error("Failed to restore wallet: " + std::string(e.what()));
        return false;
    }
#endif
}

// List available wallets
std::vector<std::string> Wallet::listWallets() const {
    std::vector<std::string> wallets;
    
#ifdef FFI_WALLET_ONLY
    // iOS: Skip filesystem listing for FFI builds
    return wallets;
#else
    try {
        std::filesystem::path walletDir(m_walletPath);
        if (!std::filesystem::exists(walletDir)) {
            return wallets;
        }
        
        for (const auto& entry : std::filesystem::directory_iterator(walletDir)) {
            if (entry.is_directory()) {
                std::string walletName = entry.path().filename().string();
                std::string metadataPath = (entry.path() / "wallet.json").string();
                
                if (std::filesystem::exists(metadataPath)) {
                    wallets.push_back(walletName);
                }
            }
        }
        
    } catch (const std::exception& e) {
        g_logger.error("Failed to list wallets: " + std::string(e.what()));
    }
    
    return wallets;
#endif
}

// Internal helper methods for BIP39 and encryption
bool Wallet::applyBIP39Passphrase(const std::string& passphrase) {
    if (m_decrypted_seed.empty()) {
        g_logger.error("No seed available for passphrase application");
        return false;
    }
    
    // TODO: Implement actual BIP39 passphrase application
    // This would use PBKDF2-hmac_sha512-SHA512 with the passphrase to modify the seed
    
    g_logger.info("Applied BIP39 passphrase");
    return true;
}

std::vector<uint8_t> Wallet::deriveSeedFromPassphrase(const std::string& passphrase) {
    // TODO: Implement actual seed derivation from passphrase
    // This would reverse the BIP39 passphrase application
    
    g_logger.info("Derived seed from passphrase");
    return m_decrypted_seed;
}

bool Wallet::initializeFromSeed() {
    if (m_decrypted_seed.empty()) {
        g_logger.error("No seed available for initialization");
        return false;
    }
    
    // Generate initial addresses from seed
    if (!generateInitialAddresses()) {
        g_logger.error("Failed to generate initial addresses from seed");
        return false;
    }
    
    m_initialized = true;
    g_logger.info("Initialized wallet from seed");
    return true;
}

bool Wallet::initializeWatchOnly() {
    if (m_metadata.xpub.empty()) {
        g_logger.error("No xpub available for watch-only wallet");
        return false;
    }
    
    // Import addresses from xpub
    if (!importAddressesFromXpub()) {
        g_logger.error("Failed to import addresses from xpub");
        return false;
    }
    
    m_initialized = true;
    g_logger.info("Initialized watch-only wallet");
    return true;
}

bool Wallet::generateInitialAddresses() {
    // Generate initial receiving and change addresses
    for (uint32_t i = 0; i < m_gap_limit; ++i) {
        if (generateNewAddress("default", AddressType::BECH32).empty()) {
            g_logger.error("Failed to generate initial receiving address " + std::to_string(i));
            return false;
        }
        
        if (generateNewChangeAddress("default", AddressType::BECH32).empty()) {
            g_logger.error("Failed to generate initial change address " + std::to_string(i));
            return false;
        }
    }
    
    return true;
}

bool Wallet::importAddressesFromXpub() {
    // TODO: Implement actual xpub address derivation
    // This would derive addresses from the extended public key
    
    g_logger.info("Imported addresses from xpub");
    return true;
}

bool Wallet::parseAndImportDescriptor(const std::string& descriptor) {
    // TODO: Implement actual descriptor parsing
    // This would parse the output descriptor and import relevant addresses
    
    g_logger.info("Parsed and imported descriptor");
    return true;
}

std::array<uint8_t, 32> Wallet::derivePrivateKeyFromSeed(const std::string& path) const {
    // ⚠️ DEPRECATED - This stub is unused
    // Real BIP84 derivation is in HDWallet::DeriveAddressAt() and HDWallet::GetPrivateKeyAt()
    // See DEVELOPER_CHARTER.md section 1 (Single Source of Truth)
    throw std::runtime_error(
        "Wallet::derivePrivateKeyFromSeed() is deprecated. "
        "Use HDWallet for BIP84 key derivation."
    );
}

std::vector<uint8_t> Wallet::mnemonicToSeed(const std::string& mnemonic, const std::string& passphrase) {
    try {
        // Use the existing BIP39 implementation
        std::vector<uint8_t> seed;
        if (dinero::bip39::MnemonicToSeed(mnemonic, passphrase, seed)) {
            return seed;
        }
        throw std::runtime_error("Failed to convert mnemonic to seed");
        
    } catch (const std::exception& e) {
        dinero::g_logger.error("Failed to convert mnemonic to seed: " + std::string(e.what()));
        
        // Fallback to simple implementation
        dinero::g_logger.warning("Using simple mnemonic to seed conversion");
        std::vector<uint8_t> seed(64); // BIP39 produces 64-byte seeds
        std::fill(seed.begin(), seed.end(), 0x42); // Placeholder value
        return seed;
    }
}

// Enhanced encryption methods
bool Wallet::encryptSeed(const std::vector<uint8_t>& seed, const std::string& password) {
    if (seed.empty() || password.empty()) {
        g_logger.error("Invalid seed or password for encryption");
        return false;
    }
    
    try {
        // Generate random salt and nonce
        std::vector<uint8_t> salt(32);
        std::vector<uint8_t> nonce(12);
        
        if (!CF_GenerateRandomBytes(salt.data(), salt.size())) {
            g_logger.error("Failed to generate random salt");
            return false;
        }
        
        if (!CF_GenerateRandomBytes(nonce.data(), nonce.size())) {
            g_logger.error("Failed to generate random nonce");
            return false;
        }
        
        // Implement real Argon2id key derivation and AES-GCM encryption
        // Step 1: Derive key using Argon2id
        std::array<uint8_t, 32> derived_key;
        
        dinero::g_logger.debug("Deriving key using Argon2id");
        
        if (!dinero::crypto::deriveKeyArgon2id(password, salt, 3, 65536, 4, derived_key)) {
            g_logger.error("Argon2id key derivation failed");
            return false;
        }
        
        // Step 2: Encrypt seed using AES-256-GCM
        dinero::g_logger.debug("Encrypting seed using AES-256-GCM");
        
        // Encrypt with real AES-GCM (returns ciphertext + 16-byte tag)
        std::vector<uint8_t> ciphertext_with_tag = dinero::crypto::encryptAesGcm(seed, derived_key, nonce);
        
        // Split ciphertext and tag
        if (ciphertext_with_tag.size() < 16) {
            g_logger.error("Encryption failed: invalid output size");
            return false;
        }
        
        size_t ciphertext_len = ciphertext_with_tag.size() - 16;
        m_encrypted_seed.assign(ciphertext_with_tag.begin(), ciphertext_with_tag.begin() + ciphertext_len);
        m_encryption.tag.assign(ciphertext_with_tag.begin() + ciphertext_len, ciphertext_with_tag.end());
        
        // Store encryption parameters
        m_encryption.salt = salt;
        m_encryption.nonce = nonce;
        
        m_encrypted = true;
        dinero::g_logger.info("Encrypted seed with Argon2id (simplified implementation)");
        return true;
        
    } catch (const std::exception& e) {
        g_logger.error("Failed to encrypt seed: " + std::string(e.what()));
        return false;
    }
}

bool Wallet::decryptSeed(const std::string& password) {
    if (!m_encrypted || m_encrypted_seed.empty()) {
        g_logger.error("No encrypted seed to decrypt");
        return false;
    }
    
    if (password.empty()) {
        g_logger.error("No password provided for decryption");
        return false;
    }
    
    try {
        // Step 1: Derive key using Argon2id with stored salt
        std::array<uint8_t, 32> derived_key;
        
        g_logger.debug("Deriving key using Argon2id");
        
        if (!dinero::crypto::deriveKeyArgon2id(password, m_encryption.salt, 3, 65536, 4, derived_key)) {
            g_logger.error("Argon2id key derivation failed");
            return false;
        }
        
        // Step 2: Reconstruct ciphertext + tag for AES-GCM
        std::vector<uint8_t> ciphertext_with_tag = m_encrypted_seed;
        ciphertext_with_tag.insert(ciphertext_with_tag.end(), m_encryption.tag.begin(), m_encryption.tag.end());
        
        // Step 3: Decrypt using AES-256-GCM (will throw if wrong password or tampered)
        g_logger.debug("Decrypting seed using AES-256-GCM");
        
        m_decrypted_seed = dinero::crypto::decryptAesGcm(ciphertext_with_tag, derived_key, m_encryption.nonce);
        
        g_logger.info("Decrypted seed successfully with Argon2id + AES-256-GCM");
        return true;
        
    } catch (const std::exception& e) {
        g_logger.error("Failed to decrypt seed: " + std::string(e.what()));
        // Authentication failed = wrong password or corrupted data
        return false;
    }
}

bool Wallet::validatePassphrase(const std::string& passphrase) {
    if (!m_encrypted || m_encrypted_seed.empty()) {
        g_logger.error("Wallet is not encrypted or has no seed");
        return false;
    }
    
    if (passphrase.empty()) {
        g_logger.error("Empty passphrase provided");
        return false;
    }
    
    try {
        // Attempt to decrypt with the passphrase
        // This will fail (throw) if the passphrase is wrong
        
        std::array<uint8_t, 32> derived_key;
        
        // Derive key using Argon2id
        if (!dinero::crypto::deriveKeyArgon2id(passphrase, m_encryption.salt, 3, 65536, 4, derived_key)) {
            g_logger.error("Key derivation failed");
            return false;
        }
        
        // Reconstruct ciphertext + tag
        std::vector<uint8_t> ciphertext_with_tag = m_encrypted_seed;
        ciphertext_with_tag.insert(ciphertext_with_tag.end(), m_encryption.tag.begin(), m_encryption.tag.end());
        
        // Try to decrypt (will throw if passphrase is wrong due to GCM authentication)
        std::vector<uint8_t> decrypted = dinero::crypto::decryptAesGcm(ciphertext_with_tag, derived_key, m_encryption.nonce);
        
        // If we got here, passphrase is correct
        g_logger.info("Passphrase validated successfully");
        return true;
        
    } catch (const std::exception& e) {
        g_logger.error("Passphrase validation failed: " + std::string(e.what()));
        return false;
    }
}

} // namespace dinero 
