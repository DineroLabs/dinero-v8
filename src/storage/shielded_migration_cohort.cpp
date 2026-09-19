#include "storage/shielded_migration_cohort.h"
#include "shielded_migration_internal.h"
#include "shielded_migration_metadata.h"
#include "crypto/sha256.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <map>
#include <stdexcept>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace dinero::storage {
namespace {
namespace fs = std::filesystem;
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Fd {
    int value = -1;
    ~Fd() { if (value >= 0) ::close(value); }
    Fd() = default;
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};
struct Digest {
    crypto::CSHA256 hash;
    void Field(std::string_view value) {
        uint8_t bytes[8]; const uint64_t size = value.size();
        for (unsigned i = 0; i < 8; ++i) bytes[i] = static_cast<uint8_t>(size >> (8 * i));
        hash.Write(bytes, sizeof(bytes)); hash.Write(reinterpret_cast<const uint8_t*>(value.data()), value.size());
    }
    std::string Finish() { return crypto::bytes_to_hex(hash.Finalize()); }
};
struct Stamp {
    struct stat value{};
    static Stamp Path(const fs::path& path) {
        Stamp result; Require(::lstat(path.c_str(), &result.value) == 0, "cannot stat companion path"); return result;
    }
    static Stamp File(int fd) {
        Stamp result; Require(::fstat(fd, &result.value) == 0, "cannot stat companion descriptor"); return result;
    }
    bool Identity(const Stamp& other) const {
        return value.st_dev == other.value.st_dev && value.st_ino == other.value.st_ino &&
            value.st_mode == other.value.st_mode && value.st_uid == other.value.st_uid && value.st_nlink == other.value.st_nlink;
    }
    bool Unchanged(const Stamp& other) const {
#if defined(__APPLE__)
        const auto m = value.st_mtimespec, n = other.value.st_mtimespec;
        const auto c = value.st_ctimespec, d = other.value.st_ctimespec;
#else
        const auto m = value.st_mtim, n = other.value.st_mtim;
        const auto c = value.st_ctim, d = other.value.st_ctim;
#endif
        return Identity(other) && value.st_size == other.value.st_size &&
            m.tv_sec == n.tv_sec && m.tv_nsec == n.tv_nsec && c.tv_sec == d.tv_sec && c.tv_nsec == d.tv_nsec;
    }
    bool Regular() const { return S_ISREG(value.st_mode) && value.st_nlink == 1; }
};
bool Nested(const fs::path& parent, const fs::path& child) {
    auto a = parent.begin(), b = child.begin();
    for (; a != parent.end() && b != child.end(); ++a, ++b) if (*a != *b) return false;
    return a == parent.end();
}
bool Present(const fs::path& path) {
    std::error_code error; const auto status = fs::symlink_status(path, error);
    Require(!error || error == std::errc::no_such_file_or_directory, "cannot inspect maintenance barrier");
    return status.type() != fs::file_type::not_found;
}
fs::path CanonicalDirectory(const fs::path& input) {
    const auto absolute = fs::absolute(input); fs::path cursor;
    for (const auto& part : absolute) { cursor /= part; Require(!fs::is_symlink(fs::symlink_status(cursor)), "symlink in datadir path"); }
    const auto path = fs::canonical(absolute); const auto stamp = Stamp::Path(path);
    Require(S_ISDIR(stamp.value.st_mode) && stamp.value.st_uid == geteuid(), "datadir must be an owned directory");
    return path;
}
class Lease {
    Fd lock_;
    Stamp root_, lock_stamp_;
public:
    const fs::path path;
    explicit Lease(const fs::path& input) : path(CanonicalDirectory(input)) {
        root_ = Stamp::Path(path);
        lock_stamp_ = Stamp::Path(path / "dinerod.lock");
        Require(lock_stamp_.Regular(), "existing unshared daemon lock required");
        // Same lock as DatadirGuard, but no O_CREAT, PID writes or PID removal.
        lock_.value = ::open((path / "dinerod.lock").c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        Require(lock_.value >= 0 && lock_stamp_.Identity(Stamp::File(lock_.value)), "daemon lock identity changed");
        Require(::flock(lock_.value, LOCK_EX | LOCK_NB) == 0, "datadir is owned by another operation");
        Recheck();
    }
    void Recheck() const {
        Require(root_.Identity(Stamp::Path(path)) && fs::canonical(path) == path, "datadir identity changed");
        Require(lock_stamp_.Identity(Stamp::Path(path / "dinerod.lock")) &&
                lock_stamp_.Identity(Stamp::File(lock_.value)), "daemon lock replaced");
        Require(!Present(path / "chainstate_recovery.marker") &&
                !Present(path / "blockchain/reindex_promotion.marker") &&
                !Present(path.parent_path() / ".nodecore-maintenance-v1"), "pending recovery or maintenance barrier");
    }
    void Bind(Digest& digest) const {
        digest.Field(path.string()); digest.Field(std::to_string(root_.value.st_dev)); digest.Field(std::to_string(root_.value.st_ino));
        digest.Field(std::to_string(lock_stamp_.value.st_dev)); digest.Field(std::to_string(lock_stamp_.value.st_ino));
    }
};
bool EndsWith(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}
struct Entry { Stamp stamp; bool missing = false; std::string digest; };
struct Inventory {
    std::map<std::string, Entry> entries;
    uint64_t bytes = 0;
    std::string digest;
};
std::string HashFile(const fs::path& path, const Stamp& expected) {
    Fd input; input.value = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    Require(input.value >= 0 && expected.Unchanged(Stamp::File(input.value)), "companion changed before read");
    crypto::CSHA256 hash; std::array<uint8_t, 65536> buffer{}; uint64_t remaining = expected.value.st_size;
    while (remaining) {
        const auto count = ::read(input.value, buffer.data(), std::min<uint64_t>(buffer.size(), remaining));
        if (count < 0 && errno == EINTR) continue;
        Require(count > 0, "companion read failed or truncated"); hash.Write(buffer.data(), count); remaining -= count;
    }
    Require(expected.Unchanged(Stamp::File(input.value)) && expected.Unchanged(Stamp::Path(path)), "companion changed during read");
    return crypto::bytes_to_hex(hash.Finalize());
}
Inventory Capture(const Lease& lease, const ShieldedCompanionLimits& limits, bool hash) {
    lease.Recheck(); Inventory inventory;
    auto add = [&](const fs::path& path) {
        Require(inventory.entries.size() < limits.max_entries, "companion entry budget exceeded");
        const auto key = path.lexically_relative(lease.path).generic_string(); Entry entry;
        if (!Present(path)) entry.missing = true;
        else {
            entry.stamp = Stamp::Path(path);
            Require(S_ISDIR(entry.stamp.value.st_mode) || entry.stamp.Regular(), "nonregular or shared companion");
            Require(key.find(".reindex.tmp") == std::string::npos, "unfinished reindex companion");
            if (entry.stamp.Regular()) {
                Require(entry.stamp.value.st_size >= 0 && uint64_t(entry.stamp.value.st_size) <= limits.max_bytes - inventory.bytes,
                        "companion byte budget exceeded");
                inventory.bytes += entry.stamp.value.st_size;
                Require(!(EndsWith(key, "-wal") || EndsWith(key, "-journal")) || entry.stamp.value.st_size == 0,
                        "SQLite companion requires clean shutdown/checkpoint");
                if (hash) entry.digest = HashFile(path, entry.stamp);
            }
        }
        inventory.entries.emplace(key, std::move(entry));
    };
    for (const char* component : {"blockchain", "blocks", "headers", "checkpoints"}) {
        const auto root = lease.path / component; add(root);
        const auto& entry = inventory.entries.at(component);
        if (entry.missing) { Require(std::string_view(component) != "blockchain", "missing blockchain directory"); continue; }
        Require(S_ISDIR(entry.stamp.value.st_mode), "companion root must be a directory");
        for (fs::recursive_directory_iterator it(root), end; it != end; ++it) {
            if (it->path() == lease.path / "blockchain/chaindb") { it.disable_recursion_pending(); continue; }
            add(it->path());
        }
    }
    Digest digest; digest.Field("chain-companion-files-v1");
    for (const auto& [name, entry] : inventory.entries) {
        digest.Field(name); digest.Field(entry.missing ? "missing" : entry.stamp.Regular() ? "file" : "directory");
        if (!entry.missing && entry.stamp.Regular()) { digest.Field(std::to_string(entry.stamp.value.st_size)); digest.Field(entry.digest); }
    }
    inventory.digest = digest.Finish(); lease.Recheck(); return inventory;
}
void Verify(const Inventory& expected, const Inventory& actual, bool hashed) {
    Require(expected.entries.size() == actual.entries.size(), "companion membership changed");
    auto a = expected.entries.begin(), b = actual.entries.begin();
    for (; a != expected.entries.end(); ++a, ++b) {
        Require(a->first == b->first && a->second.missing == b->second.missing, "companion membership changed");
        if (a->second.missing) continue;
        // Directory timestamps are not stable across inner RocksDB LOG opens;
        // membership is checked independently, and directory identity is pinned.
        Require(a->second.stamp.Regular() ? a->second.stamp.Unchanged(b->second.stamp) : a->second.stamp.Identity(b->second.stamp),
                "companion identity or contents changed");
    }
    if (hashed) Require(expected.digest == actual.digest, "companion bytes changed");
}
} // namespace

ShieldedMigrationResult MigrateShieldedDatadirCopy(const fs::path& original, const fs::path& candidate,
    const ShieldedMigrationLimits& database_limits, const ShieldedCompanionLimits& companion_limits,
    bool apply, const std::function<void(const char*)>& checkpoint) {
    ShieldedMigrationResult result;
    try {
#if defined(__APPLE__) && TARGET_OS_IOS
        throw std::runtime_error("offline datadir migration is not qualified on iOS");
#endif
        Require(companion_limits.max_entries && companion_limits.max_bytes, "explicit companion budgets required");
        const auto source_path = CanonicalDirectory(original), candidate_path = CanonicalDirectory(candidate);
        Require(!Nested(source_path, candidate_path) && !Nested(candidate_path, source_path), "overlapping datadirs");
        Lease source(source_path), copy(candidate_path);
        const auto source_files = Capture(source, companion_limits, true), copy_files = Capture(copy, companion_limits, true);
        Require(source_files.digest == copy_files.digest, "original/candidate companion mismatch");
        const auto external = detail::InspectMigrationMetadata(source.path, companion_limits);
        Digest binding; source.Bind(binding); copy.Bind(binding); binding.Field(source_files.digest);
        auto verify = [&](bool hash) {
            Verify(source_files, Capture(source, companion_limits, hash), hash);
            Verify(copy_files, Capture(copy, companion_limits, hash), hash);
        };
        verify(false);
        result = detail::MigrateBoundCopy(source.path / "blockchain/chaindb", copy.path / "blockchain/chaindb",
            database_limits, apply, binding.Finish(), external, [&] { verify(false); }, [&](const char* stage) {
                if (checkpoint) checkpoint(stage);
                if (std::string_view(stage) == "before_ready") verify(true);
            });
        // Both outer leases outlive all inner handles and this final byte check.
        // If it fails after READY, report failure; never turn READY into a
        // launch authorization. The bound journal also refuses changed resumes.
        verify(true);
    } catch (const std::exception& error) { result.ok = result.ready = false; result.error = error.what(); }
    return result;
}
} // namespace dinero::storage
