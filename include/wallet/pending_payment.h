#pragma once
#include <cstdint>
#include <array>
#include <optional>
#include <string>
#include <vector>
namespace dinero {
// Explicit external operation identity retained inside the authenticated wallet
// payment owner. Supplying these bytes does not establish a vault or pool's existence,
// authorization or durable initialization; those remain the caller's contract.
// No free-form payment label is interpreted as a request identity.
enum class PendingPaymentRequestDomain : uint64_t { VaultWithdrawal = 1, PoolPayout = 2 };
struct PendingPaymentRequest {
    PendingPaymentRequestDomain domain{PendingPaymentRequestDomain::VaultWithdrawal};
    std::array<uint8_t,32> owner{};
    std::array<uint8_t,16> id{};
    uint64_t fee_rate_hint = 0;
    uint64_t maximum_fee_una = 0;
    std::string audit_context;
    // Sorted, unique, nonzero allocation references for a PoolPayout batch.
    // These bind an already-established caller operation to the retained body;
    // they do not certify a pool allocation or authorize creating missing IDs.
    // Empty retains the historical request format and narrower contract.
    std::vector<std::array<uint8_t,32>> pool_origins;
    bool operator==(const PendingPaymentRequest&) const = default;
};
// Intent supplied by a wallet-created payment RPC, not arbitrary raw signing.
struct PendingPaymentRecipient {
    std::string address;
    uint64_t amount_una = 0;
    bool operator==(const PendingPaymentRecipient&) const = default;
};
struct PendingPaymentIntent {
    std::string address;
    uint64_t amount_una=0;
    std::string label;
    // The primary recipient above retains the original single-payment API.
    // Additional recipients are explicit request data, never inferred change.
    std::vector<PendingPaymentRecipient> additional_recipients;
    std::optional<PendingPaymentRequest> request;
    bool operator==(const PendingPaymentIntent&) const = default;
};
struct PendingPaymentInput {
    std::string txid;
    uint32_t vout=0;
    uint64_t amount_una=0;
    std::vector<uint8_t> script;
};
// Retained origin, not an assertion of mempool acceptance or chain confirmation.
struct PendingPayment {
    std::string txid;
    std::vector<uint8_t> signed_body;
    PendingPaymentIntent intent;
    uint64_t fee_una=0;
    int64_t created_at=0;
    std::vector<PendingPaymentInput> inputs;
};
}
