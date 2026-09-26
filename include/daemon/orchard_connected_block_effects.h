#pragma once

#include "daemon/connected_block_effects.h"
#include "primitives/orchard_block_reader.h"
#include <algorithm>

namespace dinero {

// Extract only transparent conflict identities from the exact typed body.
// The caller must first complete all block/transaction validation. Orchard
// nullifier conflicts belong to the separate Orchard admission path.
inline ConnectedBlockEffects BuildOrchardConnectedBlockEffects(
    const OrchardBlockCandidate& block) {
    ConnectedBlockEffects effects;
    effects.confirmed_txids.reserve(block.Transactions().size());
    for (const auto& parsed : block.Transactions()) {
        effects.confirmed_txids.push_back(parsed.GetTxid().AsUint256());
        if (parsed.IsOrchard()) {
            for (const auto& input : parsed.Orchard().Inputs()) {
                uint256 hash;
                std::copy(input.txid_wire.begin(), input.txid_wire.end(), hash.begin());
                effects.spent_transparent_inputs.emplace_back(TxId(hash), input.output_index);
            }
        } else if (!parsed.Historical().IsCoinbase()) {
            for (const auto& input : parsed.Historical().vin)
                effects.spent_transparent_inputs.emplace_back(input.prevout.txid, input.prevout.vout);
        }
    }
    return effects;
}

} // namespace dinero
