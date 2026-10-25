#pragma once
// Automatic shielded-state storage upgrade before the daemon opens ChainDB.
//
// Orchard reads and writes its state only in the separated layout (the
// shielded_state_v1 column family; storage/orchard_state.cpp refuses any
// other store), but nothing produced that layout outside the qualification
// tool tools/migrate_shielded_datadir. A node that was not migrated by hand
// could neither mine nor accept the first Orchard block, so every node on the
// network would stop at activation.
//
// This runs the reviewed engine (MigrateShieldedStateCopy, unchanged) on a
// copy of the ChainDB directory, the same way the Orchard reindex path does
// (daemon/orchard_reindex.cpp), and swaps it in:
//   <chaindb>                     the store the daemon opens
//   <chaindb>.upgrade-candidate   copy being migrated
//   <chaindb>.legacy-v1           untouched original, kept for rollback
//   <chaindb>.upgrade-state       journal: copying | migrated | swapping
// Every crash point has one recovery: copying -> discard the candidate and
// start over; migrated -> swap; swapping -> finish the renames.
//
// The caller holds the datadir lock and has not opened ChainDB yet, so the
// engine can take the store's own LOCK. DaemonApp's native and embedded hosts
// opt in under the same real DatadirGuard (SetAutomaticStorageUpgrade).
#include "consensus/chainparams.h"
#include "daemon/chainstate_recovery_marker.h"
#include "storage/shielded_migration.h"

#include <rocksdb/db.h>
#include <rocksdb/options.h>

#include <cstdint>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace dinero::daemon {

enum class StorageUpgradeOutcome { NotNeeded, Upgraded, Refused };
struct StorageUpgradeReport {
    StorageUpgradeOutcome outcome = StorageUpgradeOutcome::NotNeeded;
    std::string detail;
};

namespace storage_upgrade_detail {
namespace fs = std::filesystem;

// Column families of an existing store, read without opening it. -1: no store
// or unreadable.
inline int FamilyCount(const fs::path& db) {
    std::vector<std::string> names;
    const auto status = rocksdb::DB::ListColumnFamilies(rocksdb::DBOptions(), db.string(), &names);
    return status.ok() ? static_cast<int>(names.size()) : -1;
}

inline std::uintmax_t TreeBytes(const fs::path& root, bool count_sst = true) {
    std::uintmax_t total = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && (count_sst || it->path().extension() != ".sst"))
            total += it->file_size(ec);
    if (ec) throw fs::filesystem_error("cannot measure ChainDB", root, ec);
    return total;
}

inline void SyncPath(const fs::path& path) {
#ifndef _WIN32
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) throw std::system_error(errno, std::generic_category(), "open for storage-upgrade sync");
    if (::fsync(fd) != 0) {
        const int error = errno;
        ::close(fd);
        throw std::system_error(error, std::generic_category(), "sync storage-upgrade path");
    }
    if (::close(fd) != 0)
        throw std::system_error(errno, std::generic_category(), "close storage-upgrade path");
#else
    (void)path;
#endif
}

inline void WriteJournal(const fs::path& journal, const std::string& state) {
    const auto tmp = fs::path(journal.string() + ".tmp");
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << state << "\n";
        out.flush();
        if (!out) throw std::runtime_error("cannot write " + tmp.string());
    }
    SyncPath(tmp);
    fs::rename(tmp, journal);
    SyncPath(journal.parent_path());
}

inline std::string ReadJournal(const fs::path& journal) {
    std::ifstream in(journal);
    std::string state;
    in >> state;
    return state;
}

inline bool CanHardlinkInCandidate(const fs::path& candidate) {
    // A crash before the journal is written leaves only this candidate; the
    // ordinary recovery path removes it on the next start.
    fs::create_directory(candidate);
    const auto first = candidate / ".upgrade-link-probe";
    const auto second = candidate / ".upgrade-link-probe-pair";
    { std::ofstream out(first); out << "probe"; if (!out) throw std::runtime_error("cannot probe hardlink support"); }
    std::error_code ec;
    fs::create_hard_link(first, second, ec);
    const bool supported = !ec;
    fs::remove(second);
    fs::remove(first);
    return supported;
}

inline void CopyTree(const fs::path& from, const fs::path& to, bool link_sst) {
    fs::create_directories(to);
    for (const auto& entry : fs::recursive_directory_iterator(from))
        if (entry.is_symlink() || !(entry.is_regular_file() || entry.is_directory()))
            throw std::runtime_error("unexpected entry in ChainDB: " + entry.path().string());
    for (const auto& entry : fs::recursive_directory_iterator(from)) {
        const auto target = to / entry.path().lexically_relative(from);
        if (entry.is_directory()) { fs::create_directories(target); continue; }
        if (link_sst && entry.path().extension() == ".sst") {
            if (fs::hard_link_count(entry.path()) != 1)
                throw std::runtime_error("source SST already hardlinked: " + entry.path().string());
            fs::create_hard_link(entry.path(), target);
        } else {
            fs::copy_file(entry.path(), target);
            SyncPath(target);
        }
    }
    SyncPath(to);
}
}  // namespace storage_upgrade_detail

// A brand-new datadir: no store and no trace of an upgrade. Mid-swap the store
// directory is briefly absent, which must not be mistaken for a new install.
inline bool IsBrandNewStore(const std::filesystem::path& chaindb) {
    namespace fs = std::filesystem;
    for (const char* suffix : {"", ".upgrade-candidate", ".upgrade-state", ".legacy-v1"})
        if (fs::exists(fs::path(chaindb.string() + suffix))) return false;
    return true;
}

// after_phase is a test hook: it is called after each durable step ("copied",
// "migrated", "legacy-retired", "swapped") and may throw to simulate a crash.
// available_bytes replaces the measured free space (tests only).
inline StorageUpgradeReport EnsureSeparatedShieldedLayout(
    const std::filesystem::path& datadir, const std::filesystem::path& chaindb,
    const std::function<void(const char*)>& after_phase = {},
    std::optional<std::uintmax_t> available_bytes = std::nullopt) {
    namespace fs = std::filesystem;
    using namespace storage_upgrade_detail;
    const auto phase = [&](const char* name) { if (after_phase) after_phase(name); };
    const fs::path candidate = chaindb.string() + ".upgrade-candidate";
    const fs::path legacy = chaindb.string() + ".legacy-v1";
    const fs::path journal = chaindb.string() + ".upgrade-state";
    const auto refuse = [](std::string why) { return StorageUpgradeReport{StorageUpgradeOutcome::Refused, std::move(why)}; };

    // 1. Finish or discard an interrupted upgrade.
    std::string state = fs::exists(journal) ? ReadJournal(journal) : std::string();
    if (!state.empty() && state != "copying" && state != "migrated" && state != "swapping")
        return refuse("unknown storage-upgrade journal state");
    if (state == "swapping") {
        const bool live_exists = fs::exists(chaindb);
        const bool candidate_exists = fs::exists(candidate);
        const bool legacy_exists = fs::exists(legacy);
        // The live name is absent between the two renames. Never turn a lost
        // candidate into an apparently successful upgrade: ChainDB::init would
        // otherwise create a fresh, empty database in its place.
        if (live_exists && candidate_exists && !legacy_exists) {
            if (FamilyCount(chaindb) != 9 || FamilyCount(candidate) != 10)
                return refuse("interrupted swap has unexpected storage layouts");
            fs::rename(chaindb, legacy);
            fs::rename(candidate, chaindb);
        } else if (!live_exists && candidate_exists && legacy_exists) {
            if (FamilyCount(candidate) != 10 || FamilyCount(legacy) != 9)
                return refuse("interrupted swap has unexpected storage layouts");
            fs::rename(candidate, chaindb);
        } else if (!(live_exists && !candidate_exists && legacy_exists &&
                     FamilyCount(chaindb) == 10 && FamilyCount(legacy) == 9)) {
            return refuse("interrupted swap is missing or has conflicting storage directories");
        }
        SyncPath(chaindb.parent_path());
        fs::remove(journal);
        return {StorageUpgradeOutcome::Upgraded, "completed an interrupted swap"};
    }
    if (!fs::exists(chaindb) && (fs::exists(candidate) || fs::exists(legacy) || fs::exists(journal)))
        return refuse("live ChainDB missing during storage upgrade");
    if (state == "migrated" && (!fs::exists(candidate) || fs::exists(legacy)))
        return refuse("migrated candidate missing or rollback directory already exists");
    if (state != "migrated" && fs::exists(candidate)) {
        fs::remove_all(candidate);  // partial copy or partial migration: start over
        fs::remove(journal);
    }

    if (state != "migrated") {
        // 2. Is there anything to do?
        if (!fs::exists(chaindb)) return {StorageUpgradeOutcome::NotNeeded, "no store yet"};
        const int families = FamilyCount(chaindb);
        if (families == 10) return {StorageUpgradeOutcome::NotNeeded, "already separated"};
        if (families != 9) return refuse("unexpected ChainDB column-family count " + std::to_string(families));
        if (fs::exists(legacy))
            return refuse(legacy.string() + " already exists; move it away to upgrade again");

        // 3. Never migrate a store another operation owns or left unfinished.
        for (const auto& marker : {ChainstateRecoveryMarkerPath(datadir),
                                   datadir / "blockchain" / "reindex_promotion.marker",
                                   fs::path(chaindb.string() + ".reindex.tmp")})
            if (fs::exists(marker)) return refuse("unfinished operation marker present: " + marker.string());

        // 4. Copy only mutable files; immutable SSTs can share disk blocks
        // with the rollback store when the filesystem supports hardlinks.
        const auto total_bytes = TreeBytes(chaindb);
        const auto mutable_bytes = TreeBytes(chaindb, false);
        std::error_code ec;
        const auto space = fs::space(chaindb.parent_path(), ec);
        const std::uintmax_t available = available_bytes ? *available_bytes : (ec ? 0 : space.available);
        const auto linked_need = mutable_bytes + total_bytes / 10 + (512ULL << 20);
        if ((ec && !available_bytes) || available < linked_need)
            return refuse("not enough free disk for the storage upgrade: need " + std::to_string(linked_need >> 20) +
                          " MiB, have " + std::to_string(available >> 20) + " MiB");
        const bool link_sst = CanHardlinkInCandidate(candidate);
        const auto need = link_sst ? linked_need : total_bytes / 10 * 11 + (512ULL << 20);
        if ((ec && !available_bytes) || available < need)
        {
            fs::remove_all(candidate);
            return refuse("not enough free disk for the storage upgrade: need " + std::to_string(need >> 20) +
                          " MiB, have " + std::to_string(available >> 20) + " MiB");
        }

        // 5. Copy, then migrate the copy with the reviewed engine.
        WriteJournal(journal, "copying");
        CopyTree(chaindb, candidate, link_sst);
        phase("copied");
        // Same bounded limits as the Orchard reindex path.
        constexpr uint64_t record_bytes = 32ULL * 1024 * 1024;
        const storage::ShieldedMigrationLimits limits{record_bytes, 4096, record_bytes, 10000000};
        const auto result = storage::MigrateShieldedStateCopy(chaindb, candidate, limits, true, {}, link_sst);
        if (!result.ok || !result.ready) {
            std::error_code ignored;
            fs::remove_all(candidate, ignored);
            fs::remove(journal, ignored);
            return refuse("shielded-state migration refused: " + result.error);
        }
        WriteJournal(journal, "migrated");
        phase("migrated");
    }

    // 6. Swap: keep the original as the rollback copy.
    WriteJournal(journal, "swapping");
    fs::rename(chaindb, legacy);
    phase("legacy-retired");
    fs::rename(candidate, chaindb);
    SyncPath(chaindb.parent_path());
    fs::remove(journal);
    phase("swapped");
    return {StorageUpgradeOutcome::Upgraded, "legacy store kept at " + legacy.string()};
}

}  // namespace dinero::daemon
