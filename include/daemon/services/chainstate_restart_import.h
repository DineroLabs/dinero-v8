#pragma once

#include "consensus/block_index.h"
#include "storage/chain_db.h"
#include "primitives/uint256.h"

namespace dinero {

/**
 * Rehydrate persisted status/disk metadata for a block index entry recreated
 * from header-only state during restart. This is needed for stored non-active
 * branch blocks that are not reachable from the active height index.
 */
inline bool RestorePersistedBlockIndexMetadata(const ChainDB& chain_db,
                                               const uint256& hash,
                                               CBlockIndex* block_index) {
    if (!block_index) {
        return false;
    }

    auto metadata_result = chain_db.getHeaderMetadata(hash);
    if (metadata_result.status() != Status::Ok) {
        return false;
    }

    const auto& metadata = metadata_result.value();
    block_index->status = metadata.status_flags;
    block_index->file_number = metadata.file_number;
    block_index->data_pos = metadata.data_pos;
    block_index->data_size = metadata.data_size;
    block_index->undo_file = metadata.undo_file;
    block_index->undo_pos = metadata.undo_pos;
    block_index->undo_size = metadata.undo_size;
    return true;
}

}  // namespace dinero
