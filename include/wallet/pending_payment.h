#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace dinero {
// Intent supplied by a wallet-created payment RPC, not arbitrary raw signing.
struct PendingPaymentIntent {
    std::string address;
    uint64_t amount_una=0;
    std::string label;
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
