#pragma once
#include "daemon/runtime_block_reader.h"
#include "daemon/runtime_block_outbox.h"
namespace dinero {
class ChainWriteToken;
struct RuntimeReorgIntent {
    RuntimeOutboxCursor cursor;
    uint256 previous_digest;
    RuntimeOutboxCursor outbox_origin;
    std::shared_ptr<const RuntimeReorgPlan> plan;
};
// Selected activation lock required. Persist the whole immutable typed plan
// synchronously BEFORE exposing reorg readiness. This is local recovery intent,
// not canonical state, validation certification or consumer acknowledgement.
// Failure permits no rollback. Records are retained even for cancelled attempts;
// zero callback counts do not authorize deletion. Provider recovery remains
// responsible for reconciling canonical state and idempotent consumer effects.
[[nodiscard]] RuntimeOutboxCursor PersistRuntimeReorgIntentUnderLock(
    ChainDB&, const ChainWriteToken&, const RuntimeReorgPlan&);
// One bounded intent at a time, after the durable consumer cursor (zero starts
// at origin). Current selected Params must match the stored network/profile.
// No deletion/ack API. An empty result means the exact checked head was reached.
[[nodiscard]] std::optional<RuntimeReorgIntent> ReadRuntimeReorgIntentUnderLock(
    const ChainDB&, RuntimeOutboxCursor after = {});
} // namespace dinero
