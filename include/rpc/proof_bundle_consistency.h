#pragma once
#include <cstdint>
#include <vector>

namespace dinero {
namespace rpc {

// A light client (DineroDPI) rejects a wallet.getproofbundle response unless every
// individual UTXO proof's num_leaves equals the bundle's stump num_leaves — the
// proofs and the accumulator root must describe ONE Utreexo forest state, or the
// proofs cannot verify against the stump. Because the server generates the proofs
// and snapshots the stump as separate forest reads, a block connecting on the sync
// thread in between grows the forest and makes them diverge, producing the
// client-side "proof leaf count mismatch" rejection. This is the invariant the
// server must guarantee before emitting a bundle.
//
// Returns true iff num_leaves is consistent across all proofs and the stump
// (vacuously true when there are no proofs).
inline bool ProofBundleLeafCountsConsistent(const std::vector<uint64_t>& proof_num_leaves,
                                            uint64_t stump_num_leaves) {
    for (uint64_t n : proof_num_leaves) {
        if (n != stump_num_leaves) {
            return false;
        }
    }
    return true;
}

}  // namespace rpc
}  // namespace dinero
