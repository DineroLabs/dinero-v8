#include "daemon/runtime_block_reader.h"
#include "storage/archival_block_reader.h"
#include "consensus/orchard_profile.h"
#include "consensus/block_index.h"
#include "consensus/block_lifecycle.h"

namespace dinero {
std::vector<uint8_t> RuntimeBlockBody::Serialize() const {
    if (IsOrchardProfile()) return Orchard().WireBytes();
    const auto bytes = Historical().Serialize();
    return {bytes.begin(), bytes.end()};
}
StatusOr<RuntimeBlockBody> ReadRuntimeBlockUnderLock(
    const ChainDB& db, const BlockStorage* blocks, const uint256& hash, uint32_t height) {
    if (!blocks) return Status::Internal;
    if (!consensus::OrchardProfileConfigurationValid(Params())) return Status::Internal;
    const auto metadata = db.getHeaderMetadata(hash);
    if (!metadata.ok()) return metadata.status();
    if (metadata->height < 0 || uint32_t(metadata->height) != height) return Status::Corruption;
    if (!consensus::OrchardActiveForHeight(Params(), height)) {
        auto old = storage::ReadArchivalBlock(db, blocks, hash, storage::ArchivalReadMode::RequireFlatfiles);
        if (!old.ok()) return old.status();
        return RuntimeBlockBody(std::move(*old));
    }
    if (metadata->data_size == 0) return Status::NotFound;
    const auto header = db.getHeader(hash);
    if (!header.ok()) return header.status();
    if (header->GetHash() != hash || metadata->parent_hash != header->prev_block_hash)
        return Status::Corruption;
    const auto raw = blocks->readBlockBytes(
        FilePosition(metadata->file_number, metadata->data_pos, metadata->data_size));
    if (!raw.ok()) return raw.status();
    try {
        const auto body = OrchardBlockCandidate::DecodeExact(std::span(
            reinterpret_cast<const uint8_t*>(raw->data()), raw->size()));
        std::string error;
        const bool require_witness = Params().enforce_witness_commitment &&
            height >= Params().witness_commitment_enforcement_height;
        if (body.Header().GetHash() != hash || !body.Header().IsReservedValid() ||
            !body.CheckSizeLimits(error) || !body.CheckIdentityCommitments(require_witness, error))
            return Status::Corruption;
        const auto context = consensus::SelectedOrchardBlockContext(*header, height);
        if (!context) return Status::Internal;
        return RuntimeBlockBody(body, *context);
    } catch (const consensus::OrchardHeaderLookupError&) {
        return Status::Internal;
    } catch (const orchard::BackendError& error) {
        return error.Status() == DINERO_ORCHARD_PANIC ? Status::Internal : Status::Corruption;
    } catch (const std::invalid_argument&) {
        return Status::Corruption;
    }
}
std::shared_ptr<const RuntimeReorgPlan> ReadRuntimeReorgPlanUnderLock(
    const ChainDB& db, const BlockStorage* blocks, std::span<CBlockIndex* const> disconnect,
    std::span<CBlockIndex* const> connect, size_t byte_budget, size_t block_budget) {
    if (disconnect.empty() || disconnect.size() > block_budget ||
        connect.size() > block_budget - disconnect.size()) return {};
    std::vector<RuntimeReorgBlock> old_blocks, new_blocks;
    old_blocks.reserve(disconnect.size()); new_blocks.reserve(connect.size());
    size_t bytes = 0;
    auto read = [&](CBlockIndex* index, std::vector<RuntimeReorgBlock>& out) {
        if (!index || index->height < 0 || !(index->status & BLOCK_HAVE_DATA) ||
            (index->status & (BLOCK_FAILED_VALID | BLOCK_FAILED_CHILD))) return false;
        const auto metadata = db.getHeaderMetadata(index->hash);
        if (!metadata.ok() || metadata->status_flags != index->status ||
            metadata->file_number != index->file_number || metadata->data_pos != index->data_pos ||
            metadata->data_size != index->data_size || !metadata->data_size ||
            metadata->data_size > byte_budget - bytes ||
            metadata->chainwork.GetHex() != index->chainwork ||
            metadata->parent_hash != (index->pprev ? index->pprev->hash : uint256{})) return false;
        bytes += metadata->data_size;
        auto body = ReadRuntimeBlockUnderLock(db, blocks, index->hash, index->height);
        if (!body.ok()) return false;
        out.push_back({index->hash, uint32_t(index->height), std::move(*body)});
        return true;
    };
    CBlockIndex* next = disconnect.front();
    for (auto* index : disconnect) {
        if (index != next || !index || !index->pprev ||
            index->height != index->pprev->height + 1 || !read(index, old_blocks)) return {};
        next = index->pprev;
    }
    for (auto* index : connect) {
        if (!index || index->pprev != next || !next || index->height != next->height + 1 ||
            !read(index, new_blocks)) return {};
        next = index;
    }
    return std::make_shared<const RuntimeReorgPlan>(RuntimeReorgPlan{std::move(old_blocks), std::move(new_blocks)});
}
} // namespace dinero
