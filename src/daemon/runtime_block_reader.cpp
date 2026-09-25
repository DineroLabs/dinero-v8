#include "daemon/runtime_block_reader.h"
#include "storage/archival_block_reader.h"
#include "consensus/orchard_profile.h"

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
} // namespace dinero
