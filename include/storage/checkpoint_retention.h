#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/status.h"

namespace dinero {
class ChainDB;
class ChainWriteToken;

namespace storage {

struct CheckpointRetentionPolicy {
    uint32_t recent_blocks = 2000;
    uint32_t historical_interval = 5000;
    uint32_t replay_blocks_per_step = 16;
    uint32_t delete_heights_per_batch = 128;
    // Preserve caller-known import/lifecycle anchors and their predecessors
    // when the exact checkpoint is absent. The earliest anchor is automatic.
    std::vector<uint32_t> protected_heights;
};

enum class CheckpointRetentionPhase {
    Initialize, Ancestry, SelectInterval, VerifyInterval, DeleteInterval, Done
};

struct CheckpointRetentionProgress {
    CheckpointRetentionPhase phase = CheckpointRetentionPhase::Initialize;
    uint32_t interval_start = 0;
    uint32_t interval_end = 0;
    uint32_t replayed_height = 0;
    uint64_t replayed_blocks = 0;
    // Counts integer height slots, including absent keys; NOT reclaimed bytes
    // or an exact count of previously existing snapshots.
    uint64_t eligible_height_keys = 0;
    uint64_t deleted_height_keys = 0;
    uint64_t verified_intervals = 0;
    uint64_t skipped_intervals = 0;
    std::string last_skip_reason;
};

// First-stage retention engine for an EXCLUSIVELY OWNED OFFLINE DATABASE COPY.
// There is deliberately no daemon scheduling/configuration hook. Caller must
// prevent ALL other writers for the entire pass and supply lifecycle/import
// anchors. Frozen-tip and actual-anchor checks are additional fail-closed
// defenses, not a replacement for exclusivity or a concurrency protocol.
//
// Each interval keeps both ends and every delta. No interior U/C key can be
// deleted until complete linked-header, height-index and delta replay succeeds.
// Steps bound replay/ancestry heights or deletion pairs, NOT wall-clock latency.
// A crash loses only in-memory progress; the remaining anchors/deltas restore
// every deleted height. A new pass safely rechecks ranges after restart.
class CheckpointRetentionPass {
public:
    CheckpointRetentionPass(ChainDB& db, CheckpointRetentionPolicy policy, bool apply = false);
    ~CheckpointRetentionPass();
    CheckpointRetentionPass(const CheckpointRetentionPass&) = delete;
    CheckpointRetentionPass& operator=(const CheckpointRetentionPass&) = delete;

    Status step(const ChainWriteToken& token, std::string& error);
    const CheckpointRetentionProgress& progress() const;
    bool done() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace storage
} // namespace dinero
