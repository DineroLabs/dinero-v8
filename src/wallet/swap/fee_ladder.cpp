#include "wallet/swap/fee_ladder.h"

#include <stdexcept>

namespace dinero::swap {
namespace {

constexpr uint32_t kMaxRungs = 16;

void ValidatePolicy(const FeeLadderPolicy& policy, const FundingOutput& funding) {
    if (policy.start_feerate_una_per_vb == 0) throw std::invalid_argument("fee ladder: zero feerate");
    if (policy.max_rungs == 0 || policy.max_rungs > kMaxRungs) {
        throw std::invalid_argument("fee ladder: rung count must be 1..16");
    }
    if (policy.max_fee_una >= funding.value.GetUna()) {
        throw std::invalid_argument("fee ladder: fee cap must be below the funding value");
    }
}

// Build rungs from a template builder. The signed size does not depend on the
// amounts (8-byte value field), so it is measured once with a placeholder
// witness of exactly the real element sizes.
template <typename Build, typename Witness, typename Sighash>
std::vector<FeeRung> BuildLadder(const FundingOutput& funding,
                                 const std::vector<uint8_t>& payout_script,
                                 const FeeLadderPolicy& policy, Build build, Witness witness,
                                 Sighash sighash) {
    ValidatePolicy(policy, funding);
    Transaction sized = build(Payout{payout_script, AmountUna::Una(0)});
    sized.vin[0].witness = witness();
    const uint64_t vsize = sized.GetVirtualSize();

    std::vector<FeeRung> rungs;
    uint64_t feerate = policy.start_feerate_una_per_vb;
    for (uint32_t i = 0; i < policy.max_rungs; ++i, feerate *= 2) {
        if (feerate > policy.max_fee_una / vsize) break;  // fee would exceed the cap (no overflow)
        const uint64_t fee = feerate * vsize;
        if (funding.value.GetUna() - fee < policy.min_payout_una) break;
        FeeRung rung;
        rung.tx = build(Payout{payout_script, AmountUna::Una(fee)});
        rung.fee_una = fee;
        rung.feerate_una_per_vb = feerate;
        rung.sighash = sighash(rung.tx);
        rungs.push_back(std::move(rung));
    }
    if (rungs.empty()) {
        throw std::invalid_argument("fee ladder: the first rung exceeds the fee cap or payout floor");
    }
    return rungs;
}

}  // namespace

std::vector<FeeRung> BuildDinClaimLadder(const DinHtlcOutput& htlc, const FundingOutput& funding,
                                         const std::vector<uint8_t>& payout_script,
                                         const FeeLadderPolicy& policy) {
    return BuildLadder(
        funding, payout_script, policy,
        [&](const Payout& p) { return BuildDinClaimTx(htlc, funding, p); },
        [&] {
            return std::vector<std::vector<uint8_t>>{std::vector<uint8_t>(64), std::vector<uint8_t>(32),
                                                     htlc.claim_script, htlc.claim_control_block};
        },
        [&](const Transaction& tx) { return DinClaimSighash(tx, funding, htlc); });
}

std::vector<FeeRung> BuildDinRefundLadder(const DinHtlcTerms& terms, const DinHtlcOutput& htlc,
                                          const FundingOutput& funding,
                                          const std::vector<uint8_t>& payout_script,
                                          const FeeLadderPolicy& policy) {
    return BuildLadder(
        funding, payout_script, policy,
        [&](const Payout& p) { return BuildDinRefundTx(terms, htlc, funding, p); },
        [&] {
            return std::vector<std::vector<uint8_t>>{std::vector<uint8_t>(64), htlc.refund_script,
                                                     htlc.refund_control_block};
        },
        [&](const Transaction& tx) { return DinRefundSighash(tx, funding, htlc); });
}

}  // namespace dinero::swap
