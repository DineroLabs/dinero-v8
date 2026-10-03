#pragma once
#include "consensus/orchard_header.h"
#include "primitives/orchard_block_reader.h"
#include "common/status.h"
#include <variant>
#include <memory>
#include <span>

namespace dinero {
class ChainDB;
class BlockStorage;
struct CBlockIndex;

// A stored body selected by network/height, never by a transaction's claimed
// version. Reading authenticates identities, not consensus validity. There is
// deliberately no conversion of a mixed body into a historical Block.
class RuntimeBlockBody {
public:
    explicit RuntimeBlockBody(Block block) : body_(std::move(block)) {}
    RuntimeBlockBody(OrchardBlockCandidate block, consensus::OrchardBlockContext context)
        : body_(std::move(block)), context_(std::move(context)) {}
    bool IsOrchardProfile() const noexcept { return context_.has_value(); }
    const Block& Historical() const { return std::get<Block>(body_); }
    const OrchardBlockCandidate& Orchard() const { return std::get<OrchardBlockCandidate>(body_); }
    const auto& Context() const noexcept { return context_; }
    std::vector<uint8_t> Serialize() const;
private:
    const std::variant<Block, OrchardBlockCandidate> body_;
    const std::optional<consensus::OrchardBlockContext> context_;
};

// Complete exact bodies in execution order, including historical transactions
// across the boundary. Consumers can recover all bytes without rereading a body
// that may later be pruned. Identity authentication is not transaction admission.
struct RuntimeReorgBlock {
    uint256 hash;
    uint32_t height;
    RuntimeBlockBody body;
};
struct RuntimeReorgPlan {
    const std::vector<RuntimeReorgBlock> disconnect; // tip toward fork
    const std::vector<RuntimeReorgBlock> connect;    // fork toward new tip
};
// Operational memory bounds, NOT consensus or reorg-depth rules. Refuse the whole
// attempt if unavailable/over budget; never truncate or silently drop a body.
[[nodiscard]] std::shared_ptr<const RuntimeReorgPlan> ReadRuntimeReorgPlanUnderLock(
    const ChainDB&, const BlockStorage*, std::span<CBlockIndex* const> disconnect,
    std::span<CBlockIndex* const> connect, size_t byte_budget = 64 * 1024 * 1024,
    size_t block_budget = 2048);

// Strict indexed-flatfile read for daemon callers. The caller holds its selected
// chain lock and keeps Params fixed. Height is the selected index value, not a
// height supplied by a peer. Metadata must agree. No embedded-body fallback;
// no writes, validity flags, quarantine changes or proof verification.
[[nodiscard]] StatusOr<RuntimeBlockBody> ReadRuntimeBlockUnderLock(
    const ChainDB&, const BlockStorage*, const uint256&, uint32_t height);
} // namespace dinero
