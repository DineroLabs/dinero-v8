// Copyright (c) 2026 Dinero Labs.
//
// Liquidity Vault — pluggable signing/liquidity backend interface.
// Daemon-side port of `Core/Vault/SigningBackend.swift`.
//
// The vault is OPINIONATED about what it asks the backend for
// (sign this tx, report this float, healthcheck) and AGNOSTIC
// about how the backend fulfills the request. Concrete deployment
// mappings — wallet, multisig, HSM, distributed liquidity-provider —
// sit behind this interface.

#pragma once

#include "vault/vault_types.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace dinero::vault {

/// One output the backend should construct + sign on behalf of the
/// vault. Stored as raw bytes so the backend can decode to whatever
/// address format it speaks.
struct SignOutput {
    UnaAmount value{0};
    std::vector<uint8_t> script_pub_key;
};

/// Outline of a tx the vault wants the backend to sign + broadcast.
/// `request_id` is for idempotency — the backend MUST refuse to
/// double-sign the same logical request.
struct UnsignedTx {
    /// Stable per-request identifier. Same id re-submitted returns
    /// the same broadcast txid.
    std::array<uint8_t, 16> request_id{};
    std::vector<SignOutput> outputs;
    /// Optional fee rate hint in una/byte. 0 = let the backend choose.
    UnaAmount fee_rate_hint{0};
    /// Free-form audit context for the backend's internal log.
    std::string audit_context;
};

/// Health status surfaced to monitoring. The vault pauses new credit
/// openings whenever a backend transitions to `degraded` or `down`.
struct BackendHealthy {
    bool operator==(const BackendHealthy&) const = default;
};
struct BackendDegraded {
    std::string reason;
    bool operator==(const BackendDegraded&) const = default;
};
struct BackendDown {
    std::string reason;
    bool operator==(const BackendDown&) const = default;
};
using BackendHealth = std::variant<BackendHealthy, BackendDegraded, BackendDown>;

struct HealthReport {
    BackendId backend;
    BackendHealth status;
    UnaAmount available_float{0};
    /// Unix-nanos timestamp.
    LedgerTimestamp timestamp{0};
};

/// Errors a backend may surface. Concrete implementations map their
/// internal failure modes onto these cases so the vault doesn't need
/// backend-specific error handling.
class SigningBackendError : public std::runtime_error {
   public:
    enum class Kind : uint8_t {
        UNAVAILABLE,
        INSUFFICIENT_FLOAT,
        DUPLICATE_REQUEST,
        REJECTED_BY_POLICY,
        BROADCAST_FAILED,
        INTERNAL_ERROR,
    };
    SigningBackendError(Kind kind, const std::string& message)
        : std::runtime_error(message), kind_{kind} {}
    [[nodiscard]] Kind kind() const noexcept { return kind_; }

   private:
    Kind kind_;
};

/// The contract every vault backend implements. Synchronous on the
/// daemon side — most callers are within a single chain-event handler;
/// any IO-bound backend (HSM, remote custodian) wraps that latency
/// internally and surfaces a healthcheck rather than blocking the
/// vault's main path indefinitely.
class SigningBackend {
   public:
    SigningBackend() = default;
    SigningBackend(const SigningBackend&) = delete;
    SigningBackend& operator=(const SigningBackend&) = delete;
    SigningBackend(SigningBackend&&) = delete;
    SigningBackend& operator=(SigningBackend&&) = delete;
    virtual ~SigningBackend() = default;

    /// Stable identity, recorded on every ledger entry the backend
    /// participates in. MUST NOT change for the lifetime of one
    /// running backend instance.
    [[nodiscard]] virtual const BackendId& backendId() const noexcept = 0;

    /// Currently spendable float, in `una`. Drives the vault's cap
    /// enforcement.
    virtual UnaAmount availableFloat() = 0;

    /// Sign + broadcast. Returns the raw 32-byte broadcast txid (NOT
    /// display-order hex). MUST be idempotent on `request_id`.
    virtual std::array<uint8_t, 32> signAndBroadcast(const UnsignedTx& tx) = 0;

    /// Self-check. Surfaces both health + float in one call.
    virtual HealthReport healthcheck() = 0;
};

/// In-memory test backend. Synthesizes a deterministic txid from
/// `(request_id, total_outputs)` so tests can re-derive expected
/// values. NOT for production.
class InMemorySigningBackend : public SigningBackend {
   public:
    explicit InMemorySigningBackend(BackendId id, UnaAmount initial_float = 1'000'000'000)
        : id_{std::move(id)}, float_{initial_float} {}

    [[nodiscard]] const BackendId& backendId() const noexcept override { return id_; }
    UnaAmount availableFloat() override;
    std::array<uint8_t, 32> signAndBroadcast(const UnsignedTx& tx) override;
    HealthReport healthcheck() override;

    /// Test-only mutators.
    void setStatus(BackendHealth status) { status_ = std::move(status); }
    void setFloat(UnaAmount f) { float_ = f; }
    void setNextErrorTrap(std::optional<SigningBackendError::Kind> trap) { next_error_trap_ = trap; }

   private:
    BackendId id_;
    UnaAmount float_{0};
    BackendHealth status_{BackendHealthy{}};
    std::optional<SigningBackendError::Kind> next_error_trap_;
    std::vector<std::pair<std::array<uint8_t, 16>, std::array<uint8_t, 32>>> broadcasted_;
};

}  // namespace dinero::vault
