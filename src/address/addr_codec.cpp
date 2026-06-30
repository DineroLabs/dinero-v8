#include "address/addr_codec.h"
#include "wallet/address.h"
#include "common/logger.h"
#include "bech32/bech32.hpp"
#include <stdexcept>

namespace dinero {

// Global HRP configuration - will be set by SelectParams()
// (defined in consensus/chainparams.cpp)

// HrpForActiveNetwork() is now defined in chainparams.cpp

// Convert existing Address functions to new Destination format
Destination DecodeBase58Address(const std::string& s) {
    // Use existing Address validation
    if (!Address::validateAddress(s)) {
        return Destination(); // invalid
    }
    
    AddressMetadata metadata = Address::getAddressMetadata(s);
    if (!metadata.is_valid || metadata.hex_hash160.empty()) {
        return Destination(); // invalid
    }
    
    // Convert hex hash160 to bytes
    std::vector<uint8_t> hash;
    for (size_t i = 0; i < metadata.hex_hash160.length(); i += 2) {
        hash.push_back(static_cast<uint8_t>(std::stoi(metadata.hex_hash160.substr(i, 2), nullptr, 16)));
    }
    
    return Destination(hash);
}

Destination DecodeBech32Address(const std::string& s, const std::string& hrp) {
    // Use the public decodeAddress method
    Address::DecodedAddress decoded = Address::decodeAddress(s);
    
    if (!decoded.isValid) {
        g_logger.debug("Bech32 validation failed: " + decoded.error);
        return Destination(); // invalid
    }
    
    // Check if it's the expected HRP
    if (decoded.hrp != hrp) {
        g_logger.debug("HRP mismatch: expected " + hrp + ", got " + decoded.hrp);
        return Destination(); // wrong HRP
    }
    
    // Convert hex pubKeyHash to bytes
    if (decoded.pubKeyHash.length() != 40) { // 20 bytes = 40 hex chars
        return Destination(); // invalid hash length
    }
    
    std::vector<uint8_t> hash;
    for (size_t i = 0; i < decoded.pubKeyHash.length(); i += 2) {
        hash.push_back(static_cast<uint8_t>(std::stoi(decoded.pubKeyHash.substr(i, 2), nullptr, 16)));
    }
    
    return Destination(hash);
}

std::string EncodeBase58Address(const Destination& d) {
    if (!IsValidDestination(d)) {
        return "";
    }
    
    // Use existing Address::createAddress with DINERO_P2PKH
    // Convert hash back to public key format (this is a simplification)
    return Address::createAddress(d.pubkey_hash, AddressType::DINERO_P2PKH);
}

std::string EncodeBech32Address(const Destination& d, const std::string& hrp) {
    if (!IsValidDestination(d)) {
        return "";
    }
    
    // Use existing Address::createBech32P2WPKH
    return Address::createBech32P2WPKH(d.pubkey_hash, hrp);
}

WitnessAddressInfo DecodeWitnessAddress(const std::string& s, const std::string& hrp) {
    WitnessAddressInfo info;

    // bech32::Decode validates HRP match, checksum, program length bounds, and
    // detects bech32 vs bech32m encoding.
    auto decoded = bech32::Decode(hrp, s);
    if (!decoded.has_value()) {
        return info;  // HRP mismatch, bad checksum, or malformed program
    }

    const int witver = decoded->witver;
    const std::vector<uint8_t>& program = decoded->program;
    const bech32::Encoding enc = decoded->encoding;

    if (witver == 0) {
        // SegWit v0: P2WPKH (20) or P2WSH (32), must be bech32 (not bech32m).
        if (enc != bech32::Encoding::BECH32) return info;
        if (program.size() != 20 && program.size() != 32) return info;
    } else if (witver >= 1 && witver <= 16) {
        // SegWit v1..v16 (incl. Taproot v1): must be bech32m, program 2..40 bytes.
        if (enc != bech32::Encoding::BECH32M) return info;
        if (program.size() < 2 || program.size() > 40) return info;
    } else {
        return info;  // invalid witness version
    }

    // Canonical scriptPubKey: OP_<witver> OP_PUSHBYTES_<n> <program>.
    // OP_0 = 0x00; OP_1..OP_16 = 0x51..0x60. For taproot this yields 0x5120<program>.
    info.script_pubkey.reserve(2 + program.size());
    info.script_pubkey.push_back(witver == 0 ? static_cast<uint8_t>(0x00)
                                             : static_cast<uint8_t>(0x50 + witver));
    info.script_pubkey.push_back(static_cast<uint8_t>(program.size()));
    info.script_pubkey.insert(info.script_pubkey.end(), program.begin(), program.end());

    info.is_valid = true;
    info.is_witness = true;
    info.witness_version = witver;
    info.witness_program = program;
    return info;
}

ParsedAddress DecodeAddressAuto(const std::string& s) {
    const auto& hrp = HrpForActiveNetworkRef();

    // Quick path: looks like bech32/bech32m if it starts with "<hrp>1"
    const std::string prefix = hrp + "1";
    if (s.size() > prefix.size() && s.rfind(prefix, 0) == 0) {
        // Try Taproot (v1, bech32m) first
        if (auto d = DecodeTaprootAddress(s, hrp); IsValidDestination(d)) {
            g_logger.debug("Decoded as Taproot (bech32m v1): " + s);
            return ParsedAddress{d, AddrType::Bech32};
        }

        // Try legacy Bech32 v0 (P2WPKH/P2WSH)
        if (auto d = DecodeBech32Address(s, hrp); IsValidDestination(d)) {
            g_logger.debug("Decoded as Bech32 v0: " + s);
            return ParsedAddress{d, AddrType::Bech32};
        }
        // fall through to Base58 try if malformed bech32/bech32m
    }

    // Try Base58
    if (auto d = DecodeBase58Address(s); IsValidDestination(d)) {
        g_logger.debug("Decoded as Base58: " + s);
        return ParsedAddress{d, AddrType::Base58};
    }

    // Try Taproot even if the string didn't look like it (covers mixed HRPs / user typos)
    if (auto d = DecodeTaprootAddress(s, hrp); IsValidDestination(d)) {
        g_logger.debug("Decoded as Taproot (fallback): " + s);
        return ParsedAddress{d, AddrType::Bech32};
    }

    // Try Bech32 v0 even if the string didn't look like it (covers mixed HRPs / user typos)
    if (auto d = DecodeBech32Address(s, hrp); IsValidDestination(d)) {
        g_logger.debug("Decoded as Bech32 (fallback): " + s);
        return ParsedAddress{d, AddrType::Bech32};
    }

    throw std::runtime_error("Invalid address: unsupported format or bad checksum");
}

// ============================================================================
// Taproot Address Functions (BIP341/BIP86)
// ============================================================================

std::vector<uint8_t> DecodeTaprootWitnessProgram(const std::string& address) {
    // Get active network HRP
    const std::string& hrp = HrpForActiveNetworkRef();

    // Use bech32::Decode which handles everything properly
    auto result = bech32::Decode(hrp, address);

    if (!result.has_value()) {
        throw std::runtime_error("Invalid bech32 address: decode failed");
    }

    // Verify encoding is BECH32M (witness v1+)
    if (result->encoding != bech32::Encoding::BECH32M) {
        throw std::runtime_error("Invalid Taproot address: must use bech32m encoding");
    }

    // Verify witness version is 1 (Taproot)
    if (result->witver != 1) {
        throw std::runtime_error("Invalid Taproot address: witness version must be 1, got " + std::to_string(result->witver));
    }

    // Taproot witness programs must be exactly 32 bytes (x-only pubkey)
    if (result->program.size() != 32) {
        throw std::runtime_error("Invalid Taproot witness program: must be 32 bytes, got " + std::to_string(result->program.size()));
    }

    return result->program;
}

std::vector<uint8_t> CreateP2TRScriptPubKey(const std::vector<uint8_t>& witness_program) {
    if (witness_program.size() != 32) {
        throw std::runtime_error("Invalid Taproot witness program: must be 32 bytes");
    }

    // P2TR scriptPubKey format: OP_1 OP_PUSHBYTES_32 <32-byte-witness-program>
    // OP_1 = 0x51, OP_PUSHBYTES_32 = 0x20
    std::vector<uint8_t> script_pubkey;
    script_pubkey.reserve(34); // 1 + 1 + 32

    script_pubkey.push_back(0x51); // OP_1 (witness version 1)
    script_pubkey.push_back(0x20); // OP_PUSHBYTES_32
    script_pubkey.insert(script_pubkey.end(), witness_program.begin(), witness_program.end());

    return script_pubkey;
}

Destination DecodeTaprootAddress(const std::string& s, const std::string& hrp) {
    try {
        // Decode the Taproot address
        std::vector<uint8_t> witness_program = DecodeTaprootWitnessProgram(s);

        // Verify HRP matches (extract from address)
        auto [addr_hrp, data, encoding] = bech32::DecodeWithEncoding(s);
        if (addr_hrp != hrp) {
            g_logger.debug("Taproot HRP mismatch: expected " + hrp + ", got " + addr_hrp);
            return Destination(); // invalid
        }

        // Create destination with 32-byte witness program
        // Note: For Taproot, pubkey_hash contains the full 32-byte x-only pubkey
        return Destination(witness_program);

    } catch (const std::exception& e) {
        g_logger.debug("Taproot address decode failed: " + std::string(e.what()));
        return Destination(); // invalid
    }
}

} // namespace dinero
