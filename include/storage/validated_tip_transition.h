#pragma once
#include "storage/chain_db.h"
#include "storage/chain_write_token.h"
#include <rocksdb/write_batch.h>

namespace dinero::storage {
// Move an existing execution marker with a transition already validated by the
// caller. A behind/absent/other-branch marker is not a certificate for assumed
// history: leave it unchanged. The caller commits this in its canonical batch.
inline Status StageMatchingValidatedTip(ChainDB& db, const ChainWriteToken& token,
    const uint256& from_hash, int from_height, const uint256& to_hash, int to_height,
    rocksdb::WriteBatch& batch) {
    const auto validated = db.getValidatedTip();
    if (validated.status() == Status::NotFound) return Status::Ok;
    if (!validated.ok()) return validated.status();
    if (validated->hash != from_hash || validated->height != from_height) return Status::Ok;
    return db.setValidatedTip(token, to_hash, to_height, &batch);
}
} // namespace dinero::storage
