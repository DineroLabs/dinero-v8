#pragma once
#include "consensus/orchard_header.h"
#include "primitives/orchard_block_reader.h"
#include "common/status.h"
#include <variant>

namespace dinero {
class ChainDB;
class BlockStorage;

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

// Strict indexed-flatfile read for daemon callers. The caller holds its selected
// chain lock and keeps Params fixed. Height is the selected index value, not a
// height supplied by a peer. Metadata must agree. No embedded-body fallback;
// no writes, validity flags, quarantine changes or proof verification.
[[nodiscard]] StatusOr<RuntimeBlockBody> ReadRuntimeBlockUnderLock(
    const ChainDB&, const BlockStorage*, const uint256&, uint32_t height);
} // namespace dinero
