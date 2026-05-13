#pragma once

/**
 * Address Factory
 *
 * Unified interface for generating supported transparent address types:
 * - BIP84 SegWit (din1...)
 * - BIP86 Taproot (dint1...)
 * - Silent Payment (sp1...)
 *
 * Handles the complexity of different derivation paths, key types,
 * and address encoding schemes behind a single API.
 *
 * Usage:
 *   AddressFactory factory(wallet);
 *
 *   // Get a new receiving address (default type)
 *   auto addr = factory.GetNewAddress();
 *
 *   // Validate any address
 *   bool valid = factory.ValidateAddress("din1...");
 */

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace dinero {

// Forward declarations
class WalletManager;
class HDWallet;

namespace wallet {

/**
 * Address generation scheme
 */
enum class AddressScheme {
    BIP84_SEGWIT,       // din1... - Native SegWit (default)
    BIP86_TAPROOT,      // dint1... - Taproot
    SILENT_PAYMENT,     // sp1... - BIP352 Silent Payments
    LEGACY_P2PKH,       // 1... - Legacy (deprecated)
    LEGACY_P2SH         // 3... - P2SH-wrapped SegWit (deprecated)
};

/**
 * Address derivation information
 */
struct AddressInfo {
    std::string address;
    AddressScheme scheme;
    std::string label;

    // Derivation path components
    uint32_t account = 0;
    uint32_t change = 0;     // 0 = external, 1 = internal/change
    uint32_t index = 0;

    // Key information (for advanced users)
    std::string pubkey_hex;  // Public key in hex
    std::string path;        // Full derivation path (e.g., "m/84'/1448'/0'/0/0")

    // Metadata
    bool is_used = false;
    bool is_change = false;
    uint64_t total_received = 0;
    int tx_count = 0;
};

/**
 * Address validation result
 */
struct ValidationResult {
    bool valid = false;
    AddressScheme detected_scheme = AddressScheme::BIP84_SEGWIT;
    std::string error_message;
    std::string normalized_address;  // Canonical form

    // Network info
    bool is_mainnet = true;
    bool is_testnet = false;

    static ValidationResult Valid(AddressScheme scheme, const std::string& normalized) {
        ValidationResult r;
        r.valid = true;
        r.detected_scheme = scheme;
        r.normalized_address = normalized;
        return r;
    }

    static ValidationResult Invalid(const std::string& error) {
        ValidationResult r;
        r.valid = false;
        r.error_message = error;
        return r;
    }
};

/**
 * Address Factory
 *
 * Unified address generation and validation for all address types.
 */
class AddressFactory {
public:
    explicit AddressFactory(WalletManager* wallet_manager);
    ~AddressFactory();

    // Non-copyable
    AddressFactory(const AddressFactory&) = delete;
    AddressFactory& operator=(const AddressFactory&) = delete;

    // ========================================================================
    // Address Generation
    // ========================================================================

    /**
     * Get a new receiving address
     *
     * @param scheme Address scheme (default: BIP84_SEGWIT)
     * @param label Optional label for the address
     * @return New address string
     */
    std::string GetNewAddress(
        AddressScheme scheme = AddressScheme::BIP84_SEGWIT,
        const std::string& label = ""
    );

    /**
     * Get a new receiving address with full info
     *
     * @param scheme Address scheme
     * @param label Optional label
     * @return AddressInfo with all derivation details
     */
    AddressInfo GetNewAddressInfo(
        AddressScheme scheme = AddressScheme::BIP84_SEGWIT,
        const std::string& label = ""
    );

    /**
     * Get a change address (internal use)
     *
     * @param scheme Address scheme (should match transaction type)
     * @return Change address string
     */
    std::string GetChangeAddress(AddressScheme scheme = AddressScheme::BIP84_SEGWIT);

    /**
     * Get the default receiving address (no new derivation)
     *
     * @param scheme Address scheme
     * @return Default address for receiving
     */
    std::string GetDefaultAddress(AddressScheme scheme = AddressScheme::BIP84_SEGWIT);

    // ========================================================================
    // Address Validation
    // ========================================================================

    /**
     * Validate an address
     *
     * @param address Address string to validate
     * @return ValidationResult with details
     */
    static ValidationResult ValidateAddress(const std::string& address);

    /**
     * Quick validation check
     *
     * @param address Address to check
     * @return true if valid
     */
    static bool IsValid(const std::string& address);

    /**
     * Detect address scheme from string
     *
     * @param address Address to analyze
     * @return Detected scheme, or BIP84_SEGWIT if unknown
     */
    static AddressScheme DetectScheme(const std::string& address);

    /**
     * Get human-readable scheme name
     *
     * @param scheme Address scheme
     * @return Name (e.g., "Taproot", "SegWit")
     */
    static std::string SchemeName(AddressScheme scheme);

    /**
     * Get address prefix for scheme
     *
     * @param scheme Address scheme
     * @return Prefix (e.g., "din1", "dint1")
     */
    static std::string SchemePrefix(AddressScheme scheme);

    // ========================================================================
    // Address Lookup
    // ========================================================================

    /**
     * Get info for an existing address
     *
     * @param address Address to look up
     * @return AddressInfo if found, nullopt otherwise
     */
    std::optional<AddressInfo> GetAddressInfo(const std::string& address) const;

    /**
     * Get all addresses of a specific scheme
     *
     * @param scheme Address scheme to filter by
     * @param include_change Include change addresses
     * @return Vector of address info
     */
    std::vector<AddressInfo> GetAddresses(
        AddressScheme scheme,
        bool include_change = false
    ) const;

    /**
     * Get all addresses (all schemes)
     *
     * @param include_change Include change addresses
     * @return Vector of all address info
     */
    std::vector<AddressInfo> GetAllAddresses(bool include_change = false) const;

    /**
     * Check if address belongs to this wallet
     *
     * @param address Address to check
     * @return true if address is ours
     */
    bool IsOurAddress(const std::string& address) const;

    // ========================================================================
    // Address Labels
    // ========================================================================

    /**
     * Set label for an address
     *
     * @param address Address to label
     * @param label New label
     * @return true if successful
     */
    bool SetLabel(const std::string& address, const std::string& label);

    /**
     * Get label for an address
     *
     * @param address Address to query
     * @return Label, or empty string if none
     */
    std::string GetLabel(const std::string& address) const;

    /**
     * Get addresses by label
     *
     * @param label Label to search for
     * @return Addresses with this label
     */
    std::vector<std::string> GetAddressesByLabel(const std::string& label) const;

    // ========================================================================
    // Configuration
    // ========================================================================

    /**
     * Set default address scheme for new addresses
     */
    void SetDefaultScheme(AddressScheme scheme) { default_scheme_ = scheme; }

    /**
     * Get default address scheme
     */
    AddressScheme GetDefaultScheme() const { return default_scheme_; }

    /**
     * Set gap limit for address discovery
     */
    void SetGapLimit(uint32_t limit) { gap_limit_ = limit; }

    /**
     * Get supported schemes
     *
     * @return Vector of supported address schemes
     */
    static std::vector<AddressScheme> GetSupportedSchemes();

private:
    // Internal generation methods
    std::string GenerateBIP84Address(uint32_t account, bool is_change, uint32_t index);
    std::string GenerateBIP86Address(uint32_t account, bool is_change, uint32_t index);
    std::string GenerateSilentPaymentAddress();

    // Index management
    uint32_t GetNextIndex(AddressScheme scheme, bool is_change);
    void IncrementIndex(AddressScheme scheme, bool is_change);

    WalletManager* wallet_manager_;
    AddressScheme default_scheme_ = AddressScheme::BIP84_SEGWIT;
    uint32_t gap_limit_ = 20;

    // Address index tracking (per scheme, per change flag)
    struct IndexState {
        uint32_t external_index = 0;
        uint32_t internal_index = 0;
    };
    std::map<AddressScheme, IndexState> index_state_;
};

} // namespace wallet
} // namespace dinero
