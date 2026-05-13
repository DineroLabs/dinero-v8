#include "wallet/key_origin.h"
#include "wallet/retired_coin_type_guard.h"
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace dinero {
namespace wallet {

std::string KeyOriginInfo::toString() const {
    std::ostringstream oss;

    // Fingerprint as 8 hex digits
    oss << "[" << std::hex << std::setfill('0') << std::setw(8) << fingerprint;

    // Path components
    for (uint32_t component : path) {
        uint32_t index = component & ~HARDENED_BIT;
        bool hardened = (component & HARDENED_BIT) != 0;

        oss << "/" << std::dec << index;
        if (hardened) {
            oss << "'";
        }
    }

    oss << "]";
    return oss.str();
}

std::string KeyOriginInfo::getPathString() const {
    std::ostringstream oss;
    oss << "m";

    for (uint32_t component : path) {
        uint32_t index = component & ~HARDENED_BIT;
        bool hardened = (component & HARDENED_BIT) != 0;

        oss << "/" << index;
        if (hardened) {
            oss << "'";
        }
    }

    return oss.str();
}

std::optional<KeyOriginInfo> KeyOriginInfo::parse(const std::string& str) {
    // Expected format: "[fingerprint/path/components]"
    // Example: "[f23a9c12/86'/1448'/0'/0/12]"

    if (str.empty() || str.front() != '[' || str.back() != ']') {
        return std::nullopt;
    }

    std::string content = str.substr(1, str.length() - 2);

    // Split by '/'
    std::vector<std::string> parts;
    std::istringstream iss(content);
    std::string part;
    while (std::getline(iss, part, '/')) {
        parts.push_back(part);
    }

    if (parts.empty()) {
        return std::nullopt;
    }

    // First part is fingerprint (8 hex digits)
    KeyOriginInfo result;
    try {
        result.fingerprint = std::stoul(parts[0], nullptr, 16);
    } catch (...) {
        return std::nullopt;
    }

    // Remaining parts are path components
    for (size_t i = 1; i < parts.size(); ++i) {
        std::string component_str = parts[i];
        bool hardened = false;

        // Check for hardened marker (')
        if (!component_str.empty() && component_str.back() == '\'') {
            hardened = true;
            component_str.pop_back();
        }

        // Parse index
        uint32_t index;
        try {
            index = std::stoul(component_str);
        } catch (...) {
            return std::nullopt;
        }

        // Add to path
        if (hardened) {
            index |= HARDENED_BIT;
        }
        result.path.push_back(index);
    }

    if (PathUsesRetiredLegacyCoinType(result.path)) {
        return std::nullopt;
    }

    return result;
}

std::optional<KeyOriginInfo> KeyOriginInfo::parsePathString(const std::string& path_str) {
    // Expected format: "m/86'/1448'/0'/0/12"

    if (path_str.empty() || path_str[0] != 'm') {
        return std::nullopt;
    }

    KeyOriginInfo result;
    result.fingerprint = 0;  // Caller must set this

    // Split by '/'
    std::vector<std::string> parts;
    std::istringstream iss(path_str);
    std::string part;
    while (std::getline(iss, part, '/')) {
        if (part != "m") {  // Skip the 'm' prefix
            parts.push_back(part);
        }
    }

    if (parts.empty()) {
        return std::nullopt;
    }

    // Parse each path component
    for (const auto& component_str : parts) {
        if (component_str.empty()) {
            continue;
        }

        bool hardened = false;
        std::string num_str = component_str;

        // Check for hardened marker (')
        if (component_str.back() == '\'') {
            hardened = true;
            num_str = component_str.substr(0, component_str.length() - 1);
        }

        // Parse index
        uint32_t index;
        try {
            index = std::stoul(num_str);
        } catch (...) {
            return std::nullopt;
        }

        // Add to path
        if (hardened) {
            index |= HARDENED_BIT;
        }
        result.path.push_back(index);
    }

    if (PathUsesRetiredLegacyCoinType(result.path)) {
        return std::nullopt;
    }

    return result;
}

} // namespace wallet
} // namespace dinero
