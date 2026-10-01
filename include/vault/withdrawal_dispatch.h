#pragma once
#include "vault/withdrawal_queue.h"
#include <memory>
#include <optional>
#include <stdexcept>
namespace dinero::vault {
class VaultService;
struct VaultWithdrawalQuery {
    WithdrawalRequest request;
    WithdrawalPaymentRetained retained;
    std::optional<AllocationInclusion> prior;
    bool operator==(const VaultWithdrawalQuery&) const = default;
};
struct VaultWithdrawalObservation {
    VaultWithdrawalQuery query;
    std::optional<AllocationInclusion> included;
    // True only after the exact old height/hash is proven off the selected
    // chain. Missing optional transaction-index data is never absence proof.
    bool prior_not_canonical{false};
};
// Keeps source lifetime and selected-chain ownership through the wallet FULL
// transaction and service publication. Destroy on the acquiring thread after
// releasing wallet/service owners. No wallet callbacks run under those owners.
class VaultWithdrawalObservationLease {
public:
    virtual ~VaultWithdrawalObservationLease()=default;
    virtual uint64_t Height() const noexcept=0;
    virtual const std::array<uint8_t,32>& Hash() const noexcept=0;
    virtual const std::vector<VaultWithdrawalObservation>& Rows() const noexcept=0;
};

// Only the durable service may start a new dispatch, immediately after it
// commits Pending -> Signing in this call. Reopened Signing requests resolve
// existing wallet bodies only. No public retry API authorizes regeneration.
class VaultWithdrawalDispatcher {
public:
    virtual ~VaultWithdrawalDispatcher()=default;
private:
    friend class VaultService;
    virtual std::optional<WithdrawalPaymentRetained> DispatchNew(const WithdrawalRequest&)=0;
    virtual std::optional<WithdrawalPaymentRetained> Resolve(const WithdrawalRequest&)=0;
    virtual std::unique_ptr<VaultWithdrawalObservationLease> CaptureCanonical(
        uint64_t,const std::vector<VaultWithdrawalQuery>&) {
        throw std::runtime_error("vault canonical withdrawal owner unavailable");
    }
};
} // namespace dinero::vault
