/**
 * Minimal ChainDB stub for integration tests (UTXOSet with nullptr backend)
 *
 * These implementations should NEVER be called since we use UTXOSet(nullptr).
 * They exist only to satisfy the linker.
 *
 * If any of these are called, it means the test is wrong (not using nullptr mode).
 */

#include "storage/chain_db.h"
#include "common/status.h"
#include "primitives/uint256.h"
#include "rocksdb/write_batch.h"
#include <stdexcept>

namespace dinero {

// These should NEVER be called (UTXOSet checks chain_db_ != nullptr first)
Status ChainDB::putCoin(const ChainWriteToken&, const uint256&, uint32_t,
                        const Coin&, rocksdb::WriteBatch*) {
    throw std::runtime_error("ChainDB::putCoin stub called - test should use UTXOSet(nullptr)");
}

Status ChainDB::deleteCoin(const ChainWriteToken&, const uint256&, uint32_t,
                           rocksdb::WriteBatch*) {
    throw std::runtime_error("ChainDB::deleteCoin stub called - test should use UTXOSet(nullptr)");
}

StatusOr<Coin> ChainDB::getCoin(const uint256&, uint32_t) const {
    throw std::runtime_error("ChainDB::getCoin stub called - test should use UTXOSet(nullptr)");
}

StatusOr<TipInfo> ChainDB::getTip() const {
    throw std::runtime_error("ChainDB::getTip stub called - test should use UTXOSet(nullptr)");
}

Status ChainDB::forEachUTXO(std::function<bool(const uint256&, uint32_t, const Coin&)>) const {
    throw std::runtime_error("ChainDB::forEachUTXO stub called - test should use UTXOSet(nullptr)");
}

} // namespace dinero
