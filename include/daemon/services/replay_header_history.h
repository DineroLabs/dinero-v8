#pragma once

#include "consensus/header_chain.h"
#include "consensus/pow_context.h"
#include "wallet/runtime_replay_spool.h"
#include <algorithm>
#include <array>
#include <limits>
#include <thread>

namespace dinero::assumeutxo {
class AssumeUtxoReplayEngine;
// Private ascending replay ancestry. Each exact 128-byte header is authenticated
// in a disposable SQLite spool. There is no full-history map or parent-pointer
// graph. The bounded pager cache is not a whole-engine/RSS or disk-capacity claim.
// Shared historical header rules validate before coin effects; only the owning
// stateful replay engine can append after the same body validates. No live chain
// lookup, branch selection, backup, or persistent validity certificate is used.
class ReplayHeaderHistory final {
public:
    ReplayHeaderHistory() = default;
    ReplayHeaderHistory(const ReplayHeaderHistory&) = delete;
    ReplayHeaderHistory& operator=(const ReplayHeaderHistory&) = delete;

    bool Validate(const BlockHeader& candidate) const {
        CheckReadable();
        if (!seeded_) return candidate.prev_block_hash.IsNull() &&
            consensus::ValidateHistoricalHeader(candidate, std::nullopt);
        if (candidate.prev_block_hash != hash_ || height_ >= uint32_t(INT32_MAX)) return false;
        consensus::HeaderAsertContext context;
        context.parent_height = height_;
        // Legacy header MTP narrows each timestamp to uint32 BEFORE sorting.
        context.parent_mtp = Median<uint32_t>(height_);
        if (height_ >= 1) context.block1_time = static_cast<int64_t>(HeaderAt(1).timestamp);
        const auto consensus = GetConsensusForCurrentNetwork();
        if (const auto anchor = TimingUpgradeAnchorHeight(int32_t(height_) + 1, consensus)) {
            const auto header = HeaderAt(*anchor);
            context.timing_anchor = AsertAnchor{static_cast<int32_t>(*anchor),
                static_cast<int64_t>(header.timestamp), header.difficulty};
        }
        return consensus::ValidateHistoricalHeader(candidate, context);
    }

    std::optional<uint64_t> LockMedianTimePast(const uint256& parent, uint32_t wanted) const {
        CheckReadable();
        if (!seeded_ || parent != hash_ || wanted > height_) return std::nullopt;
        // Contextual transaction locks use the original 64-bit timestamp median.
        return Median<uint64_t>(wanted);
    }
    wallet::detail::RuntimeReplaySpool::Usage UsageNow() const {
        CheckReadable(); return spool_.UsageNow();
    }
private:
    friend class AssumeUtxoReplayEngine;
    friend struct ReplayHeaderHistoryTestAccess;
    using Spool = wallet::detail::RuntimeReplaySpool;
    static void Require(bool condition) {
        if (!condition) throw std::runtime_error("Replay header ancestry unavailable or inconsistent");
    }
    void CheckReadable() const {
        Require(thread_ == std::this_thread::get_id() && !poisoned_);
    }
    static Spool::Bytes Key(uint32_t height) {
        return {'h', uint8_t(height), uint8_t(height >> 8),
                uint8_t(height >> 16), uint8_t(height >> 24)};
    }
    BlockHeader HeaderAt(uint32_t height) const {
        Require(seeded_ && height <= height_);
        const auto bytes = spool_.Get(Key(height));
        Require(bytes && bytes->size() == 128);
        const auto header = BlockHeader::Deserialize(bytes->data(), bytes->size());
        Require(bool(header));
        if (height == height_) Require(header->GetHash() == hash_);
        return *header;
    }
    template<class Time> Time Median(uint32_t height) const {
        std::array<Time, 11> times{};size_t count = 0;
        auto header = HeaderAt(height);
        for (;;) {
            times[count++] = static_cast<Time>(header.timestamp);
            if (count == times.size() || height == 0) break;
            const auto parent = HeaderAt(--height);
            Require(header.prev_block_hash == parent.GetHash());
            header = parent;
        }
        std::sort(times.begin(), times.begin() + count);
        return times[count / 2];
    }
    // No spool mutex/transaction spans stateful proof or script validation.
    // Commit precedes position publication. An append failure poisons this
    // history; the owning engine also refuses all subsequent result access.
    void AppendValidated(const BlockHeader& header, uint32_t height) {
        CheckReadable(); poisoned_ = true;
        Require(height <= uint32_t(INT32_MAX) &&
            ((!seeded_ && height == 0 && header.prev_block_hash.IsNull()) ||
             (seeded_ && uint64_t(height_) + 1 == height && header.prev_block_hash == hash_)));
        const auto hash = header.GetHash(); const auto bytes = header.SerializeForHash();
        Require(!hash.IsNull() && bytes.size() == 128);
        Spool::Batch batch(spool_); spool_.Insert(Key(height), bytes); batch.Commit();
        height_ = height; hash_ = hash; seeded_ = true; poisoned_ = false;
    }
    Spool spool_;
    uint32_t height_ = 0;
    uint256 hash_;
    bool seeded_ = false, poisoned_ = false;
    const std::thread::id thread_ = std::this_thread::get_id();
};
} // namespace dinero::assumeutxo
