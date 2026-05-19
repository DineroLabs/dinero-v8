#include "mining/address_validator.h"
#include "daemon/bech32_decode.h"
#include "crypto/base58.hpp"
#include <algorithm>

namespace dinero {
namespace mining {

// ============================================================================
// Address Validation and Decoding (Phase 26.4)
// ============================================================================

bool IsValidDineroAddress(const std::string& addr) {
    if (addr.empty()) {
        return false;
    }

    // Bech32 SegWit addresses (native SegWit)
    // - Mainnet: din1...
    // - Testnet: tdin1...
    // - Regtest: rdin1...
    if (addr.size() >= 6 && addr.substr(0, 4) == "din1") {
        int witver;
        std::vector<uint8_t> witprog;
        return Bech32DecodeSegwit(addr, "din", witver, witprog);
    }

    if (addr.size() >= 6 && addr.substr(0, 5) == "tdin1") {
        int witver;
        std::vector<uint8_t> witprog;
        return Bech32DecodeSegwit(addr, "tdin", witver, witprog);
    }

    if (addr.size() >= 6 && addr.substr(0, 5) == "rdin1") {
        int witver;
        std::vector<uint8_t> witprog;
        return Bech32DecodeSegwit(addr, "rdin", witver, witprog);
    }

    // Legacy Base58Check addresses
    // - Mainnet P2PKH: D...
    // - Mainnet P2SH: (version byte determines this)
    if (addr.size() > 25 && addr[0] == 'D') {
        std::vector<uint8_t> decoded;
        return dinero::b58::decode_check(addr, decoded) && decoded.size() >= 21;
    }

    return false;
}

bool DecodeAddress(const std::string& addr, AddressInfo& info) {
    info = AddressInfo();  // Reset

    if (addr.empty()) {
        return false;
    }

    // Bech32 SegWit addresses
    if (addr.size() >= 6 && (
        addr.substr(0, 4) == "din1" ||
        addr.substr(0, 5) == "tdin1" ||
        addr.substr(0, 5) == "rdin1")) {

        std::string hrp;
        if (addr.substr(0, 5) == "tdin1") {
            hrp = "tdin";
            info.network = AddressNetwork::TESTNET;
        } else if (addr.substr(0, 5) == "rdin1") {
            hrp = "rdin";
            info.network = AddressNetwork::REGTEST;
        } else {
            hrp = "din";
            info.network = AddressNetwork::MAINNET;
        }

        int witver;
        std::vector<uint8_t> witprog;

        if (!Bech32DecodeSegwit(addr, hrp, witver, witprog)) {
            return false;
        }

        info.witness_version = witver;
        info.program = witprog;

        // Determine address type based on witness version and program length
        if (witver == 0) {
            if (witprog.size() == 20) {
                info.type = AddressType::P2WPKH;  // Native SegWit v0 P2WPKH
            } else if (witprog.size() == 32) {
                info.type = AddressType::P2WSH;   // Native SegWit v0 P2WSH
            } else {
                return false;  // Invalid witness program length
            }
        } else if (witver == 1 && witprog.size() == 32) {
            info.type = AddressType::P2TR;  // Taproot
        } else if (witver == 3 && witprog.size() == 32) {
            info.type = AddressType::P2MR;  // v7 P2MR (ML-DSA-65 merkle root)
        } else {
            info.type = AddressType::UNKNOWN_WITNESS;
        }

        return true;
    }

    // Legacy Base58Check addresses
    if (addr.size() > 25 && addr[0] == 'D') {
        std::vector<uint8_t> decoded;

        if (!dinero::b58::decode_check(addr, decoded) || decoded.size() < 21) {
            return false;
        }

        // First byte is version
        uint8_t version = decoded[0];

        // Remaining bytes are the hash (20 bytes for P2PKH/P2SH)
        info.program = std::vector<uint8_t>(decoded.begin() + 1, decoded.end());

        // Determine type based on version byte
        // TODO: Define actual Dinero version bytes
        // For now, assume version 0x1E is P2PKH (similar to Bitcoin's 0x00)
        if (version == 0x1E) {  // Dinero P2PKH version
            info.type = AddressType::P2PKH;
            info.network = AddressNetwork::MAINNET;
        } else if (version == 0x13) {  // Dinero P2SH version (placeholder)
            info.type = AddressType::P2SH;
            info.network = AddressNetwork::MAINNET;
        } else {
            info.type = AddressType::UNKNOWN_LEGACY;
        }

        return true;
    }

    return false;
}

std::vector<uint8_t> BuildScriptPubKey(const AddressInfo& info) {
    std::vector<uint8_t> script;

    switch (info.type) {
        case AddressType::P2PKH: {
            // OP_DUP OP_HASH160 <20-byte-hash> OP_EQUALVERIFY OP_CHECKSIG
            if (info.program.size() != 20) {
                return {};  // Invalid
            }

            script.push_back(0x76);  // OP_DUP
            script.push_back(0xa9);  // OP_HASH160
            script.push_back(0x14);  // Push 20 bytes

            script.insert(script.end(), info.program.begin(), info.program.end());

            script.push_back(0x88);  // OP_EQUALVERIFY
            script.push_back(0xac);  // OP_CHECKSIG
            break;
        }

        case AddressType::P2SH: {
            // OP_HASH160 <20-byte-hash> OP_EQUAL
            if (info.program.size() != 20) {
                return {};  // Invalid
            }

            script.push_back(0xa9);  // OP_HASH160
            script.push_back(0x14);  // Push 20 bytes

            script.insert(script.end(), info.program.begin(), info.program.end());

            script.push_back(0x87);  // OP_EQUAL
            break;
        }

        case AddressType::P2WPKH: {
            // OP_0 <20-byte-hash>
            if (info.program.size() != 20) {
                return {};  // Invalid
            }

            script.push_back(0x00);  // OP_0 (witness version 0)
            script.push_back(0x14);  // Push 20 bytes

            script.insert(script.end(), info.program.begin(), info.program.end());
            break;
        }

        case AddressType::P2WSH: {
            // OP_0 <32-byte-hash>
            if (info.program.size() != 32) {
                return {};  // Invalid
            }

            script.push_back(0x00);  // OP_0 (witness version 0)
            script.push_back(0x20);  // Push 32 bytes

            script.insert(script.end(), info.program.begin(), info.program.end());
            break;
        }

        case AddressType::P2TR: {
            // OP_1 <32-byte-x-only-pubkey>
            if (info.program.size() != 32) {
                return {};  // Invalid
            }

            script.push_back(0x51);  // OP_1 (witness version 1)
            script.push_back(0x20);  // Push 32 bytes

            script.insert(script.end(), info.program.begin(), info.program.end());
            break;
        }

        case AddressType::P2MR: {
            // OP_3 <32-byte-ML-DSA-65-merkle-root>
            if (info.program.size() != 32) {
                return {};  // Invalid
            }

            script.push_back(0x53);  // OP_3 (witness version 3)
            script.push_back(0x20);  // Push 32 bytes

            script.insert(script.end(), info.program.begin(), info.program.end());
            break;
        }

        default:
            return {};  // Unknown type
    }

    return script;
}

bool IsTaprootAddress(const std::string& addr) {
    AddressInfo info;
    if (!DecodeAddress(addr, info)) {
        return false;
    }
    return info.type == AddressType::P2TR;
}

bool IsCoinbaseEligibleAddress(const std::string& addr) {
    AddressInfo info;
    if (!DecodeAddress(addr, info)) {
        return false;
    }
    return info.type == AddressType::P2TR || info.type == AddressType::P2MR;
}

std::string GetTaprootRequiredMessage(const std::string& addr) {
    AddressInfo info;
    std::string type_name = "unknown";

    if (DecodeAddress(addr, info)) {
        switch (info.type) {
            case AddressType::P2PKH:  type_name = "P2PKH (legacy)"; break;
            case AddressType::P2SH:   type_name = "P2SH (legacy)"; break;
            case AddressType::P2WPKH: type_name = "P2WPKH (SegWit v0)"; break;
            case AddressType::P2WSH:  type_name = "P2WSH (SegWit v0)"; break;
            case AddressType::P2TR:   type_name = "P2TR (Taproot)"; break;
            case AddressType::P2MR:   type_name = "P2MR (ML-DSA-65)"; break;
            default: type_name = "unknown"; break;
        }
    }

    return "Mining requires a Taproot (din1p...) or P2MR (din1r...) address. "
           "Your address '" + addr.substr(0, 12) + "...' is " + type_name + ". "
           "Use wallet.getnewaddress [\"taproot\"|\"p2mr\"] to generate one.";
}

} // namespace mining
} // namespace dinero
