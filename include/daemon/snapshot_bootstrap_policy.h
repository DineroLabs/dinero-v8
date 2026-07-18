#pragma once

#include "consensus/interfaces/iconsensus_utxo_set.h"
#include "primitives/transaction.h"

namespace dinero::assumeutxo {

// A fresh node initializes the canonical genesis coin before deferred snapshot
// loading. Identify that state by exact UTXO identity and contents rather than
// ChainDB tip height: header synchronization may advance persisted metadata
// before the snapshot loader runs, but it must never authorize clearing real
// chainstate.
inline bool IsGenesisOnlyUtxoSet(
    const consensus::IConsensusUTXOSet& utxo_set,
    const Transaction& genesis_coinbase) {
    if (!genesis_coinbase.IsCoinbase() ||
        utxo_set.GetSetSize() != genesis_coinbase.vout.size()) {
        return false;
    }

    const TxId txid = genesis_coinbase.GetTxid();
    for (uint32_t vout = 0; vout < genesis_coinbase.vout.size(); ++vout) {
        const auto* coin = utxo_set.GetCoin(OutPoint(txid, vout));
        const auto& expected = genesis_coinbase.vout[vout];
        if (!coin || coin->height != 0 || !coin->isCoinbase ||
            coin->value != expected.value ||
            coin->scriptPubKey != expected.scriptPubKey ||
            coin->is_confidential != expected.is_confidential ||
            coin->commitment != expected.commitment) {
            return false;
        }
    }
    return true;
}

}  // namespace dinero::assumeutxo
