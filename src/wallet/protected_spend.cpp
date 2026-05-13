#include "wallet/protected_spend.h"
#include "wallet/taproot_tx_signer.h"
#include "wallet/taproot_control_block.h"
#include "wallet/schnorr_signer.h"
#include "primitives/transaction.h"
#include <stdexcept>

namespace dinero {

bool ProtectedSpend::SpendFromPanicLeaf(
    Transaction& tx,
    size_t input_index,
    const std::vector<CanonicalWalletUTXO>& utxos,
    const TemplateTreeResult::LeafInfo& panic_leaf,
    const std::vector<uint8_t>& panic_private_key,
    const std::array<uint8_t, 32>& ext_commitment) {

    return ScriptPathSpend(tx, input_index, utxos,
        panic_leaf, panic_private_key, ext_commitment);
}

bool ProtectedSpend::SpendFromRecoveryLeaf(
    Transaction& tx,
    size_t input_index,
    const std::vector<CanonicalWalletUTXO>& utxos,
    const TemplateTreeResult::LeafInfo& recovery_leaf,
    const std::vector<uint8_t>& recovery_private_key,
    const std::array<uint8_t, 32>& ext_commitment) {

    return ScriptPathSpend(tx, input_index, utxos,
        recovery_leaf, recovery_private_key, ext_commitment);
}

bool ProtectedSpend::ScriptPathSpend(
    Transaction& tx,
    size_t input_index,
    const std::vector<CanonicalWalletUTXO>& utxos,
    const TemplateTreeResult::LeafInfo& leaf,
    const std::vector<uint8_t>& private_key,
    const std::array<uint8_t, 32>& ext_commitment) {

    if (input_index >= tx.vin.size()) {
        return false;
    }
    if (private_key.size() != 32) {
        return false;
    }

    // 1. Compute script-path sighash (V1 with ext_commitment)
    auto sighash = TaprootTxSigner::ComputeScriptPathSighashV1(
        tx, input_index, utxos,
        leaf.script,
        ext_commitment,
        leaf.control_block.leaf_version,
        TaprootTxSigner::SIGHASH_DEFAULT
    );

    if (sighash.size() != 32) {
        return false;
    }

    // 2. Sign with Schnorr (BIP340)
    // For script-path, we sign with the raw (untweaked) private key
    auto sig_opt = din::SchnorrSigner::sign(sighash, private_key);
    if (!sig_opt.has_value()) {
        return false;
    }

    const auto& sig = sig_opt.value();
    if (sig.size() != 64) {
        return false;
    }

    // 3. Build witness: [signature] [script] [control_block]
    std::vector<std::vector<uint8_t>> signatures = {sig};
    auto witness = buildScriptPathWitness(signatures, leaf.script, leaf.control_block);

    // 4. Assign witness to the transaction input
    tx.vin[input_index].witness = std::move(witness);

    return true;
}

} // namespace dinero
