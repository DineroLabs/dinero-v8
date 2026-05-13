#include "rpc/methods_taproot.h"
#include "wallet/wallet_manager.h"
#include "wallet/taproot_keys.h" // Canonical TapTweak (ComputeTweakedPubkey)
#include "address/addr_codec.h"
#include "bech32.hpp"
#include "crypto/evp_secp256k1.h"
#include "crypto/sha256.h"
#include "common/logger.h"
#include "consensus/coin_type.h"
#include <secp256k1.h>
#include <secp256k1_schnorrsig.h>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace dinero {

namespace {
    // Helper: Generate random 32-byte key.
    std::vector<uint8_t> GenerateRandomKey() {
        std::vector<uint8_t> key(32);
        unsigned char rand_bytes[32];
        if (!dinero::crypto::GenerateSecp256k1PrivateKey(rand_bytes)) {
            throw std::runtime_error("Failed to generate Taproot private key");
        }
        std::copy(rand_bytes, rand_bytes + 32, key.begin());
        return key;
    }

    // Helper: Derive x-only pubkey from private key
    std::vector<uint8_t> GetXOnlyPubkey(const std::vector<uint8_t>& privkey) {
        auto* ctx = dinero::crypto::GetSecp256k1ContextSignVerify();

        secp256k1_keypair keypair;
        if (!secp256k1_keypair_create(ctx, &keypair, privkey.data())) {
            throw std::runtime_error("Failed to create keypair");
        }

        secp256k1_xonly_pubkey xonly_pubkey;
        if (!secp256k1_keypair_xonly_pub(ctx, &xonly_pubkey, nullptr, &keypair)) {
            throw std::runtime_error("Failed to extract x-only pubkey");
        }

        unsigned char pubkey_bytes[32];
        secp256k1_xonly_pubkey_serialize(ctx, pubkey_bytes, &xonly_pubkey);

        return std::vector<uint8_t>(pubkey_bytes, pubkey_bytes + 32);
    }

    // Thin adapter — delegates to canonical TaprootKeys::ComputeTweakedPubkey
    std::vector<uint8_t> ComputeTaprootOutputKey(const std::vector<uint8_t>& internal_key) {
        if (internal_key.size() != 32) {
            throw std::runtime_error("Internal key must be 32 bytes");
        }
        std::array<uint8_t, 32> internal_arr, output_arr;
        std::copy(internal_key.begin(), internal_key.end(), internal_arr.begin());
        if (!dinero::TaprootKeys::ComputeTweakedPubkey(internal_arr, output_arr)) {
            throw std::runtime_error("Failed to compute tweaked pubkey");
        }
        return std::vector<uint8_t>(output_arr.begin(), output_arr.end());
    }

    // Helper: Convert bytes to hex
    std::string ToHex(const std::vector<uint8_t>& data) {
        std::ostringstream oss;
        for (uint8_t byte : data) {
            oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
        }
        return oss.str();
    }

    // Helper: Create P2TR address from output key
    std::string CreateTaprootAddress(const std::vector<uint8_t>& output_key, const std::string& hrp) {
        // P2TR witness program: version 1 + 32-byte output key
        std::vector<uint8_t> witness_program;
        witness_program.push_back(0x01);  // Witness version 1 (Taproot)
        witness_program.insert(witness_program.end(), output_key.begin(), output_key.end());

        // Convert to 5-bit groups for bech32m
        std::vector<unsigned char> data5;
        data5.push_back(1); // Witness version
        if (!bech32::ConvertBits<8, 5, true>(data5, output_key.data(), output_key.size())) {
            throw std::runtime_error("Failed to convert witness program to 5-bit");
        }

        // Encode as bech32m
        std::string address = bech32::Encode(bech32::Encoding::BECH32M, hrp, data5);
        if (address.empty()) {
            throw std::runtime_error("Failed to encode bech32m address");
        }

        return address;
    }
}

Json::Value taproot_getnewaddress(const Json::Value& params, dinero::WalletManager* wallet_manager) {
    try {
        // Extract optional label
        std::string label;
        if (!params.empty() && params.isArray() && params.size() > 0) {
            if (!params[0].isString()) {
                throw std::runtime_error("Label must be a string");
            }
            label = params[0].asString();
        }

        // Generate random private key for this ad-hoc address request.
        std::vector<uint8_t> privkey = GenerateRandomKey();

        // Derive internal key (x-only pubkey)
        std::vector<uint8_t> internal_key = GetXOnlyPubkey(privkey);

        // Compute Taproot output key (no script tree = key path only)
        std::vector<uint8_t> output_key = ComputeTaprootOutputKey(internal_key);

        // Create P2TR address
        std::string hrp = GetChainParams().HRP();
        std::string address = CreateTaprootAddress(output_key, hrp);

        // Build P2TR scriptPubKey: OP_1 <32-byte-output-key>
        std::vector<uint8_t> scriptPubKey;
        scriptPubKey.push_back(0x51);  // OP_1 (witness version 1 = Taproot)
        scriptPubKey.push_back(0x20);  // Push 32 bytes
        scriptPubKey.insert(scriptPubKey.end(), output_key.begin(), output_key.end());

        // Register scriptPubKey with UTXOIndex for UTXO scanning
        // This is CRITICAL - without this, the wallet will never detect Taproot UTXOs!
        if (wallet_manager && wallet_manager->getUTXOIndex()) {
            // Use a deterministic descriptor-style path label for index registration.
            std::string derivation_path = "m/86'/" +
                                          std::to_string(dinero::consensus::DINERO_COIN_TYPE) +
                                          "'/0'/0/0";
            wallet_manager->getUTXOIndex()->RegisterAddress(scriptPubKey, derivation_path);
            dinero::g_logger.info("Registered Taproot scriptPubKey for address: " + address);
        } else {
            dinero::g_logger.warning("UTXOIndex not available - Taproot address NOT registered for scanning!");
        }

        Json::Value result(Json::objectValue);
        result["address"] = address;
        result["output_key"] = ToHex(output_key);
        result["internal_key"] = ToHex(internal_key);
        result["type"] = "p2tr";
        result["derivation_path"] = "m/86'/" +
                                    std::to_string(dinero::consensus::DINERO_COIN_TYPE) +
                                    "'/0'/0/0";
        result["note"] = "Taproot address registered with UTXOIndex for scanning (BIP86 derivation pending)";

        dinero::g_logger.info("Generated Taproot address: " + address);
        return result;

    } catch (const std::exception& e) {
        dinero::g_logger.error("taproot.getnewaddress failed: " + std::string(e.what()));
        throw std::runtime_error("taproot.getnewaddress failed: " + std::string(e.what()));
    }
}

Json::Value taproot_validateaddress(const Json::Value& params, dinero::WalletManager* wallet_manager) {
    try {
        if (params.empty() || !params.isArray() || params.size() != 1) {
            throw std::runtime_error("Missing required parameter: address");
        }

        if (!params[0].isString()) {
            throw std::runtime_error("Address must be a string");
        }

        std::string addr = params[0].asString();

        Json::Value result(Json::objectValue);
        result["address"] = addr;
        result["isvalid"] = false;

        // Decode bech32m
        auto dec = bech32::DecodeWithEncoding(addr);
        std::string hrp = std::get<0>(dec);
        std::vector<uint8_t> data = std::get<1>(dec);
        bech32::Encoding encoding = std::get<2>(dec);

        if (encoding != bech32::Encoding::BECH32M) {
            result["error"] = "Not a bech32m address (Taproot requires bech32m)";
            return result;
        }

        // Check HRP
        std::string expected_hrp = GetChainParams().HRP();
        if (hrp != expected_hrp) {
            result["error"] = "Wrong network (expected " + expected_hrp + ", got " + hrp + ")";
            return result;
        }

        // Check witness version
        if (data.empty() || data[0] != 1) {
            result["error"] = "Not a Taproot address (witness version must be 1)";
            return result;
        }

        // Convert from 5-bit to 8-bit
        std::vector<unsigned char> decoded;
        if (!bech32::ConvertBits<5, 8, false>(decoded, data.data() + 1, data.size() - 1)) {
            result["error"] = "Invalid bech32m data";
            return result;
        }

        // Check output key size
        if (decoded.size() != 32) {
            result["error"] = "Invalid output key size (expected 32 bytes, got " + std::to_string(decoded.size()) + ")";
            return result;
        }

        result["isvalid"] = true;
        result["type"] = "p2tr";
        result["witness_version"] = 1;
        result["witness_program"] = ToHex(decoded);

        return result;

    } catch (const std::exception& e) {
        Json::Value result(Json::objectValue);
        result["isvalid"] = false;
        result["error"] = e.what();
        return result;
    }
}

Json::Value taproot_getaddressinfo(const Json::Value& params, dinero::WalletManager* wallet_manager) {
    // Validate address first
    Json::Value validation = taproot_validateaddress(params, wallet_manager);

    if (!validation["isvalid"].asBool()) {
        return validation;
    }

    // Add additional info
    validation["note"] = "Taproot key path spending only (no script tree in demo)";

    return validation;
}

} // namespace dinero
