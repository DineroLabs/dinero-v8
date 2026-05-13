/**
 * Phase C.3: Covenant Construction Helpers
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * CRITICAL BOUNDARY RULE
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * These helpers CONSTRUCT covenant transactions.
 * They NEVER validate covenant rules.
 *
 * Validation happens EXCLUSIVELY in consensus::ScriptInterpreter.
 *
 * Allowed operations:
 *   ✅ Compute hashes for template building (ComputeCTVHash)
 *   ✅ Create covenant scripts (opcodes in scriptPubKey)
 *   ✅ Assemble transactions (build tx matching template)
 *   ✅ Estimate fees (witness size calculation)
 *
 * Forbidden operations:
 *   ❌ Verify covenant validity (VerifyCTV, VerifySignatureFromStack)
 *   ❌ Check template matches (that's consensus validation)
 *   ❌ Validate signatures (that's consensus validation)
 *   ❌ Return "valid/invalid" based on covenant checks
 *
 * Rationale: Construction ≠ Validation
 * - Wallet builds transactions
 * - Consensus validates transactions
 * - Single source of truth for validation
 *
 * ═══════════════════════════════════════════════════════════════════════════
 */

#pragma once

#include "wallet/covenant_wallet.h"
#include "primitives/transaction.h"
#include "primitives/uint256.h"
#include "wallet/canonical_wallet_utxo.h"
#include <vector>
#include <array>
#include <string>
#include <optional>

namespace dinero {
namespace wallet {

// ============================================================================
// CTV (CheckTemplateVerify) Construction Helpers
// ============================================================================

/**
 * Output for CTV template
 */
struct CTVOutput {
    uint64_t value;                     // Output value in una
    std::vector<uint8_t> scriptPubKey;  // Output script
    std::string address;                // Human-readable address (optional)

    CTVOutput() : value(0) {}
    CTVOutput(uint64_t v, const std::vector<uint8_t>& spk, const std::string& addr = "")
        : value(v), scriptPubKey(spk), address(addr) {}
};

/**
 * CTV Template Builder Result
 */
struct CTVTemplateBuilder {
    std::array<uint8_t, 32> template_hash;      // BIP-119 template hash
    std::vector<CTVOutput> outputs;              // Committed outputs
    uint32_t locktime;                           // Transaction locktime
    int32_t version;                             // Transaction version

    CTVTemplateBuilder() : locktime(0), version(2) {}
};

/**
 * Build a CTV template from desired outputs
 *
 * Phase C.3: CONSTRUCTION ONLY - does not validate
 * Computes the BIP-119 template hash that commits to the outputs
 *
 * @param outputs   List of outputs to commit to
 * @param locktime  Transaction locktime (default: 0)
 * @param version   Transaction version (default: 2)
 * @return          Template with computed hash
 */
CTVTemplateBuilder buildCTVTemplate(
    const std::vector<CTVOutput>& outputs,
    uint32_t locktime = 0,
    int32_t version = 2
);

/**
 * Create a scriptPubKey that locks funds with CTV
 *
 * Phase C.3: Script construction for Taproot (BIP 341)
 * Format: <internal_pubkey> (reveal: <template_hash> OP_CHECKTEMPLATEVERIFY)
 *
 * For testing/simple cases: P2WSH with direct CTV
 * Format: OP_SHA256 <sha256(witness_script)> OP_EQUAL
 * Witness script: <template_hash> OP_CHECKTEMPLATEVERIFY
 *
 * @param template_hash  32-byte CTV template hash
 * @param use_taproot    Use Taproot (true) or P2WSH (false) - default: false for testing
 * @return               ScriptPubKey for the covenant output
 */
std::vector<uint8_t> createCTVScript(
    const std::array<uint8_t, 32>& template_hash,
    bool use_taproot = false  // Default false for Phase C.3 testing
);

/**
 * Build a transaction that SPENDS a CTV output
 *
 * Phase C.3: CONSTRUCTION - builds the spending tx matching template
 * Does NOT validate - consensus will check the match during script execution
 *
 * @param ctv_template  The CTV template to satisfy
 * @param funding_utxo  The CTV-locked UTXO to spend
 * @param input_index   Which input in the spending tx (default: 0)
 * @return              Transaction ready for signing (matches template)
 */
Transaction buildCTVSpendingTx(
    const CTVTemplateBuilder& ctv_template,
    const CanonicalWalletUTXO& funding_utxo,
    uint32_t input_index = 0
);

// ============================================================================
// CSFS (CheckSigFromStack) Construction Helpers
// ============================================================================

/**
 * CSFS Delegation Builder Result
 */
struct CSFSDelegationBuilder {
    std::vector<uint8_t> pubkey;     // 32-byte x-only Schnorr pubkey
    std::vector<uint8_t> message;    // Arbitrary message
    std::vector<uint8_t> signature;  // 64-byte Schnorr signature (if signed)
    std::string purpose;             // Human-readable purpose
    bool is_signed;                  // Has signature been added?

    CSFSDelegationBuilder()
        : is_signed(false) {}
};

/**
 * Create a CSFS delegation (unsigned)
 *
 * Phase C.3: CONSTRUCTION - prepares message for signing
 * Does NOT verify signatures (that's consensus)
 *
 * @param pubkey   32-byte x-only Schnorr pubkey
 * @param message  Arbitrary message to be signed
 * @param purpose  Human-readable purpose (default: "delegation")
 * @return         CSFS delegation (unsigned)
 */
CSFSDelegationBuilder createCSFSDelegation(
    const std::vector<uint8_t>& pubkey,
    const std::vector<uint8_t>& message,
    const std::string& purpose = "delegation"
);

/**
 * Sign a CSFS delegation with a private key
 *
 * Phase C.3: SIGNING - creates Schnorr signature over message
 * Does NOT verify the signature (consensus does that)
 *
 * Uses libsecp256k1 for Schnorr signing (BIP 340)
 *
 * @param delegation  Unsigned delegation
 * @param privkey     32-byte private key
 * @return            Signed delegation
 */
CSFSDelegationBuilder signCSFSDelegation(
    const CSFSDelegationBuilder& delegation,
    const std::vector<uint8_t>& privkey
);

/**
 * Create a Tapscript with CSFS constraint
 *
 * Phase C.3: Script construction
 * Format: <pubkey> <message> OP_CHECKSIGFROMSTACKVERIFY <continuation_script>
 *
 * @param pubkey              32-byte x-only pubkey
 * @param message             Message that must be signed
 * @param continuation_script Script to execute after CSFS check (default: OP_TRUE)
 * @return                    Complete Tapscript with CSFS
 */
std::vector<uint8_t> createCSFSScript(
    const std::vector<uint8_t>& pubkey,
    const std::vector<uint8_t>& message,
    const std::vector<uint8_t>& continuation_script = {}
);

// ============================================================================
// General Transaction Assembly
// ============================================================================

/**
 * Covenant output specification
 */
struct CovenantOutput {
    CovenantType type;              // CTV, CSFS, etc.
    uint64_t value;                 // Output value
    std::vector<uint8_t> script;    // Covenant scriptPubKey
    std::string label;              // Human-readable label

    CovenantOutput() : type(CovenantType::NONE), value(0) {}
};

/**
 * Estimate witness size for covenant spending
 *
 * Phase C.3: Fee estimation helper
 * Used for calculating fees when spending covenant UTXOs
 *
 * @param covenant_type  Type of covenant (CTV, CSFS, etc.)
 * @param template_size  Size of template/message (default: 32 bytes)
 * @return               Estimated witness size in vbytes
 */
size_t estimateCovenantWitnessSize(
    CovenantType covenant_type,
    size_t template_size = 32
);

} // namespace wallet
} // namespace dinero
