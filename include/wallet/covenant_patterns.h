/**
 * Phase C.5: Covenant Patterns - High-Level Wallet Recipes
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * PATTERN LIBRARY PRINCIPLES
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Patterns = Composition of Primitives
 * - Use only CTV + CSFS + Timelocks
 * - No new opcodes or consensus changes
 * - Construction helpers ONLY (never validation)
 *
 * Categories:
 * 1. Vault Patterns - Secure storage with time delays
 * 2. Recovery Flows - Time-delayed and social recovery
 * 3. Multisig Covenants - Restricted spending conditions
 *
 * Foundation:
 * - Phase C.1: Consensus primitives (VerifyCTV, VerifySignatureFromStack)
 * - Phase C.3: Construction helpers (buildCTVTemplate, signCSFSDelegation)
 * - Phase C.4: RPC endpoints (wallet.createctvtemplate, etc.)
 * - Phase C.5: Patterns (THIS FILE)
 *
 * ═══════════════════════════════════════════════════════════════════════════
 */

#pragma once

#include "wallet/covenant_builders.h"
#include "wallet/canonical_wallet_utxo.h"
#include "primitives/transaction.h"
#include <vector>
#include <string>
#include <optional>

namespace dinero {
namespace wallet {
namespace patterns {

// ============================================================================
// VAULT PATTERNS
// ============================================================================

/**
 * Simple Vault Pattern
 *
 * Two-step withdrawal with time delay for theft protection
 *
 * Flow:
 * 1. Lock: Funds → Vault (CTV-locked to unvault tx)
 * 2. Unvault: Vault → Unvault (broadcasts withdrawal intent, starts delay)
 * 3. Withdraw: Unvault → Destination (after delay, completes withdrawal)
 *
 * Security Model:
 * - 24-hour delay gives time to detect and react to theft
 * - If unvault detected, can sweep to emergency address
 * - Attacker must wait, giving owner time to respond
 *
 * Use Cases:
 * - Cold storage with withdrawal protection
 * - Corporate treasury
 * - Large holdings requiring extra security
 */
struct SimpleVaultPattern {
    // Vault stage (funds locked here)
    std::vector<uint8_t> vault_script;      // CTV script committing to unvault
    std::array<uint8_t, 32> vault_hash;     // CTV template hash

    // Unvault stage (withdrawal intent)
    std::vector<uint8_t> unvault_script;    // Time-delayed script
    uint32_t unvault_delay_blocks;          // Delay (e.g., 144 = ~24h)

    // Final destination
    CTVOutput final_output;                 // Where funds ultimately go

    // Metadata
    std::string label;                      // Human-readable label

    SimpleVaultPattern() : unvault_delay_blocks(144) {}
};

/**
 * Create a simple vault pattern
 *
 * @param final_address  Destination address (Bech32)
 * @param amount         Amount in una
 * @param delay_blocks   Unvault delay (default: 144 blocks ≈ 24 hours)
 * @param label          Optional label
 * @return               Simple vault pattern ready for funding
 */
SimpleVaultPattern createSimpleVault(
    const std::string& final_address,
    uint64_t amount,
    uint32_t delay_blocks = 144,
    const std::string& label = "simple-vault"
);

/**
 * Recovery Vault Pattern
 *
 * Vault with emergency recovery key (two paths)
 *
 * Normal Path:
 * 1. Vault → Unvault (24h delay)
 * 2. Unvault → Destination
 *
 * Recovery Path:
 * 1. Vault → Recovery (after 6 months)
 * 2. Recovery pubkey can spend directly
 *
 * Security Model:
 * - Normal operations use vault path (secure)
 * - If main key lost, recovery key activates after long delay
 * - Recovery delay long enough that owner would notice
 *
 * Use Cases:
 * - Inheritance planning
 * - Key backup (lawyer, family member)
 * - Lost key recovery
 */
struct RecoveryVaultPattern {
    // Normal vault path
    SimpleVaultPattern vault;               // Standard vault flow

    // Recovery path
    std::vector<uint8_t> recovery_pubkey;   // Emergency recovery key
    uint32_t recovery_delay_blocks;         // Recovery timelock (e.g., 25920 = ~6mo)
    std::vector<uint8_t> recovery_script;   // Alternative spending path

    // Metadata
    std::string recovery_label;             // Who holds recovery key

    RecoveryVaultPattern() {}
};

/**
 * Create a recovery vault pattern
 *
 * @param final_address    Normal destination address
 * @param amount           Amount in una
 * @param recovery_pubkey  Recovery public key (32-byte x-only)
 * @param vault_delay      Normal vault delay (default: 144 blocks)
 * @param recovery_delay   Recovery activation delay (default: 25920 blocks ≈ 6mo)
 * @param label            Optional label
 * @return                 Recovery vault pattern
 */
RecoveryVaultPattern createRecoveryVault(
    const std::string& final_address,
    uint64_t amount,
    const std::vector<uint8_t>& recovery_pubkey,
    uint32_t vault_delay = 144,
    uint32_t recovery_delay = 25920,
    const std::string& label = "recovery-vault"
);

// ============================================================================
// RECOVERY FLOW PATTERNS
// ============================================================================

/**
 * Time-Delayed Recovery Pattern
 *
 * Two spending paths: Owner (immediate) OR Recovery (after delay)
 *
 * Script Logic:
 * IF <owner_sig>
 *     CHECKSIG
 * ELSE
 *     <delay> CHECKSEQUENCEVERIFY DROP
 *     <recovery_sig> CHECKSIG
 * ENDIF
 *
 * Security Model:
 * - Owner maintains full control (can spend anytime)
 * - Recovery key activates only after long inactivity
 * - Prevents accidental loss of large holdings
 *
 * Use Cases:
 * - Inheritance (heirs get access after death)
 * - Lost key backup
 * - Long-term savings with safety net
 */
struct TimeDelayedRecoveryPattern {
    std::vector<uint8_t> owner_pubkey;      // Primary key (immediate access)
    std::vector<uint8_t> recovery_pubkey;   // Backup key (delayed access)
    uint32_t recovery_delay_blocks;         // Delay before recovery activates
    std::vector<uint8_t> spending_script;   // Complete spending script

    // Metadata
    std::string owner_label;                // Who owns this
    std::string recovery_label;             // Who recovers this

    TimeDelayedRecoveryPattern() : recovery_delay_blocks(25920) {}
};

/**
 * Create time-delayed recovery pattern
 *
 * @param owner_pubkey      Owner's public key (32-byte x-only)
 * @param recovery_pubkey   Recovery public key (32-byte x-only)
 * @param delay_blocks      Recovery delay (default: 25920 blocks ≈ 6mo)
 * @param owner_label       Optional owner label
 * @param recovery_label    Optional recovery label
 * @return                  Time-delayed recovery pattern
 */
TimeDelayedRecoveryPattern createTimeDelayedRecovery(
    const std::vector<uint8_t>& owner_pubkey,
    const std::vector<uint8_t>& recovery_pubkey,
    uint32_t delay_blocks = 25920,
    const std::string& owner_label = "owner",
    const std::string& recovery_label = "recovery"
);

/**
 * Social Recovery Pattern (K-of-N)
 *
 * Two spending paths: Owner (immediate) OR Guardians (after delay)
 *
 * Script Logic:
 * IF <owner_sig>
 *     CHECKSIG
 * ELSE
 *     <delay> CHECKSEQUENCEVERIFY DROP
 *     <guardian_1_sig> CHECKSIGADD
 *     <guardian_2_sig> CHECKSIGADD
 *     ...
 *     <threshold> GREATERTHANOREQUAL
 * ENDIF
 *
 * Security Model:
 * - Owner has full control (can spend anytime)
 * - Guardians can recover only after delay + require threshold
 * - Prevents guardian theft (need K guardians + wait)
 * - Owner activity resets timelock (prevents accidental recovery)
 *
 * Use Cases:
 * - Account recovery (3-of-5 friends)
 * - Wallet inheritance (family members)
 * - Corporate key recovery (board members)
 */
struct SocialRecoveryPattern {
    std::vector<uint8_t> owner_pubkey;              // Primary owner
    std::vector<std::vector<uint8_t>> guardian_pubkeys;  // Recovery guardians
    size_t threshold;                               // K (required guardians)
    uint32_t recovery_delay_blocks;                 // Delay before guardians can act
    std::vector<uint8_t> spending_script;           // Complete spending script

    // Metadata
    std::string owner_label;
    std::vector<std::string> guardian_labels;       // Who are the guardians

    SocialRecoveryPattern() : threshold(0), recovery_delay_blocks(25920) {}
};

/**
 * Create social recovery pattern (K-of-N guardians)
 *
 * @param owner_pubkey       Owner's public key
 * @param guardian_pubkeys   List of guardian public keys
 * @param threshold          K (number of guardians required)
 * @param delay_blocks       Recovery delay (default: 25920 blocks ≈ 6mo)
 * @param owner_label        Optional owner label
 * @param guardian_labels    Optional guardian labels
 * @return                   Social recovery pattern
 */
SocialRecoveryPattern createSocialRecovery(
    const std::vector<uint8_t>& owner_pubkey,
    const std::vector<std::vector<uint8_t>>& guardian_pubkeys,
    size_t threshold,
    uint32_t delay_blocks = 25920,
    const std::string& owner_label = "owner",
    const std::vector<std::string>& guardian_labels = {}
);

// ============================================================================
// MULTISIG COVENANT PATTERNS
// ============================================================================

/**
 * Restricted Multisig Pattern
 *
 * M-of-N multisig that can ONLY send to whitelisted addresses
 *
 * Combination:
 * - Multisig: Requires M-of-N signatures
 * - CTV: Restricts outputs to whitelist
 *
 * Security Model:
 * - Prevents insider theft to unauthorized addresses
 * - Even if M signers collude, cannot send to attacker address
 * - Whitelist is pre-committed (requires new covenant to change)
 *
 * Use Cases:
 * - Corporate treasury (CFO + CEO can only send to payroll, vendors)
 * - Exchange cold storage (operators can only send to hot wallet)
 * - DAO treasury (multisig limited to approved grants)
 */
struct RestrictedMultisigPattern {
    std::vector<std::vector<uint8_t>> pubkeys;      // Multisig participants
    size_t threshold;                               // M (required signatures)
    std::vector<CTVOutput> whitelist;               // Allowed destinations
    std::vector<uint8_t> spending_script;           // Complete script

    // Metadata
    std::vector<std::string> signer_labels;         // Who are the signers
    std::vector<std::string> whitelist_labels;      // What are the destinations

    RestrictedMultisigPattern() : threshold(0) {}
};

/**
 * Create restricted multisig pattern
 *
 * @param pubkeys           List of multisig public keys
 * @param threshold         M (required signatures)
 * @param whitelist         Allowed output templates
 * @param signer_labels     Optional signer labels
 * @param whitelist_labels  Optional destination labels
 * @return                  Restricted multisig pattern
 */
RestrictedMultisigPattern createRestrictedMultisig(
    const std::vector<std::vector<uint8_t>>& pubkeys,
    size_t threshold,
    const std::vector<CTVOutput>& whitelist,
    const std::vector<std::string>& signer_labels = {},
    const std::vector<std::string>& whitelist_labels = {}
);

/**
 * Escrow Covenant Pattern
 *
 * 2-of-2 buyer + seller with time-based refund
 *
 * Paths:
 * 1. Both agree: 2-of-2 can spend anytime
 * 2. Timeout: After delay, buyer gets refund
 *
 * Security Model:
 * - Happy path: mutual agreement (instant)
 * - Dispute: buyer protected by timeout refund
 * - Seller protected: must deliver before timeout
 *
 * Use Cases:
 * - Trustless escrow
 * - Purchase protection
 * - Service payment with guarantees
 */
struct EscrowCovenantPattern {
    std::vector<uint8_t> buyer_pubkey;      // Buyer (gets refund on timeout)
    std::vector<uint8_t> seller_pubkey;     // Seller (gets paid on agreement)
    uint32_t timeout_blocks;                // Refund timeout
    CTVOutput mutual_release;               // Where funds go if both agree
    CTVOutput refund_output;                // Where buyer gets refund
    std::vector<uint8_t> spending_script;   // Complete script

    // Metadata
    std::string buyer_label;
    std::string seller_label;
    std::string purpose;                    // What is being escrowed

    EscrowCovenantPattern() : timeout_blocks(2016) {}  // ~2 weeks default
};

/**
 * Create escrow covenant pattern
 *
 * @param buyer_pubkey     Buyer's public key
 * @param seller_pubkey    Seller's public key
 * @param amount           Escrow amount in una
 * @param mutual_address   Address if both agree (seller gets paid)
 * @param refund_address   Refund address (buyer address)
 * @param timeout_blocks   Refund timeout (default: 2016 blocks ≈ 2 weeks)
 * @param purpose          Optional description
 * @return                 Escrow covenant pattern
 */
EscrowCovenantPattern createEscrowCovenant(
    const std::vector<uint8_t>& buyer_pubkey,
    const std::vector<uint8_t>& seller_pubkey,
    uint64_t amount,
    const std::string& mutual_address,
    const std::string& refund_address,
    uint32_t timeout_blocks = 2016,
    const std::string& purpose = "escrow"
);

// ============================================================================
// PATTERN UTILITIES
// ============================================================================

/**
 * Estimate fee for pattern spending
 *
 * @param pattern_type  Type of pattern (vault, recovery, etc.)
 * @return              Estimated witness size in vbytes
 */
size_t estimatePatternWitnessSize(const std::string& pattern_type);

/**
 * Validate pattern parameters
 *
 * Checks:
 * - Public keys are 32 bytes
 * - Delays are reasonable (not too short, not overflow)
 * - Thresholds are valid (K <= N)
 * - Addresses are valid Bech32
 *
 * @param pattern       Pattern to validate
 * @return              Empty string if valid, error message otherwise
 */
std::string validatePattern(const SimpleVaultPattern& pattern);
std::string validatePattern(const RecoveryVaultPattern& pattern);
std::string validatePattern(const TimeDelayedRecoveryPattern& pattern);
std::string validatePattern(const SocialRecoveryPattern& pattern);
std::string validatePattern(const RestrictedMultisigPattern& pattern);
std::string validatePattern(const EscrowCovenantPattern& pattern);

} // namespace patterns
} // namespace wallet
} // namespace dinero
