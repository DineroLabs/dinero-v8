// The enabled implementation belongs to dinero_orchard_chainstate_write.
// Keep the public capture entry point available in a genuine backend-OFF
// build so callers receive an explicit refusal rather than a link failure.
#ifndef DINERO_HAS_ORCHARD_RUNTIME_READER
#include "daemon/orchard_chainstate_write.h"
#include <stdexcept>

namespace dinero {
OrchardPoolCoinView OrchardCompactChainstate::CapturePoolCoinsUnderLock(
    AnnotatedRecursiveMutex&, const consensus::OrchardTransactionContext&,
    const BlockHeader&, std::span<const MempoolProofView>) const {
    throw std::runtime_error("Orchard compact owner backend unavailable");
}
} // namespace dinero
#endif
