// Copyright (c) 2026 Dinero Labs.
//
// Liquidity Vault — withdrawal-side queue + state machine.
// Daemon-side port of `Core/Vault/WithdrawalQueue.swift`.
//
// State machine: pending → signing → broadcast → settled | reverted | failed.

#pragma once

#include "vault/ledger.h"
#include "vault/signing_backend.h"
#include "vault/vault_types.h"

#include <array>
#include <chrono>
#include <climits>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace dinero::vault {

/// Stable per-request identifier. 16 bytes from `request_id` of the
/// signing call.
using WithdrawalId = std::array<uint8_t, 16>;

/// Ledger key for a withdrawal's lifecycle. Keyed by the STABLE REQUEST
/// ID, not by the broadcast txid: the reservation entry
/// (WithdrawalInitiated) is written before the broadcast for crash
/// safety, so no txid exists yet. The 16-byte id is left-aligned into
/// the 32-byte outpoint field; the txid is bound afterwards by
/// WithdrawalBroadcastRecorded.
///
/// Bonus: the ledger and `vault.withdrawal.status` now agree on one
/// identifier — the request id the RPC already hands back.
OutpointId outpointForWithdrawalRequest(const WithdrawalId& id);

struct WithdrawalRequest {
    WithdrawalId request_id{};
    AccountId account;
    UnaAmount amount{0};
    std::vector<uint8_t> destination_script_pub_key;
    LedgerTimestamp created_at{0};
};

struct WithdrawalPending {
    bool operator==(const WithdrawalPending&) const = default;
};
struct WithdrawalSigning {
    bool operator==(const WithdrawalSigning&) const = default;
};
struct WithdrawalBroadcast {
    std::array<uint8_t, 32> txid{};
    /// Inclusion height (set after `markBroadcastIncluded`); 0 means
    /// not yet included.
    uint64_t included_at_height{0};
    bool operator==(const WithdrawalBroadcast&) const = default;
};
struct WithdrawalSettledOnChain {
    std::array<uint8_t, 32> txid{};
    bool operator==(const WithdrawalSettledOnChain&) const = default;
};
struct WithdrawalRevertedOnChain {
    std::array<uint8_t, 32> txid{};
    bool operator==(const WithdrawalRevertedOnChain&) const = default;
};
struct WithdrawalFailed {
    std::string reason;
    bool operator==(const WithdrawalFailed&) const = default;
};
using WithdrawalState = std::variant<
    WithdrawalPending,
    WithdrawalSigning,
    WithdrawalBroadcast,
    WithdrawalSettledOnChain,
    WithdrawalRevertedOnChain,
    WithdrawalFailed
>;

struct WithdrawalCaps {
    UnaAmount per_request{UINT64_MAX};
    UnaAmount per_account_outstanding{UINT64_MAX};
    int global_queue_depth{INT_MAX};

    static WithdrawalCaps unbounded() { return WithdrawalCaps{}; }
};

struct WithdrawalConfirmationPolicy {
    uint64_t k_settle{2};
    static WithdrawalConfirmationPolicy defaults() { return WithdrawalConfirmationPolicy{}; }
};

class WithdrawalQueueError : public std::runtime_error {
   public:
    enum class Kind : uint8_t {
        ZERO_AMOUNT,
        INSUFFICIENT_SPENDABLE,
        PER_REQUEST_CAP_EXCEEDED,
        PER_ACCOUNT_OUTSTANDING_EXCEEDED,
        GLOBAL_QUEUE_FULL,
        BACKEND_ERROR,
        LEDGER_ERROR,
        UNKNOWN_REQUEST,
        LIFECYCLE_VIOLATION,
        DESTINATION_REJECTED,
    };
    WithdrawalQueueError(Kind kind, const std::string& message)
        : std::runtime_error(message), kind_{kind} {}
    [[nodiscard]] Kind kind() const noexcept { return kind_; }

   private:
    Kind kind_;
};

/// Pure-code orchestrator. Single-threaded.
class WithdrawalQueue {
   public:
    struct WithdrawalIdHasher {
        size_t operator()(const WithdrawalId& id) const noexcept {
            size_t h = 0;
            for (auto byte : id) {
                h = (h * 31U) + byte;
            }
            return h;
        }
    };
    using RequestMap = std::unordered_map<WithdrawalId, WithdrawalRequest, WithdrawalIdHasher>;
    using StateMap = std::unordered_map<WithdrawalId, WithdrawalState, WithdrawalIdHasher>;

    using DestinationValidatorFn = std::function<bool(const std::vector<uint8_t>&)>;
    using RequestIdGeneratorFn = std::function<WithdrawalId()>;

    WithdrawalQueue(Ledger* ledger, SigningBackend* backend,
                    WithdrawalCaps caps = WithdrawalCaps::unbounded(),
                    WithdrawalConfirmationPolicy policy = WithdrawalConfirmationPolicy::defaults())
        : ledger_{ledger}, backend_{backend}, caps_{caps}, policy_{policy} {
        destination_validator_ = [](const std::vector<uint8_t>& spk) { return !spk.empty(); };
        request_id_generator_ = []() {
            // Default: derive from monotonic clock + counter. Tests
            // override with a deterministic generator.
            static uint64_t counter = 0;
            counter += 1;
            WithdrawalId rid{};
            auto now = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
            for (int i = 0; i < 8; ++i) {
                rid[i] = static_cast<uint8_t>(now >> (i * 8U));
                rid[i + 8] = static_cast<uint8_t>(counter >> (i * 8U));
            }
            return rid;
        };
    }

    void setCaps(WithdrawalCaps caps) { caps_ = caps; }
    void setPolicy(WithdrawalConfirmationPolicy policy) { policy_ = policy; }
    void setDestinationValidator(DestinationValidatorFn fn) { destination_validator_ = std::move(fn); }
    void setRequestIdGenerator(RequestIdGeneratorFn fn) { request_id_generator_ = std::move(fn); }

    [[nodiscard]] const WithdrawalCaps& caps() const noexcept { return caps_; }
    [[nodiscard]] const RequestMap& requests() const noexcept { return requests_; }

    /// Validate + enqueue. Returns the request id; caller calls
    /// `processNext` to drive it through the state machine.
    WithdrawalId enqueue(const AccountId& account, UnaAmount amount,
                         const std::vector<uint8_t>& destination_script_pub_key);

    /// Advance one pending request: backend.signAndBroadcast →
    /// ledger withdrawal_initiated → state = .broadcast.
    /// Returns the processed request id, or `nullopt` if nothing pending.
    std::optional<WithdrawalId> processNext();

    /// Caller (chain watcher) reports broadcast tx was included at
    /// `height`. Required input for `tipChanged` to count confs.
    void markBroadcastIncluded(const WithdrawalId& id, uint64_t height);

    /// Per-tip pass: settle withdrawals whose broadcast tx has reached K confs.
    int tipChanged(uint64_t tip_height);

    /// Reorg path: ledger writes withdrawal_reverted, releasing the
    /// lock and returning the amount to spendable.
    void revert(const WithdrawalId& id);

    [[nodiscard]] WithdrawalState state(const WithdrawalId& id) const;
    [[nodiscard]] int outstandingDepth() const;
    [[nodiscard]] UnaAmount currentOutstanding(const AccountId& account) const;

   private:
    [[nodiscard]] bool isOutstanding(const WithdrawalState& s) const;
    [[nodiscard]] static LedgerTimestamp now();

    Ledger* ledger_{nullptr};
    SigningBackend* backend_{nullptr};
    WithdrawalCaps caps_;
    WithdrawalConfirmationPolicy policy_;
    DestinationValidatorFn destination_validator_;
    RequestIdGeneratorFn request_id_generator_;

    RequestMap requests_;
    StateMap states_;
};

}  // namespace dinero::vault
