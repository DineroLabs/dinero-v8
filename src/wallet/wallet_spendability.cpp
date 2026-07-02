#include "wallet/wallet_spendability.h"

namespace dinero {
namespace wallet {

SpendabilityPartition PartitionBySpendability(
    const std::vector<CanonicalWalletUTXO>& utxos,
    const SpendableInActiveSetFn& is_spendable) {
    SpendabilityPartition out;
    for (const auto& utxo : utxos) {
        // A null/empty check means the node doesn't distinguish an active set (full
        // node) — preserve legacy behavior: everything spendable. Otherwise a coin the
        // node can't spend (not in the active UTXO set) is kept but moved to `anchored`
        // so coin-selection never offers it and spendable_balance excludes it.
        const bool spendable = !is_spendable || is_spendable(utxo.txid, utxo.vout);
        if (spendable) {
            out.spendable_una += utxo.value.GetUna();
            out.spendable.push_back(utxo);
        } else {
            out.anchored_una += utxo.value.GetUna();
            out.anchored.push_back(utxo);
        }
    }
    return out;
}

}  // namespace wallet
}  // namespace dinero
