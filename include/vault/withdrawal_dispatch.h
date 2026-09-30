#pragma once
#include "vault/withdrawal_queue.h"
#include <memory>
#include <optional>
namespace dinero::vault {
class VaultService;
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
};
} // namespace dinero::vault
