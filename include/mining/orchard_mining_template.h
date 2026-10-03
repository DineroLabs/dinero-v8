#pragma once
#include "daemon/mempool_transaction.h"
#include "primitives/block.h"
#include <memory>
#include <span>
#include <stdexcept>

namespace dinero {
class ChainstateService;
// Immutable mining body built under the selected full-chain owner. This is a
// template, not proof of work, admission, or lasting selected-chain readiness.
class OrchardMiningTemplate {
public:
    const BlockHeader& Header() const noexcept { return header_; }
    const auto& Transactions() const noexcept { return transactions_; }
    const auto& WireBytes() const noexcept { return bytes_; }
    uint32_t Height() const noexcept { return height_; }
    uint64_t TotalFees() const noexcept { return fees_; }
    size_t BaseSize() const noexcept { return base_size_; }
    size_t Weight() const noexcept { return 3 * base_size_ + bytes_.size(); }
private:
    friend class ChainstateService;
    OrchardMiningTemplate(BlockHeader header, std::vector<MempoolTransaction> transactions,
        std::vector<uint8_t> bytes, uint32_t height, uint64_t fees, size_t base_size)
        : header_(header), transactions_(std::move(transactions)), bytes_(std::move(bytes)),
          height_(height), fees_(fees), base_size_(base_size) {}
    const BlockHeader header_;
    const std::vector<MempoolTransaction> transactions_;
    const std::vector<uint8_t> bytes_;
    const uint32_t height_;
    const uint64_t fees_;
    const size_t base_size_;
};
class MiningTemplateSizeError : public std::runtime_error {
public:
    MiningTemplateSizeError() : std::runtime_error("Complete mining template exceeds size or weight") {}
};
// Acquired before pool access and retained through complete construction. The
// default supports historical callers only; typed construction must be owned.
class MiningChainstateReadGuard {
public:
    virtual ~MiningChainstateReadGuard() = default;
    virtual std::shared_ptr<const OrchardMiningTemplate> BuildOrchardTemplate(
        const BlockHeader&, const Transaction&, std::span<const MempoolTransaction>, uint32_t) {
        return {};
    }
};
} // namespace dinero
