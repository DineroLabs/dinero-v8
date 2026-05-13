#include "wallet/bip84_descriptor.h"
#include "wallet/retired_coin_type_guard.h"
#include <sstream>
#include <regex>

namespace din {

namespace {

void ValidateBIP84ConfigCoinType(const BIP84DescriptorFactory::BIP84Config& config) {
    const std::vector<uint32_t> path = {
        84 | dinero::wallet::HARDENED_DERIVATION_BIT,
        config.coin_type | dinero::wallet::HARDENED_DERIVATION_BIT,
        config.account | dinero::wallet::HARDENED_DERIVATION_BIT,
    };
    dinero::wallet::RejectRetiredLegacyCoinTypePath(path, "BIP84 descriptor creation");
    dinero::wallet::RejectNonCanonicalCoinTypePath(path, "BIP84 descriptor creation");
}

} // namespace

std::string BIP84DescriptorFactory::createReceiveDescriptor(const BIP84Config& config) {
    ValidateBIP84ConfigCoinType(config);
    // Format: wpkh([fingerprint/84h/coin_type'/account']xpub/0/*)
    std::ostringstream oss;
    oss << "wpkh([" << config.master_fingerprint << "/84h/" 
        << config.coin_type << "h/" << config.account << "h]"
        << config.account_xpub << "/0/*)";
    return oss.str();
}

std::string BIP84DescriptorFactory::createChangeDescriptor(const BIP84Config& config) {
    ValidateBIP84ConfigCoinType(config);
    // Format: wpkh([fingerprint/84h/coin_type'/account']xpub/1/*)
    std::ostringstream oss;
    oss << "wpkh([" << config.master_fingerprint << "/84h/" 
        << config.coin_type << "h/" << config.account << "h]"
        << config.account_xpub << "/1/*)";
    return oss.str();
}

BIP84DescriptorFactory::ParsedBIP84 BIP84DescriptorFactory::parseDescriptor(const std::string& descriptor) {
    ParsedBIP84 result;
    
    // Parse BIP84 descriptor format: wpkh([fingerprint/84h/coin_type'/account']xpub/change/*)
    std::regex bip84_regex(R"(wpkh\(\[([0-9a-fA-F]{8})/84h/(\d+)h/(\d+)h\]([a-zA-Z0-9]+)/([01])/\*\))");
    std::smatch matches;
    
    if (std::regex_match(descriptor, matches, bip84_regex)) {
        result.valid = true;
        result.fingerprint = matches[1].str();
        result.xpub = matches[4].str();
        result.is_change = (matches[5].str() == "1");
        
        // Extract derivation path
        uint32_t coin_type = std::stoul(matches[2].str());
        uint32_t account = std::stoul(matches[3].str());
        result.derivation_path = {84 | 0x80000000, coin_type | 0x80000000, account | 0x80000000};
        if (dinero::wallet::PathUsesRetiredLegacyCoinType(result.derivation_path)) {
            result.valid = false;
            result.error = dinero::wallet::RetiredLegacyCoinTypeError("Invalid BIP84 descriptor");
        } else if (dinero::wallet::PathUsesNonCanonicalCoinType(result.derivation_path)) {
            result.valid = false;
            result.error = dinero::wallet::NonCanonicalCoinTypeError("Invalid BIP84 descriptor", coin_type);
        }

    } else {
        result.valid = false;
        result.error = "Invalid BIP84 descriptor format";
    }
    
    return result;
}

bool BIP84DescriptorFactory::validateBIP84Descriptor(const std::string& descriptor, std::string& error) {
    auto parsed = parseDescriptor(descriptor);
    if (!parsed.valid) {
        error = parsed.error;
        return false;
    }
    
    // Additional validation
    if (parsed.fingerprint.length() != 8) {
        error = "Invalid fingerprint length (must be 8 hex chars)";
        return false;
    }
    
    if (parsed.xpub.length() < 100) { // xpub should be ~111 chars
        error = "Invalid xpub length";
        return false;
    }
    
    return true;
}

std::pair<std::string, std::string> BIP84DescriptorFactory::createDefaultDescriptors(
    const std::string& master_fingerprint, 
    const std::string& account_xpub,
    uint32_t coin_type) {
    
    BIP84Config config;
    config.master_fingerprint = master_fingerprint;
    config.account_xpub = account_xpub;
    config.coin_type = coin_type;
    config.account = 0; // Default to account 0
    
    return {
        createReceiveDescriptor(config),
        createChangeDescriptor(config)
    };
}

bool BIP84DescriptorFactory::parseKeyOrigin(const std::string& origin_str, 
                                           std::string& fingerprint, 
                                           std::vector<uint32_t>& path) {
    // Parse key origin format: [fingerprint/84h/1448h/0h]
    std::regex origin_regex(R"(\[([0-9a-fA-F]{8})/(.+)\])");
    std::smatch matches;
    
    if (!std::regex_match(origin_str, matches, origin_regex)) {
        return false;
    }
    
    fingerprint = matches[1].str();
    std::string path_str = matches[2].str();
    
    // Parse derivation path
    std::regex path_regex(R"((\d+)h?)");
    std::sregex_iterator iter(path_str.begin(), path_str.end(), path_regex);
    std::sregex_iterator end;
    
    path.clear();
    for (; iter != end; ++iter) {
        std::smatch match = *iter;
        uint32_t index = std::stoul(match[1].str());
        bool hardened = match[0].str().back() == 'h';
        path.push_back(hardened ? (index | 0x80000000) : index);
    }
    
    return true;
}

std::string BIP84DescriptorFactory::formatKeyOrigin(const std::string& fingerprint, 
                                                   const std::vector<uint32_t>& path) {
    std::ostringstream oss;
    oss << "[" << fingerprint;
    
    for (uint32_t index : path) {
        oss << "/";
        if (index & 0x80000000) {
            oss << (index & 0x7FFFFFFF) << "h";
        } else {
            oss << index;
        }
    }
    
    oss << "]";
    return oss.str();
}

} // namespace din
