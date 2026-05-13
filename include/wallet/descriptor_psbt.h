#pragma once

#include "dinero/core/wallet/psbt.h"
#include "wallet/bip86_descriptor.h"
#include "wallet/bip84_descriptor.h"
#include "wallet/descriptor_activation.h"
#include <string>
#include <vector>
#include <optional>

namespace dinero {

/**
 * @brief PSBT creation from active descriptors (Phase 2 Step 3)
 *
 * This module bridges the descriptor activation gate with PSBT creation,
 * ensuring that PSBTs inherit wallet policy and signing capability from
 * the activated descriptor.
 *
 * Security guarantees:
 * - BIP86 descriptors create PSBTs with BIP86 guardrails enforced
 * - BIP84 descriptors create standard SegWit PSBTs
 * - Watch-only descriptors (active=false) cannot sign
 * - External descriptors require hardware wallet signing
 */

/**
 * @brief Input UTXO for PSBT creation
 */
struct PsbtInputInfo {
    std::string txid;                 // Previous transaction ID (hex)
    uint32_t vout;                    // Output index
    uint64_t amount;                  // Amount in una
    std::vector<uint8_t> scriptPubKey; // ScriptPubKey of the UTXO
    uint32_t sequence = 0xFFFFFFFD;   // Default: RBF enabled
    std::optional<std::vector<uint8_t>> witness_utxo; // Full witness UTXO for SegWit
    uint32_t address_index = 0;       // BIP32 address index (for derivation path)
};

/**
 * @brief Output for PSBT creation
 */
struct PsbtOutputInfo {
    std::string address;              // Destination address
    uint64_t amount;                  // Amount in una
};

/**
 * @brief Descriptor-based PSBT creation request
 */
struct DescriptorPsbtRequest {
    std::string descriptor;                   // Active descriptor (with checksum)
    std::vector<PsbtInputInfo> inputs;        // UTXOs to spend
    std::vector<PsbtOutputInfo> outputs;      // Destination outputs
    uint32_t locktime = 0;                    // Transaction locktime
    bool include_bip32_derivation = true;     // Add BIP32 derivation paths
    std::optional<uint32_t> sighash_type;     // Override default sighash type
};

/**
 * @brief Result of descriptor-based PSBT creation
 */
struct DescriptorPsbtResult {
    bool success = false;
    din::Psbt psbt;                           // Created PSBT
    std::string psbt_base64;                  // Base64-encoded PSBT
    WalletPolicy wallet_policy;               // Inherited wallet policy
    SigningCapability signing_capability;     // Inherited signing capability
    std::string descriptor_type;              // "tr" or "wpkh"
    std::string error;                        // Error message if failed

    // Metadata
    size_t input_count = 0;
    size_t output_count = 0;
    uint64_t total_input_amount = 0;
    uint64_t total_output_amount = 0;
    uint64_t fee = 0;
};

/**
 * @brief Factory for creating PSBTs from active descriptors
 *
 * This class ensures PSBTs inherit wallet policy and signing capability
 * from the descriptor activation gate.
 *
 * Usage:
 *   1. Import descriptor via wallet.importdescriptors (with activation gate)
 *   2. Create PSBT using descriptor: createPsbtFromDescriptor()
 *   3. Sign PSBT with wallet keys (enforces BIP86 guardrails)
 *   4. Finalize and broadcast
 */
class DescriptorPsbtFactory {
public:
    /**
     * @brief Create PSBT from active descriptor
     *
     * @param request Descriptor and UTXO information
     * @param wallet_policy Wallet policy (from activation gate)
     * @param wallet_fingerprint Wallet's master fingerprint (for validation)
     * @return Result with PSBT or error
     *
     * Security:
     * - Validates descriptor matches wallet policy
     * - Adds BIP32 derivation paths for signing
     * - Prepares PSBT for BIP86 guardrails enforcement during signing
     */
    static DescriptorPsbtResult createPsbtFromDescriptor(
        const DescriptorPsbtRequest& request,
        WalletPolicy wallet_policy,
        const std::string& wallet_fingerprint
    );

    /**
     * @brief Create PSBT for BIP86 Taproot descriptor
     *
     * Internal helper that creates BIP86-compliant PSBT with:
     * - TAP_INTERNAL_KEY field for each input
     * - BIP32 derivation paths
     * - No TAP_MERKLE_ROOT (key-path only)
     * - No TAP_LEAF_SCRIPT (key-path only)
     */
    static DescriptorPsbtResult createBIP86Psbt(
        const DescriptorPsbtRequest& request,
        const din::BIP86DescriptorFactory::ParsedBIP86& parsed,
        const std::string& wallet_fingerprint
    );

    /**
     * @brief Create PSBT for BIP84 SegWit descriptor
     *
     * Internal helper that creates BIP84-compliant PSBT with:
     * - WITNESS_UTXO for each input
     * - BIP32 derivation paths
     */
    static DescriptorPsbtResult createBIP84Psbt(
        const DescriptorPsbtRequest& request,
        const din::BIP84DescriptorFactory::ParsedBIP84& parsed,
        const std::string& wallet_fingerprint
    );

private:
    /**
     * @brief Add BIP32 derivation information to PSBT input
     *
     * This is crucial for hardware wallets and signing validation.
     */
    static void addBip32DerivationInfo(
        din::Psbt& psbt,
        size_t input_idx,
        const std::vector<uint8_t>& pubkey,
        const std::string& fingerprint,
        const std::vector<uint32_t>& derivation_path
    );

    /**
     * @brief Convert derivation path string to vector of indices
     *
     * Example: "86h/1448h/0h/0" → [0x80000056, 0x800005A8, 0x80000000, 0]
     */
    static std::vector<uint32_t> parseDerivationPath(const std::string& path_str);

    /**
     * @brief Convert hex string to bytes
     */
    static std::vector<uint8_t> hexToBytes(const std::string& hex);
};

} // namespace dinero
