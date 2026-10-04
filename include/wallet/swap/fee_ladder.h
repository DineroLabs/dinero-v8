#pragma once
// Pre-signed fee ladders for swap claims and refunds (milestone 2 of
// docs/design/din-btc-atomic-swaps-v1-plan.md, design §6.2).
//
// A keyless watchtower cannot re-fee a transaction, so Bob pre-signs each
// claim/refund at several fee levels. Every rung pays the SAME destination;
// only the fee differs, and fees rise strictly so a higher rung can replace a
// lower one (all rungs signal replaceability). Rungs are sized from the real
// signed size, never exceed the absolute fee cap, and never leave the payout
// below the minimum. Callers sign each rung's sighash (SIGHASH_DEFAULT), so the
// tower can choose a rung but never change where the money goes.

#include "wallet/swap/htlc.h"

#include <vector>

namespace dinero::swap {

struct FeeLadderPolicy {
    uint64_t start_feerate_una_per_vb{};  // first rung
    uint32_t max_rungs{8};                // each next rung doubles the feerate
    uint64_t max_fee_una{};               // absolute cap per rung (user-visible)
    uint64_t min_payout_una{};            // payout never below this
};

struct FeeRung {
    Transaction tx;     // unsigned; complete with SetDin*Witness after signing
    uint64_t fee_una{};
    uint64_t feerate_una_per_vb{};
    Bytes32 sighash{};  // sign this (BIP340, SIGHASH_DEFAULT)
};

// Throw std::invalid_argument for an invalid policy or when not even the first
// rung fits under the cap / above the minimum payout.
std::vector<FeeRung> BuildDinClaimLadder(const DinHtlcOutput& htlc, const FundingOutput& funding,
                                         const std::vector<uint8_t>& payout_script,
                                         const FeeLadderPolicy& policy);
std::vector<FeeRung> BuildDinRefundLadder(const DinHtlcTerms& terms, const DinHtlcOutput& htlc,
                                          const FundingOutput& funding,
                                          const std::vector<uint8_t>& payout_script,
                                          const FeeLadderPolicy& policy);

}  // namespace dinero::swap
