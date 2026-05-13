/**
 * Address Factory Implementation
 *
 * Unified address generation and validation for all address types.
 *
 * NOTE: This implementation provides compilable stubs. Full integration with
 * existing HDWallet derivation APIs is TODO.
 */

#include "wallet/address_factory.h"
#include "wallet/wallet_manager.h"
#include "common/logger.h"

#include <algorithm>

namespace dinero {
namespace wallet {

// ============================================================================
// Constructor / Destructor
// ============================================================================

AddressFactory::AddressFactory(WalletManager* wallet_manager)
    : wallet_manager_(wallet_manager)
{
    // Initialize index state for all schemes
    index_state_[AddressScheme::BIP84_SEGWIT] = IndexState{};
    index_state_[AddressScheme::BIP86_TAPROOT] = IndexState{};
    index_state_[AddressScheme::SILENT_PAYMENT] = IndexState{};
}

AddressFactory::~AddressFactory() = default;

// ============================================================================
// Address Generation
// ============================================================================

std::string AddressFactory::GetNewAddress(AddressScheme scheme, const std::string& label) {
    auto info = GetNewAddressInfo(scheme, label);
    return info.address;
}

AddressInfo AddressFactory::GetNewAddressInfo(AddressScheme scheme, const std::string& label) {
    AddressInfo info;
    info.scheme = scheme;
    info.label = label;
    info.is_change = false;

    if (!wallet_manager_ || !wallet_manager_->hasActiveWallet()) {
        g_logger.error("AddressFactory: No active wallet");
        return info;
    }

    // For now, delegate to existing WalletManager API for SegWit addresses
    // Full integration with all schemes is TODO
    if (scheme == AddressScheme::BIP84_SEGWIT || scheme == AddressScheme::BIP86_TAPROOT) {
        std::string addr_type = (scheme == AddressScheme::BIP86_TAPROOT) ? "taproot" : "legacy";
        info.address = wallet_manager_->getNewAddress(label, addr_type);
        info.account = 0;
        info.change = 0;
        info.index = GetNextIndex(scheme, false);
        IncrementIndex(scheme, false);

        g_logger.debug("AddressFactory generated " + SchemeName(scheme) + " address: " + info.address);
        return info;
    }

    // TODO: Implement other address types
    // - SILENT_PAYMENT: Use BIP352 derivation
    g_logger.warning("AddressFactory: " + SchemeName(scheme) + " generation not yet implemented");
    return info;
}

std::string AddressFactory::GetChangeAddress(AddressScheme scheme) {
    if (!wallet_manager_ || !wallet_manager_->hasActiveWallet()) {
        return "";
    }

    // Delegate to existing WalletManager for change addresses
    if (scheme == AddressScheme::BIP84_SEGWIT || scheme == AddressScheme::BIP86_TAPROOT) {
        std::string addr_type = (scheme == AddressScheme::BIP86_TAPROOT) ? "taproot" : "legacy";
        return wallet_manager_->getNewChangeAddress("", addr_type);
    }

    g_logger.warning("AddressFactory::GetChangeAddress not implemented for " + SchemeName(scheme));
    return "";
}

std::string AddressFactory::GetDefaultAddress(AddressScheme scheme) {
    if (!wallet_manager_ || !wallet_manager_->hasActiveWallet()) {
        return "";
    }

    // Return index 0 address for the scheme
    // TODO: Properly implement without incrementing index
    return GetNewAddress(scheme, "");
}

// ============================================================================
// Address Validation (Static)
// ============================================================================

ValidationResult AddressFactory::ValidateAddress(const std::string& address) {
    if (address.empty()) {
        return ValidationResult::Invalid("Empty address");
    }

    // Detect scheme
    AddressScheme scheme = DetectScheme(address);

    // Validate based on scheme
    switch (scheme) {
        case AddressScheme::BIP84_SEGWIT:
            if (address.length() < 42 || address.length() > 62) {
                return ValidationResult::Invalid("Invalid SegWit address length");
            }
            if (address.substr(0, 4) != "din1" && address.substr(0, 5) != "tdin1") {
                return ValidationResult::Invalid("Invalid SegWit address prefix");
            }
            return ValidationResult::Valid(scheme, address);

        case AddressScheme::BIP86_TAPROOT:
            if (address.length() < 42 || address.length() > 62) {
                return ValidationResult::Invalid("Invalid Taproot address length");
            }
            if (address.substr(0, 5) != "dint1" && address.substr(0, 6) != "tdint1") {
                return ValidationResult::Invalid("Invalid Taproot address prefix");
            }
            return ValidationResult::Valid(scheme, address);

        case AddressScheme::SILENT_PAYMENT:
            if (address.length() < 100) {
                return ValidationResult::Invalid("Silent payment address too short");
            }
            return ValidationResult::Valid(scheme, address);

        case AddressScheme::LEGACY_P2PKH:
        case AddressScheme::LEGACY_P2SH:
            if (address.length() < 25 || address.length() > 35) {
                return ValidationResult::Invalid("Invalid legacy address length");
            }
            return ValidationResult::Valid(scheme, address);

        default:
            return ValidationResult::Invalid("Unknown address format");
    }
}

bool AddressFactory::IsValid(const std::string& address) {
    return ValidateAddress(address).valid;
}

AddressScheme AddressFactory::DetectScheme(const std::string& address) {
    if (address.empty()) {
        return AddressScheme::BIP84_SEGWIT;  // Default
    }

    // Check prefixes (mainnet + testnet)
    if (address.length() >= 5 && (address.substr(0, 5) == "dint1" || address.substr(0, 6) == "tdint1")) {
        return AddressScheme::BIP86_TAPROOT;
    }
    if (address.length() >= 4 && (address.substr(0, 4) == "din1" || address.substr(0, 5) == "tdin1")) {
        return AddressScheme::BIP84_SEGWIT;
    }
    if (address.length() >= 3 && address.substr(0, 3) == "sp1") {
        return AddressScheme::SILENT_PAYMENT;
    }

    // Legacy formats
    if (!address.empty() && (address[0] == '1' || address[0] == 'm' || address[0] == 'n')) {
        return AddressScheme::LEGACY_P2PKH;
    }
    if (!address.empty() && (address[0] == '3' || address[0] == '2')) {
        return AddressScheme::LEGACY_P2SH;
    }

    return AddressScheme::BIP84_SEGWIT;  // Default fallback
}

std::string AddressFactory::SchemeName(AddressScheme scheme) {
    switch (scheme) {
        case AddressScheme::BIP84_SEGWIT:    return "SegWit";
        case AddressScheme::BIP86_TAPROOT:   return "Taproot";
        case AddressScheme::SILENT_PAYMENT:  return "Silent Payment";
        case AddressScheme::LEGACY_P2PKH:    return "Legacy P2PKH";
        case AddressScheme::LEGACY_P2SH:     return "Legacy P2SH";
        default:                             return "Unknown";
    }
}

std::string AddressFactory::SchemePrefix(AddressScheme scheme) {
    switch (scheme) {
        case AddressScheme::BIP84_SEGWIT:    return "din1";
        case AddressScheme::BIP86_TAPROOT:   return "dint1";
        case AddressScheme::SILENT_PAYMENT:  return "sp1";
        case AddressScheme::LEGACY_P2PKH:    return "1";
        case AddressScheme::LEGACY_P2SH:     return "3";
        default:                             return "din1";
    }
}

// ============================================================================
// Address Lookup
// ============================================================================

std::optional<AddressInfo> AddressFactory::GetAddressInfo(const std::string& address) const {
    if (!wallet_manager_ || !wallet_manager_->hasActiveWallet()) {
        return std::nullopt;
    }

    // Check if address belongs to wallet
    if (!IsOurAddress(address)) {
        return std::nullopt;
    }

    AddressInfo info;
    info.address = address;
    info.scheme = DetectScheme(address);

    // Get label from WalletManager
    auto label_opt = wallet_manager_->getAddressLabel(address);
    if (label_opt) {
        info.label = *label_opt;
    }

    return info;
}

std::vector<AddressInfo> AddressFactory::GetAddresses(
    AddressScheme scheme,
    bool include_change
) const {
    std::vector<AddressInfo> addresses;

    auto all = GetAllAddresses(include_change);
    for (const auto& addr : all) {
        if (addr.scheme == scheme) {
            addresses.push_back(addr);
        }
    }

    return addresses;
}

std::vector<AddressInfo> AddressFactory::GetAllAddresses(bool include_change) const {
    std::vector<AddressInfo> addresses;

    if (!wallet_manager_ || !wallet_manager_->hasActiveWallet()) {
        return addresses;
    }

    // Get addresses from WalletManager
    auto wallet_addresses = wallet_manager_->listAddresses(true);
    for (const auto& wa : wallet_addresses) {
        // Skip change addresses if not requested
        if (!include_change && wa.change != 0) {
            continue;
        }

        AddressInfo info;
        info.address = wa.address;
        info.scheme = DetectScheme(wa.address);
        if (wa.label) {
            info.label = *wa.label;
        }
        info.is_change = (wa.change != 0);
        info.account = wa.account;
        info.index = wa.index;
        addresses.push_back(info);
    }

    return addresses;
}

bool AddressFactory::IsOurAddress(const std::string& address) const {
    if (!wallet_manager_) {
        return false;
    }
    return wallet_manager_->isAddressMine(address);
}

// ============================================================================
// Address Labels
// ============================================================================

bool AddressFactory::SetLabel(const std::string& address, const std::string& label) {
    if (!wallet_manager_ || !wallet_manager_->hasActiveWallet()) {
        return false;
    }
    wallet_manager_->setAddressLabel(address, label);
    return true;
}

std::string AddressFactory::GetLabel(const std::string& address) const {
    if (!wallet_manager_ || !wallet_manager_->hasActiveWallet()) {
        return "";
    }
    auto label = wallet_manager_->getAddressLabel(address);
    return label.value_or("");
}

std::vector<std::string> AddressFactory::GetAddressesByLabel(const std::string& label) const {
    std::vector<std::string> addresses;

    auto all = GetAllAddresses(true);
    for (const auto& addr : all) {
        if (addr.label == label) {
            addresses.push_back(addr.address);
        }
    }

    return addresses;
}

// ============================================================================
// Static Methods
// ============================================================================

std::vector<AddressScheme> AddressFactory::GetSupportedSchemes() {
    return {
        AddressScheme::BIP84_SEGWIT,
        AddressScheme::BIP86_TAPROOT,
        AddressScheme::SILENT_PAYMENT
    };
}

// ============================================================================
// Private Methods
// ============================================================================

std::string AddressFactory::GenerateBIP84Address(uint32_t /*account*/, bool /*is_change*/, uint32_t /*index*/) {
    // TODO: Implement direct derivation
    // For now, use WalletManager
    return "";
}

std::string AddressFactory::GenerateBIP86Address(uint32_t /*account*/, bool /*is_change*/, uint32_t /*index*/) {
    // TODO: Implement direct derivation
    return "";
}

std::string AddressFactory::GenerateSilentPaymentAddress() {
    // TODO: Implement BIP352 silent payment address
    return "";
}

uint32_t AddressFactory::GetNextIndex(AddressScheme scheme, bool is_change) {
    auto it = index_state_.find(scheme);
    if (it == index_state_.end()) {
        return 0;
    }
    return is_change ? it->second.internal_index : it->second.external_index;
}

void AddressFactory::IncrementIndex(AddressScheme scheme, bool is_change) {
    auto it = index_state_.find(scheme);
    if (it == index_state_.end()) {
        return;
    }
    if (is_change) {
        ++it->second.internal_index;
    } else {
        ++it->second.external_index;
    }
}

} // namespace wallet
} // namespace dinero
