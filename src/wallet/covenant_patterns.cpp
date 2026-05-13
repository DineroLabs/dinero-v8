/**
 * Phase C.5: Covenant Patterns - Implementation
 *
 * High-level wallet recipes using covenant primitives
 */

#include "wallet/covenant_patterns.h"
#include "wallet/covenant_builders.h"
#include "crypto/sha256.h"
#include "external/bech32/bech32.hpp"
#include <stdexcept>
#include <algorithm>

namespace dinero {
namespace wallet {
namespace patterns {

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * Decode Bech32 address to scriptPubKey
 */
static std::vector<uint8_t> decodeAddress(const std::string& address) {
    auto decoded = bech32::decode(address);
    if (decoded.hrp.empty()) {
        throw std::runtime_error("Invalid Bech32 address: " + address);
    }

    // Convert 5-bit to 8-bit
    std::vector<uint8_t> data;
    if (!bech32::convertbits(data, decoded.dp, 5, 8, false)) {
        throw std::runtime_error("Failed to convert address data");
    }

    // Construct scriptPubKey: OP_0 + data
    std::vector<uint8_t> scriptPubKey;
    scriptPubKey.push_back(0x00);  // OP_0 (witness v0)
    scriptPubKey.push_back(static_cast<uint8_t>(data.size()));
    scriptPubKey.insert(scriptPubKey.end(), data.begin(), data.end());

    return scriptPubKey;
}

/**
 * Create OP_CHECKSEQUENCEVERIFY script fragment
 */
static std::vector<uint8_t> createCSVScript(uint32_t delay_blocks) {
    std::vector<uint8_t> script;

    // Push delay as CScriptNum
    if (delay_blocks <= 16) {
        script.push_back(0x50 + static_cast<uint8_t>(delay_blocks));  // OP_1 through OP_16
    } else if (delay_blocks <= 0x7f) {
        script.push_back(0x01);  // PUSHBYTES_1
        script.push_back(static_cast<uint8_t>(delay_blocks));
    } else if (delay_blocks <= 0x7fff) {
        script.push_back(0x02);  // PUSHBYTES_2
        script.push_back(static_cast<uint8_t>(delay_blocks & 0xff));
        script.push_back(static_cast<uint8_t>((delay_blocks >> 8) & 0xff));
    } else {
        script.push_back(0x03);  // PUSHBYTES_3
        script.push_back(static_cast<uint8_t>(delay_blocks & 0xff));
        script.push_back(static_cast<uint8_t>((delay_blocks >> 8) & 0xff));
        script.push_back(static_cast<uint8_t>((delay_blocks >> 16) & 0xff));
    }

    script.push_back(0xb2);  // OP_CHECKSEQUENCEVERIFY
    script.push_back(0x75);  // OP_DROP

    return script;
}

// ============================================================================
// VAULT PATTERNS
// ============================================================================

SimpleVaultPattern createSimpleVault(
    const std::string& final_address,
    uint64_t amount,
    uint32_t delay_blocks,
    const std::string& label
) {
    SimpleVaultPattern pattern;
    pattern.unvault_delay_blocks = delay_blocks;
    pattern.label = label;

    // Step 1: Create final output
    pattern.final_output.value = amount;
    pattern.final_output.address = final_address;
    pattern.final_output.scriptPubKey = decodeAddress(final_address);

    // Step 2: Build CTV template for unvault → final
    // The unvault transaction will spend to final_output
    std::vector<CTVOutput> unvault_outputs;
    unvault_outputs.push_back(pattern.final_output);

    auto ctv_template = buildCTVTemplate(unvault_outputs, 0, 2);
    pattern.vault_hash = ctv_template.template_hash;

    // Step 3: Create vault script (CTV-locked)
    pattern.vault_script = createCTVScript(pattern.vault_hash, false);

    // Step 4: Create unvault script (time-delayed)
    // Format: <delay> OP_CSV OP_DROP <pubkey> OP_CHECKSIG
    // For simplicity, use OP_TRUE for now (real impl would use pubkey)
    pattern.unvault_script = createCSVScript(delay_blocks);
    pattern.unvault_script.push_back(0x51);  // OP_TRUE (anyone can spend after delay)

    return pattern;
}

RecoveryVaultPattern createRecoveryVault(
    const std::string& final_address,
    uint64_t amount,
    const std::vector<uint8_t>& recovery_pubkey,
    uint32_t vault_delay,
    uint32_t recovery_delay,
    const std::string& label
) {
    RecoveryVaultPattern pattern;
    pattern.recovery_pubkey = recovery_pubkey;
    pattern.recovery_delay_blocks = recovery_delay;
    pattern.recovery_label = label + "-recovery";

    // Create normal vault path
    pattern.vault = createSimpleVault(final_address, amount, vault_delay, label);

    // Create recovery script
    // Format: <recovery_delay> OP_CSV OP_DROP <recovery_pubkey> OP_CHECKSIG
    pattern.recovery_script = createCSVScript(recovery_delay);
    pattern.recovery_script.push_back(0x20);  // PUSHBYTES_32
    pattern.recovery_script.insert(
        pattern.recovery_script.end(),
        recovery_pubkey.begin(),
        recovery_pubkey.end()
    );
    pattern.recovery_script.push_back(0xac);  // OP_CHECKSIG

    return pattern;
}

// ============================================================================
// RECOVERY FLOW PATTERNS
// ============================================================================

TimeDelayedRecoveryPattern createTimeDelayedRecovery(
    const std::vector<uint8_t>& owner_pubkey,
    const std::vector<uint8_t>& recovery_pubkey,
    uint32_t delay_blocks,
    const std::string& owner_label,
    const std::string& recovery_label
) {
    if (owner_pubkey.size() != 32) {
        throw std::runtime_error("Owner pubkey must be 32 bytes (x-only)");
    }
    if (recovery_pubkey.size() != 32) {
        throw std::runtime_error("Recovery pubkey must be 32 bytes (x-only)");
    }

    TimeDelayedRecoveryPattern pattern;
    pattern.owner_pubkey = owner_pubkey;
    pattern.recovery_pubkey = recovery_pubkey;
    pattern.recovery_delay_blocks = delay_blocks;
    pattern.owner_label = owner_label;
    pattern.recovery_label = recovery_label;

    // Build script:
    // OP_IF
    //     <owner_pubkey> OP_CHECKSIG
    // OP_ELSE
    //     <delay> OP_CSV OP_DROP
    //     <recovery_pubkey> OP_CHECKSIG
    // OP_ENDIF

    std::vector<uint8_t> script;

    // OP_IF
    script.push_back(0x63);

    // Owner path: <owner_pubkey> OP_CHECKSIG
    script.push_back(0x20);  // PUSHBYTES_32
    script.insert(script.end(), owner_pubkey.begin(), owner_pubkey.end());
    script.push_back(0xac);  // OP_CHECKSIG

    // OP_ELSE
    script.push_back(0x67);

    // Recovery path: <delay> OP_CSV OP_DROP <recovery_pubkey> OP_CHECKSIG
    auto csv_fragment = createCSVScript(delay_blocks);
    script.insert(script.end(), csv_fragment.begin(), csv_fragment.end());

    script.push_back(0x20);  // PUSHBYTES_32
    script.insert(script.end(), recovery_pubkey.begin(), recovery_pubkey.end());
    script.push_back(0xac);  // OP_CHECKSIG

    // OP_ENDIF
    script.push_back(0x68);

    pattern.spending_script = script;
    return pattern;
}

SocialRecoveryPattern createSocialRecovery(
    const std::vector<uint8_t>& owner_pubkey,
    const std::vector<std::vector<uint8_t>>& guardian_pubkeys,
    size_t threshold,
    uint32_t delay_blocks,
    const std::string& owner_label,
    const std::vector<std::string>& guardian_labels
) {
    if (owner_pubkey.size() != 32) {
        throw std::runtime_error("Owner pubkey must be 32 bytes (x-only)");
    }
    if (threshold == 0 || threshold > guardian_pubkeys.size()) {
        throw std::runtime_error("Invalid threshold: must be 1 <= K <= N");
    }
    for (const auto& pubkey : guardian_pubkeys) {
        if (pubkey.size() != 32) {
            throw std::runtime_error("All guardian pubkeys must be 32 bytes (x-only)");
        }
    }

    SocialRecoveryPattern pattern;
    pattern.owner_pubkey = owner_pubkey;
    pattern.guardian_pubkeys = guardian_pubkeys;
    pattern.threshold = threshold;
    pattern.recovery_delay_blocks = delay_blocks;
    pattern.owner_label = owner_label;
    pattern.guardian_labels = guardian_labels;

    // Build script:
    // OP_IF
    //     <owner_pubkey> OP_CHECKSIG
    // OP_ELSE
    //     <delay> OP_CSV OP_DROP
    //     <guardian_1_pubkey> OP_CHECKSIG
    //     <guardian_2_pubkey> OP_CHECKSIGADD
    //     <guardian_3_pubkey> OP_CHECKSIGADD
    //     ...
    //     <threshold> OP_GREATERTHANOREQUAL
    // OP_ENDIF

    std::vector<uint8_t> script;

    // OP_IF
    script.push_back(0x63);

    // Owner path: <owner_pubkey> OP_CHECKSIG
    script.push_back(0x20);  // PUSHBYTES_32
    script.insert(script.end(), owner_pubkey.begin(), owner_pubkey.end());
    script.push_back(0xac);  // OP_CHECKSIG

    // OP_ELSE
    script.push_back(0x67);

    // Recovery path: <delay> OP_CSV OP_DROP
    auto csv_fragment = createCSVScript(delay_blocks);
    script.insert(script.end(), csv_fragment.begin(), csv_fragment.end());

    // First guardian: <pubkey> OP_CHECKSIG (starts count)
    script.push_back(0x20);  // PUSHBYTES_32
    script.insert(script.end(), guardian_pubkeys[0].begin(), guardian_pubkeys[0].end());
    script.push_back(0xac);  // OP_CHECKSIG

    // Remaining guardians: <pubkey> OP_CHECKSIGADD (accumulates count)
    for (size_t i = 1; i < guardian_pubkeys.size(); i++) {
        script.push_back(0x20);  // PUSHBYTES_32
        script.insert(script.end(), guardian_pubkeys[i].begin(), guardian_pubkeys[i].end());
        script.push_back(0xba);  // OP_CHECKSIGADD (Tapscript opcode)
    }

    // <threshold> OP_GREATERTHANOREQUAL
    if (threshold <= 16) {
        script.push_back(0x50 + static_cast<uint8_t>(threshold));  // OP_1 through OP_16
    } else {
        script.push_back(0x01);  // PUSHBYTES_1
        script.push_back(static_cast<uint8_t>(threshold));
    }
    script.push_back(0xa2);  // OP_GREATERTHANOREQUAL

    // OP_ENDIF
    script.push_back(0x68);

    pattern.spending_script = script;
    return pattern;
}

// ============================================================================
// MULTISIG COVENANT PATTERNS
// ============================================================================

RestrictedMultisigPattern createRestrictedMultisig(
    const std::vector<std::vector<uint8_t>>& pubkeys,
    size_t threshold,
    const std::vector<CTVOutput>& whitelist,
    const std::vector<std::string>& signer_labels,
    const std::vector<std::string>& whitelist_labels
) {
    if (threshold == 0 || threshold > pubkeys.size()) {
        throw std::runtime_error("Invalid threshold: must be 1 <= M <= N");
    }
    for (const auto& pubkey : pubkeys) {
        if (pubkey.size() != 32) {
            throw std::runtime_error("All pubkeys must be 32 bytes (x-only)");
        }
    }
    if (whitelist.empty()) {
        throw std::runtime_error("Whitelist cannot be empty");
    }

    RestrictedMultisigPattern pattern;
    pattern.pubkeys = pubkeys;
    pattern.threshold = threshold;
    pattern.whitelist = whitelist;
    pattern.signer_labels = signer_labels;
    pattern.whitelist_labels = whitelist_labels;

    // Build CTV template committing to whitelist
    auto ctv_template = buildCTVTemplate(whitelist, 0, 2);

    // Build multisig script with CTV restriction
    // Format: <M> <pubkey1> <pubkey2> ... <pubkeyN> <N> OP_CHECKMULTISIG
    //         <ctv_hash> OP_CHECKTEMPLATEVERIFY
    std::vector<uint8_t> script;

    // Threshold M
    if (threshold <= 16) {
        script.push_back(0x50 + static_cast<uint8_t>(threshold));
    } else {
        script.push_back(0x01);
        script.push_back(static_cast<uint8_t>(threshold));
    }

    // Pubkeys
    for (const auto& pubkey : pubkeys) {
        script.push_back(0x20);  // PUSHBYTES_32
        script.insert(script.end(), pubkey.begin(), pubkey.end());
    }

    // N (number of pubkeys)
    size_t n = pubkeys.size();
    if (n <= 16) {
        script.push_back(0x50 + static_cast<uint8_t>(n));
    } else {
        script.push_back(0x01);
        script.push_back(static_cast<uint8_t>(n));
    }

    // OP_CHECKMULTISIG
    script.push_back(0xae);

    // CTV restriction
    script.push_back(0x20);  // PUSHBYTES_32
    script.insert(script.end(), ctv_template.template_hash.begin(), ctv_template.template_hash.end());
    script.push_back(0xb3);  // OP_CHECKTEMPLATEVERIFY

    pattern.spending_script = script;
    return pattern;
}

EscrowCovenantPattern createEscrowCovenant(
    const std::vector<uint8_t>& buyer_pubkey,
    const std::vector<uint8_t>& seller_pubkey,
    uint64_t amount,
    const std::string& mutual_address,
    const std::string& refund_address,
    uint32_t timeout_blocks,
    const std::string& purpose
) {
    if (buyer_pubkey.size() != 32) {
        throw std::runtime_error("Buyer pubkey must be 32 bytes (x-only)");
    }
    if (seller_pubkey.size() != 32) {
        throw std::runtime_error("Seller pubkey must be 32 bytes (x-only)");
    }

    EscrowCovenantPattern pattern;
    pattern.buyer_pubkey = buyer_pubkey;
    pattern.seller_pubkey = seller_pubkey;
    pattern.timeout_blocks = timeout_blocks;
    pattern.purpose = purpose;

    // Create outputs
    pattern.mutual_release.value = amount;
    pattern.mutual_release.address = mutual_address;
    pattern.mutual_release.scriptPubKey = decodeAddress(mutual_address);

    pattern.refund_output.value = amount;
    pattern.refund_output.address = refund_address;
    pattern.refund_output.scriptPubKey = decodeAddress(refund_address);

    // Build script:
    // OP_IF
    //     <buyer_pubkey> OP_CHECKSIGVERIFY
    //     <seller_pubkey> OP_CHECKSIG
    // OP_ELSE
    //     <timeout> OP_CSV OP_DROP
    //     <buyer_pubkey> OP_CHECKSIG
    // OP_ENDIF

    std::vector<uint8_t> script;

    // OP_IF
    script.push_back(0x63);

    // Mutual release path: 2-of-2
    script.push_back(0x20);  // PUSHBYTES_32
    script.insert(script.end(), buyer_pubkey.begin(), buyer_pubkey.end());
    script.push_back(0xad);  // OP_CHECKSIGVERIFY

    script.push_back(0x20);  // PUSHBYTES_32
    script.insert(script.end(), seller_pubkey.begin(), seller_pubkey.end());
    script.push_back(0xac);  // OP_CHECKSIG

    // OP_ELSE
    script.push_back(0x67);

    // Refund path: timeout + buyer signature
    auto csv_fragment = createCSVScript(timeout_blocks);
    script.insert(script.end(), csv_fragment.begin(), csv_fragment.end());

    script.push_back(0x20);  // PUSHBYTES_32
    script.insert(script.end(), buyer_pubkey.begin(), buyer_pubkey.end());
    script.push_back(0xac);  // OP_CHECKSIG

    // OP_ENDIF
    script.push_back(0x68);

    pattern.spending_script = script;
    return pattern;
}

// ============================================================================
// PATTERN UTILITIES
// ============================================================================

size_t estimatePatternWitnessSize(const std::string& pattern_type) {
    if (pattern_type == "simple-vault") {
        return 100;  // CTV witness + signatures
    } else if (pattern_type == "recovery-vault") {
        return 150;  // CTV witness + recovery path
    } else if (pattern_type == "time-delayed-recovery") {
        return 100;  // Signature + script reveal
    } else if (pattern_type == "social-recovery") {
        return 200;  // Multiple signatures + script reveal
    } else if (pattern_type == "restricted-multisig") {
        return 250;  // Multisig sigs + CTV witness
    } else if (pattern_type == "escrow-covenant") {
        return 150;  // 2-of-2 sigs or timeout sig
    }
    return 0;
}

std::string validatePattern(const SimpleVaultPattern& pattern) {
    if (pattern.vault_script.empty()) {
        return "Vault script is empty";
    }
    if (pattern.unvault_delay_blocks == 0) {
        return "Unvault delay must be positive";
    }
    if (pattern.unvault_delay_blocks > 65535) {
        return "Unvault delay too large (max: 65535 blocks)";
    }
    if (pattern.final_output.value == 0) {
        return "Final output value must be positive";
    }
    if (pattern.final_output.scriptPubKey.empty()) {
        return "Final output scriptPubKey is empty";
    }
    return "";  // Valid
}

std::string validatePattern(const RecoveryVaultPattern& pattern) {
    auto vault_error = validatePattern(pattern.vault);
    if (!vault_error.empty()) {
        return "Vault validation failed: " + vault_error;
    }
    if (pattern.recovery_pubkey.size() != 32) {
        return "Recovery pubkey must be 32 bytes";
    }
    if (pattern.recovery_delay_blocks == 0) {
        return "Recovery delay must be positive";
    }
    if (pattern.recovery_delay_blocks <= pattern.vault.unvault_delay_blocks) {
        return "Recovery delay should be longer than vault delay";
    }
    return "";  // Valid
}

std::string validatePattern(const TimeDelayedRecoveryPattern& pattern) {
    if (pattern.owner_pubkey.size() != 32) {
        return "Owner pubkey must be 32 bytes";
    }
    if (pattern.recovery_pubkey.size() != 32) {
        return "Recovery pubkey must be 32 bytes";
    }
    if (pattern.recovery_delay_blocks == 0) {
        return "Recovery delay must be positive";
    }
    if (pattern.spending_script.empty()) {
        return "Spending script is empty";
    }
    return "";  // Valid
}

std::string validatePattern(const SocialRecoveryPattern& pattern) {
    if (pattern.owner_pubkey.size() != 32) {
        return "Owner pubkey must be 32 bytes";
    }
    if (pattern.guardian_pubkeys.empty()) {
        return "Must have at least one guardian";
    }
    if (pattern.threshold == 0 || pattern.threshold > pattern.guardian_pubkeys.size()) {
        return "Invalid threshold: must be 1 <= K <= N";
    }
    for (const auto& pubkey : pattern.guardian_pubkeys) {
        if (pubkey.size() != 32) {
            return "All guardian pubkeys must be 32 bytes";
        }
    }
    if (pattern.recovery_delay_blocks == 0) {
        return "Recovery delay must be positive";
    }
    if (pattern.spending_script.empty()) {
        return "Spending script is empty";
    }
    return "";  // Valid
}

std::string validatePattern(const RestrictedMultisigPattern& pattern) {
    if (pattern.pubkeys.empty()) {
        return "Must have at least one pubkey";
    }
    if (pattern.threshold == 0 || pattern.threshold > pattern.pubkeys.size()) {
        return "Invalid threshold: must be 1 <= M <= N";
    }
    for (const auto& pubkey : pattern.pubkeys) {
        if (pubkey.size() != 32) {
            return "All pubkeys must be 32 bytes";
        }
    }
    if (pattern.whitelist.empty()) {
        return "Whitelist cannot be empty";
    }
    if (pattern.spending_script.empty()) {
        return "Spending script is empty";
    }
    return "";  // Valid
}

std::string validatePattern(const EscrowCovenantPattern& pattern) {
    if (pattern.buyer_pubkey.size() != 32) {
        return "Buyer pubkey must be 32 bytes";
    }
    if (pattern.seller_pubkey.size() != 32) {
        return "Seller pubkey must be 32 bytes";
    }
    if (pattern.timeout_blocks == 0) {
        return "Timeout must be positive";
    }
    if (pattern.mutual_release.value == 0) {
        return "Mutual release value must be positive";
    }
    if (pattern.refund_output.value == 0) {
        return "Refund output value must be positive";
    }
    if (pattern.spending_script.empty()) {
        return "Spending script is empty";
    }
    return "";  // Valid
}

} // namespace patterns
} // namespace wallet
} // namespace dinero
