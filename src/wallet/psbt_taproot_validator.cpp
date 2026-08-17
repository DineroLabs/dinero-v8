#include "wallet/psbt_taproot_validator.h"
#include "wallet/psbt_witness_utxo_decode.h"
#include <sstream>

namespace dinero {

namespace {

bool extractWitnessUtxoScript(const std::vector<uint8_t>& value, std::vector<uint8_t>& script_out) {
    const auto decoded = din::psbt::DecodeWitnessUtxoValue(value);
    if (!decoded.ok) return false;
    script_out = decoded.script_pubkey;
    return !script_out.empty();
}

} // namespace

bool PSBTTaprootValidator::isTaprootScriptPubKey(const std::vector<uint8_t>& scriptPubKey) {
    // Taproot scriptPubKey: OP_1 <32-byte-pubkey>
    // Format: 0x51 0x20 <32 bytes>
    return scriptPubKey.size() == 34 &&
           scriptPubKey[0] == 0x51 &&  // OP_1 (witness version 1)
           scriptPubKey[1] == 0x20;    // Push 32 bytes
}

bool PSBTTaprootValidator::hasScriptPath(
    const std::vector<PsbtMapKV>& input_kv
) {
    for (const auto& kv : input_kv) {
        if (kv.key.empty()) continue;

        uint8_t type = kv.key[0];
        const auto& value = kv.value;

        // Check for script-path indicators
        if (type == static_cast<uint8_t>(PSBTTaprootInputType::TAP_SCRIPT_SIG)) {
            // TAP_SCRIPT_SIG: Script-path signature (BIP 342)
            return true;
        }

        if (type == static_cast<uint8_t>(PSBTTaprootInputType::TAP_LEAF_SCRIPT)) {
            // TAP_LEAF_SCRIPT: Tapscript leaf in the tree
            return true;
        }

        if (type == static_cast<uint8_t>(PSBTTaprootInputType::TAP_MERKLE_ROOT)) {
            // TAP_MERKLE_ROOT: Non-empty merkle root indicates script tree
            // Empty merkle root (all zeros) means key-path only
            if (!value.empty() && value.size() == 32) {
                // Check if all bytes are zero (key-path only)
                bool all_zeros = true;
                for (uint8_t byte : value) {
                    if (byte != 0) {
                        all_zeros = false;
                        break;
                    }
                }
                if (!all_zeros) {
                    // Non-zero merkle root = script tree exists
                    return true;
                }
            }
        }
    }

    return false;
}

TaprootValidationResult PSBTTaprootValidator::validateInput(
    const std::vector<PsbtMapKV>& input_kv,
    const std::string& wallet_policy
) {
    TaprootValidationResult result;
    result.valid = true;

    // Only enforce guardrails for BIP86 wallets
    if (wallet_policy != "bip86") {
        return result;  // BIP84 or other policies: no Taproot restrictions
    }

    // Check if this input is spending a Taproot output
    bool is_taproot_input = false;
    std::vector<uint8_t> witness_utxo_script;

    // Look for PSBT_IN_WITNESS_UTXO (type 0x01)
    for (const auto& kv : input_kv) {
        if (kv.key.empty()) continue;

        if (kv.key[0] == 0x01) {  // WITNESS_UTXO
            const auto& value = kv.value;
            if (!extractWitnessUtxoScript(value, witness_utxo_script)) {
                // Fail CLOSED: a witness UTXO we cannot canonically decode
                // means we cannot prove the input is not Taproot, so the
                // BIP86 guardrail cannot be evaluated. Refusing is the only
                // safe answer; skipping the guardrail here previously let a
                // malformed witness UTXO bypass the policy entirely.
                result.valid = false;
                result.error =
                    "BIP86 Policy Violation: witness UTXO could not be decoded; "
                    "refusing to validate this input (fail closed).";
                return result;
            }
            if (isTaprootScriptPubKey(witness_utxo_script)) {
                is_taproot_input = true;
                result.is_taproot = true;
            }
            break;
        }
    }

    // If not a Taproot input, no further validation needed
    if (!is_taproot_input) {
        return result;
    }

    // BIP86 GUARDRAIL: Check for script-path spending
    if (hasScriptPath(input_kv)) {
        result.valid = false;
        result.is_key_path = false;
        result.has_script_path = true;
        result.error = getViolationMessage(wallet_policy);
        return result;
    }

    // All checks passed
    result.is_key_path = true;
    result.has_script_path = false;
    return result;
}

std::string PSBTTaprootValidator::getViolationMessage(const std::string& wallet_policy) {
    std::ostringstream oss;

    if (wallet_policy == "bip86") {
        oss << "BIP86 Policy Violation: Script-path spending detected.\n"
            << "\n"
            << "This wallet uses BIP86 Taproot (key-path only) for maximum simplicity and privacy.\n"
            << "Script-path spending (complex Tapscript) is NOT allowed.\n"
            << "\n"
            << "The PSBT contains one or more of:\n"
            << "  - TAP_SCRIPT_SIG: Script-path signature (BIP 342)\n"
            << "  - TAP_LEAF_SCRIPT: Tapscript leaf in merkle tree\n"
            << "  - TAP_MERKLE_ROOT: Non-empty merkle root (indicates script tree)\n"
            << "\n"
            << "BIP86 wallets ONLY support key-path spending (simple single signature).\n"
            << "\n"
            << "To use script-path spending, create a custom wallet with advanced Tapscript features.\n"
            << "For standard use, BIP86 key-path spending provides maximum security and privacy.";
    } else {
        oss << "Taproot policy violation detected for wallet policy: " << wallet_policy;
    }

    return oss.str();
}

} // namespace dinero
