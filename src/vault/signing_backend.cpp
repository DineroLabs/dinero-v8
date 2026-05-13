// Copyright (c) 2026 Dinero Labs.
//
// Daemon-side port of `Core/Vault/InMemorySigningBackend.swift`.
// Used by tests + as a reference shape for production backend
// implementations.

#include "vault/signing_backend.h"

#include <chrono>
#include <cstring>

namespace dinero::vault {

UnaAmount InMemorySigningBackend::availableFloat() {
    if (auto* down = std::get_if<BackendDown>(&status_); down != nullptr) {
        throw SigningBackendError(SigningBackendError::Kind::UNAVAILABLE, down->reason);
    }
    return float_;
}

namespace {

// Lightweight 32-byte hash: FNV-1a over the bytes. Production
// backends use real cryptographic hashes; this is just a stable
// per-input synthesis for the in-memory test backend.
std::array<uint8_t, 32> synthesizeTxid(const std::array<uint8_t, 16>& request_id, UnaAmount total) {
    constexpr uint64_t FNV_OFFSET = 0xcbf29ce484222325ULL;
    constexpr uint64_t FNV_PRIME = 0x100000001b3ULL;
    uint64_t a = FNV_OFFSET;
    uint64_t b = FNV_OFFSET;
    for (uint8_t byte : request_id) {
        a = (a ^ byte) * FNV_PRIME;
        b = (b ^ byte) * FNV_PRIME;
    }
    for (int i = 0; i < 8; ++i) {
        auto byte = static_cast<uint8_t>(total >> (i * 8U));
        a = (a ^ byte) * FNV_PRIME;
        b = (b ^ byte) * FNV_PRIME;
    }
    std::array<uint8_t, 32> out{};
    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<uint8_t>(a >> (i * 8U));
        out[i + 8] = static_cast<uint8_t>(b >> (i * 8U));
        out[i + 16] = static_cast<uint8_t>((a ^ b) >> (i * 8U));
        out[i + 24] = static_cast<uint8_t>((a + b) >> (i * 8U));
    }
    return out;
}

LedgerTimestamp nowNanos() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

std::array<uint8_t, 32> InMemorySigningBackend::signAndBroadcast(const UnsignedTx& tx) {
    if (next_error_trap_.has_value()) {
        SigningBackendError::Kind kind = *next_error_trap_;
        next_error_trap_.reset();
        throw SigningBackendError(kind, "test-injected");
    }
    if (auto* down = std::get_if<BackendDown>(&status_); down != nullptr) {
        throw SigningBackendError(SigningBackendError::Kind::UNAVAILABLE, down->reason);
    }
    // Idempotency: same request_id → same fake txid, no double-debit.
    for (const auto& [rid, tid] : broadcasted_) {
        if (rid == tx.request_id) {
            return tid;
        }
    }
    UnaAmount total = 0;
    for (const auto& output : tx.outputs) {
        total += output.value;
    }
    if (total > float_) {
        throw SigningBackendError(SigningBackendError::Kind::INSUFFICIENT_FLOAT,
                                  "requested > available");
    }
    auto txid = synthesizeTxid(tx.request_id, total);
    broadcasted_.emplace_back(tx.request_id, txid);
    float_ = total <= float_ ? float_ - total : 0;
    return txid;
}

HealthReport InMemorySigningBackend::healthcheck() {
    return HealthReport{id_, status_, float_, nowNanos()};
}

}  // namespace dinero::vault
