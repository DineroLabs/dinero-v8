#pragma once
#include "wallet/transaction_signer.h"
#include "wallet/pending_payment.h"
#include <cstdint>
#include <string>
namespace dinero {
class WalletManager;
class ChainstateService;
namespace wallet { class OrchardAccountDelivery; }
// An identity snapshot, not a key or lasting authorization. Signing rechecks it.
struct WalletSigningIdentity {std::string name;uint64_t session=0;};
WalletSigningIdentity CaptureWalletSigningIdentity(WalletManager&,const std::string& requested_name);
// Caller resolves chain/index data before entry. No admission, broadcast or
// chain callbacks occur while the selected wallet/key owner is held here.
SignResult SignWalletTransaction(WalletManager&,const WalletSigningIdentity&,const UnsignedTransaction&);
// Resolve before selecting new coins. A returned body is a retained origin,
// never an acknowledgment of admission/confirmation or permission to rebroadcast.
// Exact request identity with different payload, unavailable storage or a stale
// selected-wallet session throws. This API never generates another transaction.
std::optional<PendingPayment> FindRetainedWalletPayment(
    WalletManager&, const WalletSigningIdentity&, const PendingPaymentIntent&);
// Internal preflight signing for an explicit request. Rechecks absence under
// the same key owner before signing and enforces its fee limit; no persistence
// or submission occurs. Final staging must still recheck and commit the request.
SignResult SignWalletRequestPreview(WalletManager&,const WalletSigningIdentity&,
                                   const UnsignedTransaction&,const PendingPaymentIntent&);
// A successful return requires the signed body and payment reservations to have
// committed under the same signing owner, before any external submission.
SignResult SignAndStageWalletPayment(WalletManager&,const WalletSigningIdentity&,
                                     const UnsignedTransaction&,const PendingPaymentIntent&);
// Internal bridge for the composite wallet owner. The public signing functions
// retain their existing transaction contract; no general borrowed-write API.
class WalletTransactionOwner {
    friend class wallet::OrchardAccountDelivery;
    friend class ChainstateService;
    // Only the selected service can establish the pre-activation branch.
    // Present declared/stored Orchard owners refuse without a replay source.
    static SignResult SignBeforeActivation(WalletManager&,const WalletSigningIdentity&,
                                          const UnsignedTransaction&,const PendingPaymentIntent&);
    friend SignResult SignWalletTransaction(WalletManager&,const WalletSigningIdentity&,const UnsignedTransaction&);
    friend SignResult SignWalletRequestPreview(WalletManager&,const WalletSigningIdentity&,const UnsignedTransaction&,const PendingPaymentIntent&);
    friend SignResult SignAndStageWalletPayment(WalletManager&,const WalletSigningIdentity&,const UnsignedTransaction&,const PendingPaymentIntent&);
    static SignResult Sign(WalletManager&,const WalletSigningIdentity&,const UnsignedTransaction&,
                           const PendingPaymentIntent*,bool retain,bool caller_transaction=false);
};

}
