#pragma once

#include "wallet/taproot_template_builder.h"
#include "wallet/canonical_wallet_utxo.h"
#include "wallet/intent_descriptor.h"
#include <array>
#include <cstdint>
#include <vector>

// Forward declarations
namespace dinero {
struct Transaction;
}

namespace dinero {

/**
 * Script-path spending helpers for PROTECTED template outputs.
 *
 * Constructs witnesses for:
 * - Panic leaf: immediate cancel via panic key (CSV-limited)
 * - Recovery leaf: disaster recovery via recovery key (long CSV delay)
 *
 * Witness structure (BIP341 script-path):
 *   [signature] [script] [control_block]
 */
class ProtectedSpend {
public:
    /**
     * Spend via the panic leaf (script-path).
     *
     * Builds the witness: [schnorr_sig] [panic_script] [control_block]
     * The spending tx must set nSequence >= panic_window_blocks.
     *
     * @param tx Transaction to sign (input must have correct nSequence)
     * @param input_index Index of the input being spent
     * @param utxos All transaction UTXOs (for sighash computation)
     * @param panic_leaf LeafInfo for the panic leaf
     * @param panic_private_key 32-byte private key for panic pubkey
     * @param ext_commitment Optional ext_commitment for V1 sighash
     * @return true on success
     */
    static bool SpendFromPanicLeaf(
        Transaction& tx,
        size_t input_index,
        const std::vector<CanonicalWalletUTXO>& utxos,
        const TemplateTreeResult::LeafInfo& panic_leaf,
        const std::vector<uint8_t>& panic_private_key,
        const std::array<uint8_t, 32>& ext_commitment = DEFAULT_EXT_COMMITMENT
    );

    /**
     * Spend via the recovery leaf (script-path).
     *
     * Builds the witness: [schnorr_sig] [recovery_script] [control_block]
     * The spending tx must set nSequence >= recovery_delay_blocks.
     *
     * @param tx Transaction to sign
     * @param input_index Index of the input being spent
     * @param utxos All transaction UTXOs
     * @param recovery_leaf LeafInfo for the recovery leaf
     * @param recovery_private_key 32-byte private key for recovery pubkey
     * @param ext_commitment Optional ext_commitment for V1 sighash
     * @return true on success
     */
    static bool SpendFromRecoveryLeaf(
        Transaction& tx,
        size_t input_index,
        const std::vector<CanonicalWalletUTXO>& utxos,
        const TemplateTreeResult::LeafInfo& recovery_leaf,
        const std::vector<uint8_t>& recovery_private_key,
        const std::array<uint8_t, 32>& ext_commitment = DEFAULT_EXT_COMMITMENT
    );

private:
    /**
     * Common script-path spend: compute sighash, sign, assemble witness.
     */
    static bool ScriptPathSpend(
        Transaction& tx,
        size_t input_index,
        const std::vector<CanonicalWalletUTXO>& utxos,
        const TemplateTreeResult::LeafInfo& leaf,
        const std::vector<uint8_t>& private_key,
        const std::array<uint8_t, 32>& ext_commitment
    );
};

} // namespace dinero
