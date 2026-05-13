/**
 * Minimal AssumeUTXORegistry stub for integration tests
 *
 * Returns nullopt for all snapshots (tests don't use AssumeUTXO).
 */

#include "consensus/assume_utxo_registry.h"

namespace dinero {
namespace consensus {

std::optional<SnapshotInfo> AssumeUTXORegistry::GetSnapshot(uint32_t /*height*/) {
    // For tests, return nullopt (no registered snapshots)
    return std::nullopt;
}

} // namespace consensus
} // namespace dinero
