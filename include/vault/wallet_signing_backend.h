// Copyright (c) 2026 Dinero Labs.
//
// Liquidity Vault — production signing backend backed by the live
// wallet RPC stack. Replaces InMemorySigningBackend for real-funds
// rollouts.
//
// Idempotency contract:
//   - Same `request_id` resubmitted MUST return the same broadcast txid.
//   - The cache survives daemon restarts via a JSON-line sidecar at
//     `<datadir>/vault/idempotency.jsonl` (`{request_id_hex, txid_hex}`
//     per line).
//   - The wallet RPC's coin-selection / signing path is the inner
//     mechanism; failure surfaces as a SigningBackendError.
//
// Concurrency: a single std::mutex serialises both signAndBroadcast
// and the sidecar append. The withdrawal queue calls one tx at a time
// from the vault service, so contention is non-existent in practice.

#pragma once

#include "vault/signing_backend.h"

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace dinero::vault {

class WalletSigningBackend : public SigningBackend {
   public:
    /// Caller passes a closure that takes (script_pub_key bytes,
    /// una amount, fee_rate_hint, audit_context) and returns the raw
    /// 32-byte broadcast txid on success, or throws
    /// SigningBackendError on any failure. The runtime wires this to
    /// the live wallet RPC dispatch
    /// (`g_rpcRegistry.lookup("wallet.sendtoaddress")`) and handles
    /// the script→address bech32m encoding inside the closure so
    /// this header has zero coupling to the address codec.
    using SendCallback = std::function<std::array<uint8_t, 32>(
        const std::vector<uint8_t>& script_pub_key, UnaAmount amount,
        UnaAmount fee_rate_hint, const std::string& audit_context)>;

    /// Caller supplies an available-float lookup (typically the
    /// active wallet's spendable balance, in una). 0-arg → returns
    /// the current balance.
    using FloatCallback = std::function<UnaAmount()>;

    WalletSigningBackend(BackendId id, SendCallback send,
                         FloatCallback float_lookup,
                         std::string idempotency_path);

    [[nodiscard]] const BackendId& backendId() const noexcept override { return id_; }
    UnaAmount availableFloat() override;
    std::array<uint8_t, 32> signAndBroadcast(const UnsignedTx& tx) override;
    HealthReport healthcheck() override;

   private:
    void loadIdempotency();
    void persistIdempotency(const std::array<uint8_t, 16>& request_id,
                            const std::array<uint8_t, 32>& txid);

    BackendId id_;
    SendCallback send_;
    FloatCallback float_lookup_;
    std::string idempotency_path_;

    std::mutex mu_;
    std::unordered_map<std::string /* hex(request_id) */,
                       std::array<uint8_t, 32>>
        cache_;
};

}  // namespace dinero::vault
