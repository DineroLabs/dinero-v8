#pragma once
#include "wallet/transaction_signer.h"
#include "wallet/pending_payment.h"
#include <cstdint>
#include <string>
namespace dinero {
class WalletManager;
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
}
