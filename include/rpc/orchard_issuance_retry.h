#pragma once
#include "wallet/orchard_account_delivery.h"

namespace dinero::rpc::detail {
// The operation must roll back every CatalogChanged refusal before returning
// control here. Each attempt must recapture the source and durable owners while
// retaining the request's original wallet session. Other failures propagate.
template<class Attempt>
auto RetryOrchardIssuance(Attempt&& attempt) {
    for (unsigned tries=0;;++tries) {
        try { return attempt(); }
        catch (const wallet::OrchardAccountDelivery::CatalogChanged&) {
            if (tries==3) throw;
        }
    }
}
} // namespace dinero::rpc::detail
