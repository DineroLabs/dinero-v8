#pragma once
#include "common/status.h"
#include "consensus/tx_validation.h"
#include <cstdint>
#include <vector>

namespace dinero::consensus {
// Public value flow, derived from authenticated transparent coins and the
// exact transaction's outputs/fee. No proof, note, commitment or claimed
// shielded value balance is an input to the counter arithmetic.
// This is a storage/consensus value type, NOT an authorization certificate.
// The block reward check must use these SAME validated fees: coinbase value
// <= subsidy + sum(fees), with checked arithmetic. An unclaimed fee may burn;
// it must not be credited back to the pool. This counter alone cannot enforce
// the reward bound or ordinary transparent transaction conservation.
// A validated ordinary transparent flow with fee = inputs - outputs is zero
// net, but legacy fee derivation/admission is separate. Never feed coinbase
// issuance into this counter, and never substitute zero for an absent fee.
struct OrchardValueFlow {
    uint64_t transparent_inputs = 0;
    uint64_t transparent_outputs = 0;
    uint64_t fee = 0;
};

// Draft rule: apply flows in canonical block transaction order, requiring the
// counter to stay in range after EACH transaction. Later deposits cannot fund
// an earlier underflow. This is deliberately explicit; it is stricter than
// checking only the final block total. No activation is selected here.
inline StatusOr<uint64_t> ApplyOrchardValueFlows(uint64_t parent_balance,
                                               const std::vector<OrchardValueFlow>& flows) {
    if (parent_balance > MAX_MONEY) return Status::Invalid;
    uint64_t balance = parent_balance;
    for (const auto& flow : flows) {
        if (flow.transparent_inputs > MAX_MONEY || flow.transparent_outputs > MAX_MONEY ||
            flow.fee > MAX_MONEY - flow.transparent_outputs) return Status::Invalid;
        const uint64_t out_and_fee = flow.transparent_outputs + flow.fee;
        if (flow.transparent_inputs >= out_and_fee) {
            const uint64_t deposit = flow.transparent_inputs - out_and_fee;
            if (deposit > MAX_MONEY - balance) return Status::Invalid;
            balance += deposit;
        } else {
            const uint64_t withdrawal = out_and_fee - flow.transparent_inputs;
            if (withdrawal > balance) return Status::Invalid;
            balance -= withdrawal;
        }
    }
    return balance;
}
} // namespace dinero::consensus
