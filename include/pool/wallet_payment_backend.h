#pragma once
#include "pool/payment_attempt.h"
struct DaemonContext;
namespace dinero::pool {
// Exclusive to the processor, which drains all operations before destruction
// of the referenced daemon context. No operation is exposed to retained callers.
std::unique_ptr<PoolPaymentBackend> MakeWalletPoolPaymentBackend(DaemonContext&);
}
