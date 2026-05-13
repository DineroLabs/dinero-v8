#include "wallet/tx_classifier.h"
#include "wallet/policy_descriptor.h"
#include <cstring>

namespace dinero {

UTXOClassification TxClassifier::Classify(const CanonicalWalletUTXO& utxo) {
    if (utxo.policy_id.empty()) {
        return UTXOClassification::STANDARD;
    }

    if (utxo.policy_id.size() != 32) {
        return UTXOClassification::UNKNOWN_POLICY;
    }

    // TODO: Look up policy_id in wallet_policies DB to get template_type
    // For now, return UNKNOWN_POLICY for non-empty policy IDs
    // (will be resolved when DB queries are wired)
    return UTXOClassification::UNKNOWN_POLICY;
}

ExtendedTxDetail TxClassifier::GetExtendedDetail(
    const CanonicalWalletUTXO& utxo,
    uint32_t current_height) {

    ExtendedTxDetail detail;
    detail.classification = Classify(utxo);

    switch (detail.classification) {
        case UTXOClassification::STANDARD:
            detail.template_name = "Standard";
            detail.policy_description = "Key-path only (BIP86)";
            break;

        case UTXOClassification::PROTECTED: {
            detail.template_name = "Protected";
            detail.policy_description = "Protected with panic + recovery";

            if (!utxo.policy_id.empty()) {
                std::array<uint8_t, 32> pid;
                std::copy(utxo.policy_id.begin(), utxo.policy_id.end(), pid.begin());
                detail.policy_id = pid;
            }

            // Compute remaining timelocks
            // TODO: Look up panic_window_blocks and recovery_delay_blocks from policy params
            // For now, leave as nullopt until DB is wired
            break;
        }

        case UTXOClassification::ESCROW: {
            detail.template_name = "Escrow";
            detail.policy_description = "Escrow with attestor release + timeout refund";

            if (!utxo.policy_id.empty()) {
                std::array<uint8_t, 32> pid;
                std::copy(utxo.policy_id.begin(), utxo.policy_id.end(), pid.begin());
                detail.policy_id = pid;
            }

            // TODO: Look up escrow state from escrow_sessions table
            break;
        }

        case UTXOClassification::VAULT:
            detail.template_name = "Simple Lock";
            detail.policy_description = "CTV lock pattern";
            break;

        case UTXOClassification::UNKNOWN_POLICY:
            detail.template_name = "Unknown";
            detail.policy_description = "Unrecognized policy template";
            if (!utxo.policy_id.empty()) {
                std::array<uint8_t, 32> pid;
                std::copy(utxo.policy_id.begin(), utxo.policy_id.end(), pid.begin());
                detail.policy_id = pid;
            }
            break;
    }

    return detail;
}

UTXOClassification TxClassifier::ClassifyByScript(
    const std::vector<uint8_t>& scriptPubKey) {

    // All Taproot outputs are 34 bytes: OP_1 (0x51) + push32 (0x20) + 32-byte key
    if (scriptPubKey.size() != 34 ||
        scriptPubKey[0] != 0x51 ||
        scriptPubKey[1] != 0x20) {
        return UTXOClassification::STANDARD;  // Not even Taproot
    }

    // It's a Taproot output — but without the policy DB, we can't distinguish
    // STANDARD (key-path only) from PROTECTED/ESCROW (with script tree).
    //
    // Heuristic: all Taproot outputs without DB context are classified as STANDARD.
    // During mnemonic-only recovery, the wallet can try to reconstruct known
    // template trees using the recovered keys and match against the output key.
    // That reconstruction logic will be added when the full recovery flow is wired.
    return UTXOClassification::STANDARD;
}

} // namespace dinero
