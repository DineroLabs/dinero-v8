#include "daemon/relay_transaction_reader.h"
#include <stdexcept>

// These isolated address/proof fixtures already stub persistence and do not
// exercise file recovery. Unexpected decoding must refuse, never fabricate a
// transaction. Real persistence fixtures link the production decoder.
namespace dinero {
MempoolTransaction DecodeRelayTransaction(
    std::span<const uint8_t>, RelayTransactionReadMode) {
    throw std::runtime_error("Relay decoding unavailable in isolated mempool fixture");
}
}
