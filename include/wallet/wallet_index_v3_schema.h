#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <sqlite3.h>

namespace dinero {

struct WalletIndexV3SchemaResult {
    std::filesystem::path rollback_backup;
    uint64_t quarantined_rows = 0;
    // Exact values authenticated by full backup/quarantine verification.
    // The open index handle pins these for later cheap transactional reads.
    std::string rollback_digest;
    std::string legacy_guard_digest;
};

// Probe an existing index without creating or upgrading it. A missing index
// returns nullopt; an unreadable or non-file target is a refusal. Legacy v2
// symlinks remain readable; authenticated v3 startup refuses a symlink before
// the legacy writer opens it.
std::optional<int64_t> ReadWalletIndexSchemaVersionReadOnly(
    const std::filesystem::path& index_file);

// Detect a published or interrupted v3 upgrade independently of user_version.
// A missing or damaged index must not turn a v3 source into a legacy startup,
// repair, or genesis-wipe candidate while its rollback copy still exists.
bool WalletIndexHasFullArchiveV3FootprintReadOnly(
    const std::filesystem::path& index_file,
    bool inspect_rollback_artifacts = true);

// Offline maintenance preflight. An imported snapshot index is not a
// canonical full-archive wallet source, even when its SQLite schema is v2.
// This opens the existing file read-only and never creates an index.
bool WalletIndexHasImportedSnapshotMarkerReadOnly(
    const std::filesystem::path& index_file);

// Full-archival only. This operation prepares a WAL-consistent, durable v2
// rollback backup before any v3 schema change. It does not attribute a legacy
// row to a wallet or create a wallet delivery receipt.
WalletIndexV3SchemaResult UpgradeFullArchiveWalletIndexSchemaToV3(
    const std::filesystem::path& index_file);

// Read-only verification for v3 consumers. Refuses v2 and damaged v3 sources;
// never performs an upgrade or changes the index/backup files.
WalletIndexV3SchemaResult RequireFullArchiveWalletIndexV3(
    const std::filesystem::path& index_file);
// Verify the exact SQLite connection that the caller will use, avoiding a
// validate-one-handle/use-another gap. Verification makes no writes; the
// caller owns the handle and controls whether it was opened read-only.
WalletIndexV3SchemaResult RequireFullArchiveWalletIndexV3(
    sqlite3* source, const std::filesystem::path& index_file);

// Cheap check for a connection already fully verified at open/reopen. Call
// inside a read transaction on that same connection while holding its owner
// mutex. Rechecks exact v3 table/guard definitions and durable markers;
// it does not rescan quarantined v2 rows or reopen the backup.
void RequireFullArchiveWalletIndexV3ReadGuards(sqlite3* source,
    std::string_view verified_rollback_digest,
    std::string_view verified_legacy_guard_digest);

// Chainstate-wide fatal reset only. Clears every v3 wallet namespace and
// receipt in one transaction, including empty-wallet receipts. It retains
// quarantined v2 rows and the rollback-backup marker for restart validation.
void ResetFullArchiveWalletIndexV3(const std::filesystem::path& index_file);

} // namespace dinero
