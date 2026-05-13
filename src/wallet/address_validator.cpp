#include "wallet/address_validator.h"
#include "../external/bech32/bech32.hpp"
#include "wallet/address.h"
#include <algorithm>
#include <cctype>

namespace din {

AddressValidator::ValidationResult AddressValidator::validate(
    const std::string& address, 
    Network expected_network
) {
    ValidationResult result;
    
    if (address.empty()) {
        result.error_message = "Empty address";
        return result;
    }
    
    // Detect address format and validate accordingly
    if (address.size() >= 4 && (address.substr(0, 4) == "din1" || 
                                address.substr(0, 5) == "tdin1" || 
                                address.substr(0, 5) == "rdin1")) {
        // Bech32/Bech32m format
        result = validateBech32(address, expected_network);
        if (!result.valid) {
            // Try Bech32m if Bech32 fails
            result = validateBech32m(address, expected_network);
        }
    } else {
        // Legacy Base58Check format
        result = validateBase58(address, expected_network);
    }
    
    return result;
}

bool AddressValidator::isValid(const std::string& address, Network expected_network) {
    return validate(address, expected_network).valid;
}

AddressValidator::AddressType AddressValidator::getType(const std::string& address) {
    auto result = validate(address);
    return result.type;
}

std::string AddressValidator::typeToString(AddressType type) {
    switch (type) {
        case AddressType::P2WPKH: return "P2WPKH";
        case AddressType::P2WSH: return "P2WSH";
        case AddressType::P2TR: return "P2TR";
        case AddressType::P2PKH: return "P2PKH";
        case AddressType::P2SH: return "P2SH";
        case AddressType::Unknown: return "Unknown";
    }
    return "Invalid";
}

std::string AddressValidator::networkToString(Network network) {
    switch (network) {
        case Network::Mainnet: return "mainnet";
        case Network::Testnet: return "testnet";
        case Network::Regtest: return "regtest";
    }
    return "unknown";
}

std::string AddressValidator::getHRP(Network network) {
    switch (network) {
        case Network::Mainnet: return "din";
        case Network::Testnet: return "tdin";
        case Network::Regtest: return "rdin";
    }
    return "din"; // Default to mainnet
}

// Private implementation methods

AddressValidator::ValidationResult AddressValidator::validateBech32(
    const std::string& address, 
    Network expected_network
) {
    ValidationResult result;
    
    // Extract HRP from address (everything before '1')
    size_t pos = address.find('1');
    if (pos == std::string::npos) {
        result.error_message = "Invalid Bech32 format";
        return result;
    }
    
    std::string hrp = address.substr(0, pos);
    auto decoded = bech32::Decode(hrp, address);
    
    if (!decoded) {
        result.error_message = "Invalid Bech32 encoding";
        return result;
    }
    
    // Validate HRP matches expected network
    if (!isValidNetworkPrefix(hrp, expected_network)) {
        result.error_message = "Address network mismatch (expected " + 
                              networkToString(expected_network) + ", got " + hrp + ")";
        return result;
    }
    
    // The bech32 library has already decoded the witness version and program
    uint8_t witness_version = static_cast<uint8_t>(decoded->witver);
    std::vector<uint8_t> program = decoded->program;
    
    // Determine address type based on witness version and program size
    AddressType type = AddressType::Unknown;
    if (witness_version == 0) {
        if (program.size() == 20) {
            type = AddressType::P2WPKH;
        } else if (program.size() == 32) {
            type = AddressType::P2WSH;
        } else {
            result.error_message = "Invalid witness v0 program size: " + std::to_string(program.size());
            return result;
        }
    } else if (witness_version == 1) {
        if (program.size() == 32) {
            result.error_message = "Bech32m required for witness v1+ (Taproot)";
            return result; // This should be Bech32m, not Bech32
        }
    } else {
        result.error_message = "Unsupported witness version: " + std::to_string(witness_version);
        return result;
    }
    
    // Set network based on HRP
    Network detected_network = Network::Mainnet;
    if (hrp == "tdin") detected_network = Network::Testnet;
    else if (hrp == "rdin") detected_network = Network::Regtest;
    
    // Success
    result.valid = true;
    result.type = type;
    result.network = detected_network;
    result.program = program;
    result.witness_version = witness_version;
    
    return result;
}

AddressValidator::ValidationResult AddressValidator::validateBech32m(
    const std::string& address, 
    Network expected_network
) {
    ValidationResult result;
    
    // Extract HRP from address (everything before '1')
    size_t pos = address.find('1');
    if (pos == std::string::npos) {
        result.error_message = "Invalid Bech32m format";
        return result;
    }
    
    std::string hrp = address.substr(0, pos);
    auto decoded = bech32::Decode(hrp, address);
    
    if (!decoded) {
        result.error_message = "Invalid Bech32m encoding";
        return result;
    }
    
    // Validate HRP matches expected network
    if (!isValidNetworkPrefix(hrp, expected_network)) {
        result.error_message = "Address network mismatch (expected " + 
                              networkToString(expected_network) + ", got " + hrp + ")";
        return result;
    }
    
    uint8_t witness_version = static_cast<uint8_t>(decoded->witver);
    if (witness_version == 0) {
        result.error_message = "Bech32 required for witness v0 (not Bech32m)";
        return result;
    }
    if (witness_version > 16) {
        result.error_message = "Invalid witness version: " + std::to_string(witness_version);
        return result;
    }
    
    std::vector<uint8_t> program = decoded->program;
    
    // Determine address type based on witness version and program size
    AddressType type = AddressType::Unknown;
    if (witness_version == 1) {
        if (program.size() == 32) {
            // Validate Taproot public key
            if (TaprootAddress::isValidPubkey(program)) {
                type = AddressType::P2TR;
            } else {
                result.error_message = "Invalid Taproot public key";
                return result;
            }
        } else {
            result.error_message = "Invalid witness v1 program size: " + std::to_string(program.size());
            return result;
        }
    } else {
        result.error_message = "Unsupported witness version: " + std::to_string(witness_version);
        return result;
    }
    
    // Set network based on HRP
    Network detected_network = Network::Mainnet;
    if (hrp == "tdin") detected_network = Network::Testnet;
    else if (hrp == "rdin") detected_network = Network::Regtest;
    
    // Success
    result.valid = true;
    result.type = type;
    result.network = detected_network;
    result.program = program;
    result.witness_version = witness_version;
    
    return result;
}

AddressValidator::ValidationResult AddressValidator::validateBase58(
    const std::string& address, 
    Network expected_network
) {
    ValidationResult result;
    
    auto decoded = decodeBase58Check(address);
    if (!decoded || decoded->empty()) {
        result.error_message = "Invalid Base58Check encoding";
        return result;
    }
    
    // First byte is version/network indicator
    uint8_t version_byte = (*decoded)[0];
    
    // Determine address type and validate network
    AddressType detected_type;
    if (!isValidBase58Prefix(version_byte, expected_network, detected_type)) {
        result.error_message = "Invalid address version or network mismatch";
        return result;
    }
    
    // Extract program (skip version byte)
    std::vector<uint8_t> program(decoded->begin() + 1, decoded->end());
    
    // Validate program size
    if (!isValidProgramSize(detected_type, program.size())) {
        result.error_message = "Invalid program size for " + typeToString(detected_type);
        return result;
    }
    
    // Success
    result.valid = true;
    result.type = detected_type;
    result.network = expected_network;
    result.program = program;
    result.witness_version = 0; // Legacy addresses don't use witness versions
    
    return result;
}

// Low-level decoding helpers

std::optional<std::pair<std::string, std::vector<uint8_t>>> 
AddressValidator::decodeBech32(const std::string& address) {
    try {
        // Extract HRP from address (everything before '1')
        size_t pos = address.find('1');
        if (pos == std::string::npos) {
            return std::nullopt;
        }
        
        std::string hrp = address.substr(0, pos);
        auto result = bech32::Decode(hrp, address);
        
        if (!result) {
            return std::nullopt;
        }
        
        return std::make_pair(hrp, result->program);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::pair<std::string, std::vector<uint8_t>>> 
AddressValidator::decodeBech32m(const std::string& address) {
    try {
        // Extract HRP from address (everything before '1')
        size_t pos = address.find('1');
        if (pos == std::string::npos) {
            return std::nullopt;
        }
        
        std::string hrp = address.substr(0, pos);
        auto result = bech32::Decode(hrp, address);
        
        if (!result) {
            return std::nullopt;
        }
        
        // For now, treat as same as Bech32 - real Bech32m would have different checksum validation
        return std::make_pair(hrp, result->program);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::vector<uint8_t>> AddressValidator::decodeBase58Check(const std::string& address) {
    try {
        // For now, return nullopt since we don't have Base58Check implementation
        // This can be implemented later when needed for legacy address support
        return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

// Validation helpers

bool AddressValidator::isValidNetworkPrefix(const std::string& hrp, Network network) {
    std::string expected_hrp = getHRP(network);
    return hrp == expected_hrp;
}

bool AddressValidator::isValidBase58Prefix(
    uint8_t version_byte, 
    Network network, 
    AddressType& detected_type
) {
    // Dinero version bytes (following Bitcoin conventions)
    switch (network) {
        case Network::Mainnet:
            if (version_byte == 30) {  // 'D' prefix for P2PKH
                detected_type = AddressType::P2PKH;
                return true;
            }
            if (version_byte == 5) {   // '3' prefix for P2SH
                detected_type = AddressType::P2SH;
                return true;
            }
            break;
            
        case Network::Testnet:
        case Network::Regtest:
            if (version_byte == 111) { // 't' prefix for P2PKH
                detected_type = AddressType::P2PKH;
                return true;
            }
            if (version_byte == 196) { // '2' prefix for P2SH
                detected_type = AddressType::P2SH;
                return true;
            }
            break;
    }
    
    return false;
}

bool AddressValidator::isValidProgramSize(AddressType type, size_t program_size) {
    switch (type) {
        case AddressType::P2WPKH: return program_size == 20;
        case AddressType::P2WSH:  return program_size == 32;
        case AddressType::P2TR:   return program_size == 32;
        case AddressType::P2PKH:  return program_size == 20;
        case AddressType::P2SH:   return program_size == 20;
        case AddressType::Unknown: return false;
    }
    return false;
}

} // namespace din
