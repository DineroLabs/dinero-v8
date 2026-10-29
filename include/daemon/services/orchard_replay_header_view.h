#pragma once
#include "consensus/orchard_header.h"
#include "daemon/services/orchard_history_capture.h"
#include <array>

namespace dinero {
// An ancestry reader for one private, ascending branch. Historical headers stay
// in the completed capture; appended headers use a separate authenticated spool.
// No all-history HeaderChainSelector or hash/height map is reconstructed here.
// The parent is independently validated by OrchardParentReplay; each appended
// header is checked by the unchanged common gate before the branch publishes it.
// This is a lookup owner, not a standalone consensus validator or readiness claim.
class OrchardReplayHeaderView final : public consensus::OrchardHeaderAncestry {
public:
    explicit OrchardReplayHeaderView(const OrchardHistoryCapture& history)
        : history_(history), base_(CheckedBase(history)), height_(base_),
          hash_(history.HeaderAt(base_).hash) {}
    OrchardReplayHeaderView(const OrchardReplayHeaderView&) = delete;
    OrchardReplayHeaderView& operator=(const OrchardReplayHeaderView&) = delete;

    std::optional<consensus::OrchardHeaderAncestor> GetHeaderValue(const uint256& hash) const override {
        CheckReadable();
        if(hash != hash_) return std::nullopt;
        const auto header=HeaderAt(height_);Require(header.GetHash()==hash_);
        // Return exactly the contextual gate's value fields. No raw parent
        // pointer or invented chainwork is exposed by this lookup interface.
        return consensus::OrchardHeaderAncestor{header,height_};
    }
    bool GetAncestorHashByHash(const uint256& hash,uint32_t height,
        uint256& out,uint32_t& parent_height) const override {
        CheckReadable();out.SetNull();parent_height=0;
        if(hash!=hash_)return false;
        parent_height=height_;if(height>height_)return false;
        out=HeaderAt(height).GetHash();return true;
    }
    bool GetAsertContextByHash(const uint256& hash,consensus::HeaderAsertContext& out,
        std::optional<uint32_t> anchor) const override {
        CheckReadable();out=consensus::HeaderAsertContext{};
        if(hash!=hash_ || (anchor && *anchor>height_))return false;
        consensus::HeaderAsertContext next;
        next.parent_height=height_;next.parent_mtp=MedianTimePast(height_);
        if(anchor) {
            const auto header=HeaderAt(*anchor);
            next.timing_anchor=AsertAnchor{static_cast<int32_t>(*anchor),
                static_cast<int64_t>(header.timestamp),header.difficulty};
        }
        if(height_>=1)next.block1_time=static_cast<int64_t>(HeaderAt(1).timestamp);
        out=next;return true;
    }
    uint32_t MedianTimePast(uint32_t height) const {
        CheckReadable();Require(height<=height_);
        std::array<uint32_t,11> times{};size_t count=0;
        auto header=HeaderAt(height);
        for(;;) {
            // Preserve HeaderIndexEntry::GetMedianTimePast's existing uint32
            // timestamp semantics, including its upper median on short chains.
            times[count++]=static_cast<uint32_t>(header.timestamp);
            if(count==times.size() || height==0)break;
            const auto parent=HeaderAt(--height);
            Require(header.prev_block_hash==parent.GetHash());header=parent;
        }
        std::sort(times.begin(),times.begin()+count);return times[count/2];
    }
    wallet::detail::RuntimeReplaySpool::Usage UsageNow() const {
        CheckReadable();return appended_.UsageNow();
    }
private:
    friend class OrchardBranchReplay;
    friend struct OrchardReplayHeaderViewTestAccess;
    using Spool=wallet::detail::RuntimeReplaySpool;
    static uint32_t CheckedBase(const OrchardHistoryCapture& history) {
        Require(history.Finished() && history.Count()>0 && history.Count()<=uint64_t(INT32_MAX));
        return uint32_t(history.Count()-1);
    }
    static void Require(bool ok) {
        if(!ok)throw consensus::OrchardHeaderLookupError("Orchard replay header ancestry unavailable");
    }
    void CheckReadable() const {
        Require(thread_==std::this_thread::get_id() && !poisoned_ && history_.Finished());
    }
    static Spool::Bytes Key(uint32_t height) {
        return {'b',uint8_t(height),uint8_t(height>>8),uint8_t(height>>16),uint8_t(height>>24)};
    }
    BlockHeader HeaderAt(uint32_t height) const {
        Require(height<=height_);
        if(height<=base_)return history_.HeaderAt(height).header;
        const auto bytes=appended_.Get(Key(height));Require(bytes && bytes->size()==128);
        const auto header=BlockHeader::Deserialize(bytes->data(),bytes->size());Require(bool(header));
        if(height==height_)Require(header->GetHash()==hash_);
        return *header;
    }
    // Only the full branch owner calls this after all header/body/state rules.
    // The checked commit precedes nonthrowing position publication. Failure
    // poisons this private view; no caller may reuse a partial append.
    void AppendHeader(const BlockHeader& header,uint32_t height) {
        CheckReadable();const bool usable=!finished_;poisoned_=true;
        const auto hash=header.GetHash();
        Require(usable && uint64_t(height_)+1==height && height<=uint32_t(INT32_MAX) &&
            header.prev_block_hash==hash_ && !hash.IsNull());
        const auto bytes=header.SerializeForHash();Require(bytes.size()==128);
        Spool::Batch batch(appended_);appended_.Insert(Key(height),bytes);batch.Commit();
        height_=height;hash_=hash;poisoned_=false;
    }
    void Freeze() {
        CheckReadable();const bool usable=!finished_;poisoned_=true;Require(usable);
        appended_.Freeze();finished_=true;poisoned_=false;
    }
    const OrchardHistoryCapture& history_;
    const uint32_t base_;
    Spool appended_;
    uint32_t height_;uint256 hash_;
    const std::thread::id thread_=std::this_thread::get_id();
    bool poisoned_=false,finished_=false;
};
} // namespace dinero
