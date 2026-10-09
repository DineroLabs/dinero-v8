#pragma once
#include "rpc/rpc_registry.h"
#include "wallet/wallet_transaction_signer.h"
#include <memory>
namespace dinero { class WalletService;
// Internal owner-bound entry point for a durable external request. It accepts
// only the strict named request format and preserves the supplied wallet
// service/name/session through lookup, selection and signing. Caller releases
// vault/SQLite/key owners before entry. This does not certify a vault owner.
enum class WalletRequestDispatchMode { Submit, RetainOnly };
din::Json DispatchBoundWalletRequest(const ExecutionContext&,const din::Json&,
    const std::shared_ptr<WalletService>&,const WalletSigningIdentity&,
    WalletRequestDispatchMode = WalletRequestDispatchMode::Submit);
}
