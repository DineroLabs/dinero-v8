// Copyright (c) 2026 Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "consensus/shielded/shielded_output_feed.h"

#include "consensus/shielded/shielded_serialization.h"
#include "consensus/shielded/shielded_tx.h"
#include "primitives/block.h"
#include "primitives/transaction.h"

#include <cstring>

namespace dinero::consensus::shielded {

ShieldedOutputFeedError ExtractShieldedOutputFeed(
    const dinero::Block& block,
    uint32_t height,
    uint64_t first_leaf_index,
    ShieldedOutputFeedResult* out) {

    if (out == nullptr) {
        return ShieldedOutputFeedError::BundleDecodeFailed;
    }

    out->outputs.clear();
    out->spent_nullifiers.clear();
    out->next_leaf_index = first_leaf_index;

    const ::dinero::uint256 block_hash = block.GetHash();
    uint64_t                next_leaf  = first_leaf_index;

    for (uint32_t tx_index = 0; tx_index < block.vtx.size(); ++tx_index) {
        const ::dinero::Transaction& tx = block.vtx[tx_index];

        // Skip non-shielded transactions and shielded transactions
        // whose wallet declined to attach a bundle (consensus-valid
        // shape per `Transaction::IsShielded()`).
        if (!::dinero::Transaction::IsShieldedVersion(tx.version)) continue;
        if (tx.shielded_bundle_bytes.empty())                       continue;

        ShieldedBundle bundle{};
        const auto decode = DeserializeShieldedBundle(tx.shielded_bundle_bytes,
                                                       &bundle);
        if (decode != BundleDecodeError::Ok) {
            return ShieldedOutputFeedError::BundleDecodeFailed;
        }

        const ::dinero::TxId txid = ::dinero::TxId::Compute(tx);

        // Spends emit nullifier-feed entries but do NOT advance the leaf
        // counter — only outputs append to the commitment tree.
        for (uint32_t spend_index = 0;
             spend_index < bundle.spends.size();
             ++spend_index) {
            const ShieldedSpend& s = bundle.spends[spend_index];
            ShieldedNullifierFeedEntry entry{};
            entry.block_hash  = block_hash;
            entry.height      = height;
            entry.txid        = txid;
            entry.tx_index    = tx_index;
            entry.spend_index = spend_index;
            entry.nullifier   = s.nullifier;
            out->spent_nullifiers.push_back(std::move(entry));
        }

        // Outputs emit per-output entries with monotonically advancing
        // leaf indices in canonical bundle order (already sorted by
        // commitment via DeserializeShieldedBundle).
        for (uint32_t output_index = 0;
             output_index < bundle.outputs.size();
             ++output_index) {
            const ShieldedOutput& o = bundle.outputs[output_index];
            if (o.encrypted_note.size() != kShieldedEncryptedNoteBytes) {
                return ShieldedOutputFeedError::EncryptedNoteWrongSize;
            }
            ShieldedOutputFeedEntry entry{};
            entry.block_hash     = block_hash;
            entry.height         = height;
            entry.txid           = txid;
            entry.tx_index       = tx_index;
            entry.output_index   = output_index;
            entry.leaf_index     = next_leaf;
            entry.commitment     = o.commitment;
            entry.encrypted_note = o.encrypted_note;
            out->outputs.push_back(std::move(entry));
            ++next_leaf;
        }
    }

    out->next_leaf_index = next_leaf;
    return ShieldedOutputFeedError::Ok;
}

} // namespace dinero::consensus::shielded
