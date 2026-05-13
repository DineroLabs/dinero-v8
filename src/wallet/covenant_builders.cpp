/**
 * Phase C.3: Covenant Construction Helpers - Implementation
 *
 * BOUNDARY ENFORCEMENT:
 * - All consensus function calls marked with "CONSTRUCTION ONLY"
 * - No validation logic (that's consensus-only)
 * - Builds transactions, never checks validity
 */

#include "wallet/covenant_builders.h"
#include "consensus/covenants.h"
#include "consensus/script.h"
#include "crypto/sha256.h"
#include <secp256k1.h>
#include <secp256k1_schnorrsig.h>
#include <cstring>

namespace dinero {
namespace wallet {

// ============================================================================
// CTV Template Builder
// ============================================================================

CTVTemplateBuilder buildCTVTemplate(
    const std::vector<CTVOutput>& outputs,
    uint32_t locktime,
    int32_t version
) {
    CTVTemplateBuilder result;
    result.outputs = outputs;
    result.locktime = locktime;
    result.version = version;

    // Phase C.3: CONSTRUCTION ONLY - building template tx to compute hash
    // NOT validation - consensus will validate during script execution
    //
    // We build a dummy transaction matching the template structure
    // and compute its CTV hash using consensus helper

    Transaction template_tx;
    template_tx.version = version;
    template_tx.lockTime = locktime;

    // CTV doesn't commit to inputs (only outputs, version, locktime)
    // Add a dummy input with sequence 0xfffffffe (standard)
    TxInput dummy_input;
    dummy_input.prevout.txid = uint256();  // All zeros
    dummy_input.prevout.vout = 0;
    dummy_input.sequence = 0xfffffffe;
    template_tx.vin.push_back(dummy_input);

    // Add outputs from template
    for (const auto& output : outputs) {
        TxOutput tx_output;
        tx_output.value = output.value;
        tx_output.scriptPubKey = output.scriptPubKey;
        template_tx.vout.push_back(tx_output);
    }

    // Phase C.3: CONSTRUCTION ONLY - computing hash for template building
    // This is ALLOWED - we're building a template, not validating
    // Consensus will validate when the spending tx is executed
    result.template_hash = consensus::ComputeCTVHash(template_tx, 0);

    return result;
}

// ============================================================================
// CTV Script Creation
// ============================================================================

std::vector<uint8_t> createCTVScript(
    const std::array<uint8_t, 32>& template_hash,
    bool use_taproot
) {
    std::vector<uint8_t> script;

    if (use_taproot) {
        // Phase C.3: Taproot CTV (deferred - complex)
        // For now, fall back to P2WSH for testing
        // TODO: Implement Taproot CTV in future phase
        use_taproot = false;
    }

    if (!use_taproot) {
        // P2WSH with CTV witness script
        // Format: OP_SHA256 <sha256(witness_script)> OP_EQUAL
        //
        // Witness script: <template_hash> OP_CHECKTEMPLATEVERIFY
        //
        // When spending:
        // - Witness stack: <witness_script>
        // - Script execution: SHA256(witness_script) == committed_hash

        // Build the witness script
        std::vector<uint8_t> witness_script;
        witness_script.push_back(0x20);  // OP_PUSHBYTES_32
        witness_script.insert(witness_script.end(),
                            template_hash.begin(),
                            template_hash.end());
        witness_script.push_back(0xb3);  // OP_CHECKTEMPLATEVERIFY

        // Hash the witness script
        std::vector<uint8_t> witness_script_hash(32);
        SHA256(witness_script.data(), witness_script.size(),
               witness_script_hash.data());

        // Create P2WSH scriptPubKey
        script.push_back(0x00);  // OP_0 (witness v0)
        script.push_back(0x20);  // Push 32 bytes
        script.insert(script.end(),
                     witness_script_hash.begin(),
                     witness_script_hash.end());
    }

    return script;
}

// ============================================================================
// CTV Spending Transaction Builder
// ============================================================================

Transaction buildCTVSpendingTx(
    const CTVTemplateBuilder& ctv_template,
    const CanonicalWalletUTXO& funding_utxo,
    uint32_t input_index
) {
    // Phase C.3: CONSTRUCTION - build tx matching template
    // Does NOT validate - consensus checks the match during execution

    Transaction tx;
    tx.version = ctv_template.version;
    tx.lockTime = ctv_template.locktime;

    // Add input spending the CTV-locked UTXO
    TxInput input;
    input.prevout.txid = funding_utxo.txid;
    input.prevout.vout = funding_utxo.vout;
    input.sequence = 0xfffffffe;  // Standard sequence
    input.scriptSig = {};  // Empty for witness

    // Witness will be added during signing:
    // For P2WSH-CTV: witness = [<witness_script>]
    // where witness_script = <template_hash> OP_CHECKTEMPLATEVERIFY
    //
    // Placeholder: empty witness (to be filled by signer)
    input.witness = {};

    tx.vin.push_back(input);

    // Add outputs from template (these MUST match for CTV to pass)
    for (const auto& output : ctv_template.outputs) {
        TxOutput tx_output;
        tx_output.value = output.value;
        tx_output.scriptPubKey = output.scriptPubKey;
        tx.vout.push_back(tx_output);
    }

    return tx;
}

// ============================================================================
// CSFS Delegation Builder
// ============================================================================

CSFSDelegationBuilder createCSFSDelegation(
    const std::vector<uint8_t>& pubkey,
    const std::vector<uint8_t>& message,
    const std::string& purpose
) {
    CSFSDelegationBuilder delegation;
    delegation.pubkey = pubkey;
    delegation.message = message;
    delegation.purpose = purpose;
    delegation.is_signed = false;

    return delegation;
}

// ============================================================================
// CSFS Signing
// ============================================================================

CSFSDelegationBuilder signCSFSDelegation(
    const CSFSDelegationBuilder& delegation,
    const std::vector<uint8_t>& privkey
) {
    // Phase C.3: SIGNING - creates signature over message
    // Does NOT verify - consensus validates signatures

    if (privkey.size() != 32) {
        throw std::runtime_error("Private key must be 32 bytes");
    }

    CSFSDelegationBuilder signed_delegation = delegation;

    // Create secp256k1 context
    secp256k1_context* ctx = secp256k1_context_create(
        SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY
    );

    if (!ctx) {
        throw std::runtime_error("Failed to create secp256k1 context");
    }

    try {
        // Generate keypair from private key
        secp256k1_keypair keypair;
        if (!secp256k1_keypair_create(ctx, &keypair, privkey.data())) {
            throw std::runtime_error("Invalid private key");
        }

        // Create Schnorr signature over message (BIP 340)
        signed_delegation.signature.resize(64);

        // For CSFS, we sign the message directly (no sighash needed)
        int result = secp256k1_schnorrsig_sign32(
            ctx,
            signed_delegation.signature.data(),
            delegation.message.data(),
            &keypair,
            nullptr  // No aux_rand (deterministic signing)
        );

        if (!result) {
            throw std::runtime_error("Schnorr signing failed");
        }

        signed_delegation.is_signed = true;

        secp256k1_context_destroy(ctx);
        return signed_delegation;

    } catch (...) {
        secp256k1_context_destroy(ctx);
        throw;
    }
}

// ============================================================================
// CSFS Script Creation
// ============================================================================

std::vector<uint8_t> createCSFSScript(
    const std::vector<uint8_t>& pubkey,
    const std::vector<uint8_t>& message,
    const std::vector<uint8_t>& continuation_script
) {
    std::vector<uint8_t> script;

    // Format: <pubkey> <message> OP_CHECKSIGFROMSTACKVERIFY <continuation>
    //
    // When spending:
    // - Witness stack: [..., <signature>]
    // - Script pops signature, verifies against pubkey+message
    // - If valid, continues with continuation_script

    // Push pubkey (32 bytes for x-only Schnorr)
    if (pubkey.size() != 32) {
        throw std::runtime_error("CSFS pubkey must be 32 bytes (x-only)");
    }
    script.push_back(0x20);  // OP_PUSHBYTES_32
    script.insert(script.end(), pubkey.begin(), pubkey.end());

    // Push message
    if (message.empty() || message.size() > 520) {
        throw std::runtime_error("CSFS message must be 1-520 bytes");
    }
    if (message.size() <= 75) {
        script.push_back(static_cast<uint8_t>(message.size()));
    } else {
        script.push_back(0x4c);  // OP_PUSHDATA1
        script.push_back(static_cast<uint8_t>(message.size()));
    }
    script.insert(script.end(), message.begin(), message.end());

    // OP_CHECKSIGFROMSTACKVERIFY
    script.push_back(0xbc);  // From script.h:189-190

    // Add continuation script (default: OP_TRUE for simple case)
    if (continuation_script.empty()) {
        script.push_back(0x51);  // OP_TRUE (OP_1)
    } else {
        script.insert(script.end(),
                     continuation_script.begin(),
                     continuation_script.end());
    }

    return script;
}

// ============================================================================
// Fee Estimation
// ============================================================================

size_t estimateCovenantWitnessSize(
    CovenantType covenant_type,
    size_t template_size
) {
    // Phase C.3: Fee estimation helper
    // Estimates witness size for different covenant types

    switch (covenant_type) {
        case CovenantType::CTV: {
            // P2WSH-CTV witness: [<witness_script>]
            // witness_script = <32-byte-hash> OP_CHECKTEMPLATEVERIFY
            // = 1 + 32 + 1 = 34 bytes
            //
            // Witness encoding: [1 item][34-byte item] = ~36 bytes
            return 36;
        }

        case CovenantType::CSFS: {
            // CSFS witness: [<signature>]
            // signature = 64-byte Schnorr (BIP 340)
            //
            // Witness encoding: [1 item][64-byte item] = ~66 bytes
            return 66;
        }

        case CovenantType::TXHASH: {
            // TXHASH witness: similar to CSFS
            // Depends on continuation script
            return 66;
        }

        case CovenantType::CCV: {
            // Contract state transition
            // Depends on state size (variable)
            return 100 + template_size;  // Estimate
        }

        default:
            return 0;  // No covenant
    }
}

} // namespace wallet
} // namespace dinero
