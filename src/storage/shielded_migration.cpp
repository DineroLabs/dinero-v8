#include "storage/shielded_migration.h"
#include "shielded_migration_internal.h"
#include "common/serialization.h"
#include "consensus/chainwork.h"
#include "consensus/shielded/shielded_root.h"
#include "crypto/sha256.h"
#include <rocksdb/db.h>
#include <rocksdb/env.h>
#include <rocksdb/write_batch.h>
#include <algorithm>
#include <charconv>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace dinero::storage {
namespace {
namespace fs = std::filesystem;
namespace sh = consensus::shielded;
constexpr const char* kFamily = "shielded_state_v1";
constexpr const char* kLayout = "storage_layout_v1";
constexpr const char* kJournal = "shielded_migration_v1";
constexpr const char* kFrontier = "Mshielded_frontier";
constexpr const char* kAnchors = "Mshielded_anchor_history";
constexpr const char* kImport = "Mshielded_anchor_history_migrated_v1";
const std::set<std::string> kLegacy{
    "default", "meta", "blocks", "headers", "height", "txindex", "utxo", "utreexo", "prebase_coins"};

void Require(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
void Check(const rocksdb::Status& status, const char* action) {
    if (!status.ok()) throw std::runtime_error(std::string(action) + ": " + status.ToString());
}
bool Selected(const rocksdb::Slice& key) {
    return (!key.empty() && key[0] == 'N') || key == kFrontier || key == kAnchors || key == kImport;
}
bool Control(const std::string& family, const rocksdb::Slice& key) {
    return family == "meta" && (key == kLayout || key == kJournal);
}
struct Digest {
    crypto::CSHA256 hash;
    void Field(const rocksdb::Slice& value) {
        uint8_t size[8]; const uint64_t n = value.size();
        for (unsigned i = 0; i < 8; ++i) size[i] = static_cast<uint8_t>(n >> (8 * i));
        hash.Write(size, sizeof(size));
        hash.Write(reinterpret_cast<const uint8_t*>(value.data()), value.size());
    }
    std::string Finish() { return crypto::bytes_to_hex(hash.Finalize()); }
};
struct Identity {
    fs::path path;
    dev_t device;
    ino_t inode;
    Identity(const fs::path& input) {
        const auto absolute = fs::absolute(input);
        fs::path cursor;
        for (const auto& part : absolute) {
            cursor /= part;
            Require(!fs::is_symlink(fs::symlink_status(cursor)), "symlink in database path");
        }
        path = fs::canonical(absolute);
        struct stat info{};
        Require(::lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode), "database directory missing");
        device = info.st_dev; inode = info.st_ino;
        Require(fs::is_regular_file(path / "CURRENT") && fs::is_regular_file(path / "LOCK"), "existing database required");
        CheckFiles();
    }
    void CheckFiles() const {
        // Reject shared mutable files and links, even to paths within this DB.
        // Filesystem clones have separate inodes and are not hardlinks.
        for (const auto& entry : fs::recursive_directory_iterator(path)) {
            struct stat info{};
            Require(::lstat(entry.path().c_str(), &info) == 0, "cannot stat database entry");
            Require(S_ISDIR(info.st_mode) || (S_ISREG(info.st_mode) && info.st_nlink == 1),
                    "nonregular or hardlinked database entry");
        }
    }
    void Recheck() const {
        struct stat info{};
        Require(::lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode) &&
                info.st_dev == device && info.st_ino == inode && fs::canonical(path) == path,
                "database identity changed");
    }
    void Bind(Digest& digest) const {
        digest.Field(path.string()); digest.Field(std::to_string(device)); digest.Field(std::to_string(inode));
    }
};
bool Nested(const fs::path& parent, const fs::path& child) {
    auto a = parent.begin(), b = child.begin();
    for (; a != parent.end() && b != child.end(); ++a, ++b) if (*a != *b) return false;
    return a == parent.end();
}

// A real LOCK is owned outside all DB opens. Writable Open may borrow it once;
// closing a DB returns the loan, and only destruction here releases ownership.
class LockedEnv final : public rocksdb::EnvWrapper {
    std::string path_;
    rocksdb::FileLock* lock_ = nullptr;
    bool borrowed_ = false;
    dev_t device_{};
    ino_t inode_{};
public:
    LockedEnv(const fs::path& directory, rocksdb::Env* base) : EnvWrapper(base), path_((directory / "LOCK").string()) {
        Check(target()->LockFile(path_, &lock_), "exclusive database lock");
        struct stat info{};
        if (::lstat(path_.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1) {
            target()->UnlockFile(lock_).PermitUncheckedError(); lock_ = nullptr;
            throw std::runtime_error("invalid lock identity");
        }
        device_ = info.st_dev; inode_ = info.st_ino;
    }
    ~LockedEnv() override { if (lock_) target()->UnlockFile(lock_).PermitUncheckedError(); }
    void Recheck() const {
        struct stat info{};
        Require(::lstat(path_.c_str(), &info) == 0 && S_ISREG(info.st_mode) && info.st_nlink == 1 &&
                info.st_dev == device_ && info.st_ino == inode_, "database lock identity changed");
    }
    rocksdb::Status LockFile(const std::string& path, rocksdb::FileLock** result) override {
        *result = nullptr;
        if (borrowed_ || fs::path(path).lexically_normal() != fs::path(path_))
            return rocksdb::Status::IOError("unexpected migration lock request");
        borrowed_ = true; *result = lock_; return rocksdb::Status::OK();
    }
    rocksdb::Status UnlockFile(rocksdb::FileLock* lock) override {
        if (!borrowed_ || lock != lock_) return rocksdb::Status::IOError("unexpected migration lock release");
        borrowed_ = false; return rocksdb::Status::OK();
    }
};
struct Store {
    std::unique_ptr<rocksdb::DB> db;
    std::map<std::string, rocksdb::ColumnFamilyHandle*> families;
    ~Store() { Close(); }
    void Close() { for (const auto& [name, h] : families) db->DestroyColumnFamilyHandle(h); families.clear(); db.reset(); }
    void Open(const fs::path& path, LockedEnv& env, bool read_only) {
        Close(); rocksdb::Options options; options.env = &env;
        options.create_if_missing = options.create_missing_column_families = false;
        options.paranoid_checks = true;
        std::vector<std::string> names;
        Check(rocksdb::DB::ListColumnFamilies(options, path.string(), &names), "list families");
        auto expected = kLegacy; const bool separated = std::find(names.begin(), names.end(), kFamily) != names.end();
        if (separated) expected.insert(kFamily);
        Require(names.size() == expected.size() && std::set<std::string>(names.begin(), names.end()) == expected,
                "unsupported family layout");
        std::vector<rocksdb::ColumnFamilyDescriptor> descriptors;
        for (const auto& name : names) descriptors.emplace_back(name, options);
        std::vector<rocksdb::ColumnFamilyHandle*> handles; rocksdb::DB* pointer = nullptr;
        const auto status = read_only
            ? rocksdb::DB::OpenForReadOnly(options, path.string(), descriptors, &handles, &pointer)
            : rocksdb::DB::Open(options, path.string(), descriptors, &handles, &pointer);
        db.reset(pointer);
        for (auto* handle : handles) families.emplace(handle->GetName(), handle);
        Check(status, "open migration database");
    }
    std::optional<std::string> Get(const std::string& family, const rocksdb::Slice& key) const {
        const auto found = families.find(family);
        if (found == families.end()) return std::nullopt;
        std::string bytes; const auto status = db->Get(rocksdb::ReadOptions(), found->second, key, &bytes);
        if (status.IsNotFound()) return std::nullopt;
        Check(status, "read migration record"); return bytes;
    }
    std::string Need(const std::string& family, const std::string& key) const {
        auto value = Get(family, key); Require(value.has_value(), "missing required record"); return *value;
    }
    template<typename F> void Scan(const std::string& family, F&& visit) const {
        const auto found = families.find(family); if (found == families.end()) return;
        rocksdb::ReadOptions read; read.verify_checksums = true; read.fill_cache = false;
        std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(read, found->second));
        for (it->SeekToFirst(); it->Valid(); it->Next()) visit(it->key(), it->value());
        Check(it->status(), "migration inventory iterator");
    }
    void Write(rocksdb::WriteBatch& batch) {
        rocksdb::WriteOptions options; options.sync = true; options.disableWAL = false;
        Check(db->Write(options, &batch), "sync migration batch");
    }
};
uint64_t LE(const std::string& value, size_t offset, size_t count) {
    Require(offset <= value.size() && count <= value.size() - offset && count <= 8, "truncated integer");
    uint64_t n = 0; for (size_t i = 0; i < count; ++i) n |= uint64_t(static_cast<uint8_t>(value[offset + i])) << (8 * i);
    return n;
}
struct Inventory {
    std::string digest;
    uint64_t selected = 0;
    uint32_t height = 0;
    uint256 tip;
};
Inventory InspectOriginal(const Store& source, const ShieldedMigrationLimits& limits) {
    Require(source.families.size() == 9 && !source.Get("meta", kLayout) && !source.Get("meta", kJournal),
            "original must have legacy layout without migration controls");
    const auto schema = source.Need("meta", "schema_version");
    Require(schema.size() == 4 && LE(schema, 0, 4) == 4, "unsupported schema");
    const auto shielded = source.Need("meta", "shielded_tip"), forest = source.Need("meta", "forest_tip");
    Require(shielded.size() == 84 && forest.size() == 68, "invalid tip markers");
    const auto tip_bytes = source.Need("meta", "tip"); Reader tip(tip_bytes);
    const auto hash_hex = tip.readString();
    Require(hash_hex.size() == 64 && std::all_of(hash_hex.begin(), hash_hex.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }), "invalid tip hash");
    const auto hash = uint256::FromHexUnsafe(hash_hex); const auto height = tip.read<uint32_t>();
    (void)tip.read<arith_uint256>(); (void)tip.read<uint32_t>();
    Require(tip.eof() && height <= uint32_t(std::numeric_limits<int32_t>::max()) && LE(shielded, 0, 4) == height &&
            LE(forest, 0, 4) == height && std::memcmp(shielded.data() + 4, hash.data, 32) == 0 &&
            std::memcmp(forest.data() + 4, hash.data, 32) == 0, "chain/forest/shielded tip mismatch");
    Inventory result; Digest digest;
    std::vector<sh::NullifierEntry> nullifiers; std::set<sh::Hash> seen;
    for (const auto& [family, handle] : source.families) {
        digest.Field("CF"); digest.Field(family);
        source.Scan(family, [&](const rocksdb::Slice& key, const rocksdb::Slice& value) {
            digest.Field("KV"); digest.Field(key); digest.Field(value);
            if (family != "utreexo" || !Selected(key)) return;
            Require(key.size() <= limits.max_record_bytes && value.size() <= limits.max_record_bytes - key.size(),
                    "selected record exceeds resource limit");
            Require(key.size() + value.size() <= limits.batch_bytes, "selected record exceeds batch budget");
            ++result.selected;
            if (key[0] == 'N') {
                Require(key.size() == 37 && value.empty(), "malformed nullifier record");
                Require(nullifiers.size() < limits.max_nullifiers, "nullifier inventory exceeds resource limit");
                sh::NullifierEntry entry;
                for (size_t i = 1; i < 5; ++i) entry.height = (entry.height << 8) | static_cast<uint8_t>(key[i]);
                std::memcpy(entry.nullifier.data(), key.data() + 5, 32);
                Require(entry.height <= height && seen.insert(entry.nullifier).second,
                        "future or duplicate nullifier");
                nullifiers.push_back(entry);
            } else if (key == kImport) Require(value == "1", "ambiguous legacy import marker");
        });
        digest.Field("ENDCF");
    }
    sh::CommitmentTree tree; sh::AnchorHistory anchors;
    const auto frontier = source.Need("utreexo", kFrontier), history = source.Need("utreexo", kAnchors);
    Require(tree.DeserializeFrontier(reinterpret_cast<const uint8_t*>(frontier.data()), frontier.size()), "invalid frontier");
    // The legacy decoder accepts trailing bytes. Migration must not silently
    // treat them as a valid canonical record or discard them by reserializing.
    const auto encoded = tree.SerializeFrontier();
    Require(frontier == std::string(encoded.begin(), encoded.end()), "noncanonical frontier encoding");
    Require(anchors.DeserializePersistenceBytes({history.begin(), history.end()}) == sh::AnchorHistory::IoResult::Ok,
            "invalid anchor history");
    const auto tree_root = tree.Root();
    const auto root = sh::ComputeShieldedRootFromParts({tree_root.begin(), tree_root.end()}, tree.Size(),
        sh::ComputeNullifierAccumulator(nullifiers), anchors.SerializeBytes());
    Require(root && LE(shielded, 68, 8) == tree.Size() && LE(shielded, 76, 8) == nullifiers.size() &&
            std::memcmp(shielded.data() + 36, root->data, 32) == 0, "shielded state root/count mismatch");
    result.height = height; result.tip = hash;
    result.digest = digest.Finish(); return result;
}
// Validate external base claims while the original's real RocksDB lock is
// owned, before opening the candidate writable. Height indexes are not an
// ancestry authority: walk stored headers back from the active tip identity.
void CheckProtectedBases(const Store& source, const Inventory& inventory,
                         const detail::ExternalMigrationState& external) {
    std::map<uint32_t, std::optional<std::string>> targets;
    auto add = [&](uint32_t height, const std::optional<std::string>& hash) {
        Require(height <= inventory.height, "protected base is above active tip");
        auto [it, inserted] = targets.emplace(height, hash);
        if (!inserted && hash) {
            Require(!it->second || *it->second == *hash, "conflicting protected-base identities"); it->second = hash;
        }
    };
    if (external.promoted_base) {
        const auto& base = *external.promoted_base;
        Require(source.Get("utreexo", "Massumeutxo_promoted:" + base.hash) == std::optional<std::string>("1"),
                "fully-validated base lacks completed ChainDB promotion");
        add(base.height, base.hash);
    }
    if (external.wallet_base_height) add(*external.wallet_base_height, {});
    const auto prebase = source.Get("prebase_coins", "M:base");
    if (prebase) {
        Require(prebase->size() == 69 && (*prebase)[0] == 64, "invalid pre-base marker encoding");
        Reader reader(*prebase); const auto hash = reader.readString(); const auto height = reader.read<uint32_t>();
        Require(reader.eof() && hash != std::string(64, '0') && std::all_of(hash.begin(), hash.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }), "invalid pre-base marker identity");
        add(height, hash);
    } else source.Scan("prebase_coins", [](const rocksdb::Slice&, const rocksdb::Slice&) {
        throw std::runtime_error("pre-base records lack their base marker");
    });
    if (targets.empty()) return;
    const auto lowest = targets.begin()->first;
    Require(uint64_t(inventory.height) - lowest + 1 <= external.max_ancestry_headers, "protected ancestry exceeds budget");
    auto hash = inventory.tip;
    for (uint32_t height = inventory.height;; --height) {
        const auto bytes = source.Need("headers", "h" + hash.GetHex());
        Require(bytes.size() == kHeaderWireSize + sizeof(uint32_t) + sizeof(arith_uint256), "invalid ancestry header encoding");
        Reader reader(bytes); BlockHeader header{}; Deserialize(reader, header);
        const auto stored_height = reader.read<uint32_t>(); (void)reader.read<arith_uint256>();
        Require(reader.eof() && stored_height == height && header.GetHash() == hash, "protected ancestry header identity mismatch");
        const auto target = targets.find(height);
        if (target != targets.end() && target->second)
            Require(*target->second == hash.GetHex(), "protected base is not on active ancestry");
        if (height == lowest) break;
        hash = header.prev_block_hash;
    }
}
struct Journal {
    std::string operation, source, phase;
    uint64_t rows = 0, retired = 0;
    std::string Encode() const {
        return "SCFM1\n" + operation + "\n" + source + "\n" + std::to_string(rows) + "\n" +
            std::to_string(retired) + "\n" + phase + "\n";
    }

};

// Decode separately with a canonical re-encode check so neither overflow,
// trailing bytes nor alternate representations can masquerade as our journal.
Journal ReadJournal(const std::string& bytes) {
    Require(bytes.size() < 512, "oversized migration journal");
    std::vector<std::string> fields; size_t begin = 0, end;
    while ((end = bytes.find('\n', begin)) != std::string::npos) { fields.push_back(bytes.substr(begin, end - begin)); begin = end + 1; }
    Require(begin == bytes.size() && fields.size() == 6 && fields[0] == "SCFM1", "malformed migration journal");
    Journal j; j.operation = fields[1]; j.source = fields[2]; j.phase = fields[5];
    auto parse = [](const std::string& s, uint64_t& n) {
        const auto result = std::from_chars(s.data(), s.data() + s.size(), n);
        Require(result.ec == std::errc{} && result.ptr == s.data() + s.size(), "invalid journal count");
    };
    parse(fields[3], j.rows); parse(fields[4], j.retired);
    Require(j.operation.size() == 64 && j.source.size() == 64 && j.retired <= j.rows && j.Encode() == bytes &&
            (j.phase == "PREPARING" || j.phase == "MOVING" || j.phase == "VERIFYING" || j.phase == "READY"),
            "invalid migration journal");
    return j;
}
std::string Layout(const Journal& j) { return "shielded-state-v1:" + j.phase; }
void Save(Store& candidate, const Journal& journal, rocksdb::WriteBatch& batch) {
    batch.Put(candidate.families.at("meta"), kLayout, Layout(journal));
    batch.Put(candidate.families.at("meta"), kJournal, journal.Encode());
}
void Compare(const Store& original, const Store& candidate, const Journal& journal, bool started) {
    // Walk both directions, including every unknown nonselected record. Point
    // reads require byte equality, not merely equal counts or a root match.
    uint64_t selected = 0;
    for (const auto& [family, handle] : original.families) original.Scan(family, [&](const rocksdb::Slice& key, const rocksdb::Slice& value) {
        const auto old = candidate.Get(family, key);
        if (family == "utreexo" && Selected(key)) {
            const auto moved = candidate.Get(kFamily, key);
            Require((old || moved) && (!old || *old == value) && (!moved || *moved == value), "missing or conflicting selected copy");
            const bool retired = selected++ < journal.retired;
            Require(retired ? (!old && moved) : old.has_value(), "retirement disagrees with durable journal");
            if (!started || journal.phase == "PREPARING") Require(!moved, "unexpected early destination record");
        } else Require(old && *old == value, "nonselected record changed or missing");
    });
    Require(selected == journal.rows, "source selected count changed");
    for (const auto& [family, handle] : candidate.families) candidate.Scan(family, [&](const rocksdb::Slice& key, const rocksdb::Slice& value) {
        if (started && Control(family, key)) return;
        if (family == kFamily) Require(Selected(key), "unexpected destination record");
        const auto expected = original.Get(family == kFamily ? "utreexo" : family, key);
        Require(expected && *expected == value, "candidate contains unexplained record");
    });
    if (journal.phase == "VERIFYING" || journal.phase == "READY") Require(journal.retired == journal.rows, "premature completion");
}
} // namespace

static ShieldedMigrationResult RunMigration(const fs::path& original_path, const fs::path& candidate_path,
    const ShieldedMigrationLimits& limits, bool apply, const std::function<void(const char*)>& checkpoint,
    rocksdb::Env* environment, const std::string& cohort_binding = {},
    const std::function<void()>& outer_ownership = {}, const detail::ExternalMigrationState* external = nullptr) {
    ShieldedMigrationResult result;
    try {
#if defined(__APPLE__) && TARGET_OS_IOS
        throw std::runtime_error("offline migration is not qualified on iOS");
#endif
        Require(limits.batch_bytes && limits.batch_rows && limits.max_record_bytes && limits.max_nullifiers &&
                limits.max_record_bytes <= limits.batch_bytes && limits.max_record_bytes <= std::numeric_limits<size_t>::max() &&
                limits.max_nullifiers <= std::numeric_limits<size_t>::max() / sizeof(sh::NullifierEntry), "invalid resource budgets");
        Identity original_id(original_path), candidate_id(candidate_path);
        Require(!Nested(original_id.path, candidate_id.path) && !Nested(candidate_id.path, original_id.path) &&
                !(original_id.device == candidate_id.device && original_id.inode == candidate_id.inode), "overlapping database paths");
        LockedEnv original_lock(original_id.path, environment), candidate_lock(candidate_id.path, environment);
        auto identity_check = [&] {
            original_id.Recheck(); candidate_id.Recheck(); original_lock.Recheck(); candidate_lock.Recheck();
            if (outer_ownership) outer_ownership();
        };
        identity_check();
        Store original, candidate;
        original.Open(original_id.path, original_lock, true); candidate.Open(candidate_id.path, candidate_lock, true);
        const auto inventory = InspectOriginal(original, limits);
        if (external) CheckProtectedBases(original, inventory, *external);
        Digest binding; binding.Field("shielded-copy-v1"); original_id.Bind(binding); candidate_id.Bind(binding); binding.Field(inventory.digest);
        // A bound migration cannot resume through the unbound engine.
        if (!cohort_binding.empty()) { binding.Field("datadir-companions-v1"); binding.Field(cohort_binding); }
        Journal journal{binding.Finish(), inventory.digest, "PREPARING", inventory.selected, 0};
        result.operation = journal.operation; result.source_digest = journal.source; result.selected_rows = journal.rows;
        const auto persisted = candidate.Get("meta", kJournal), layout = candidate.Get("meta", kLayout);
        const bool started = persisted.has_value();
        if (started) {
            const auto saved = ReadJournal(*persisted);
            Require(saved.operation == journal.operation && saved.source == journal.source && saved.rows == journal.rows &&
                    layout && *layout == Layout(saved), "migration binding or layout mismatch");
            journal = saved;
            Require((journal.phase == "PREPARING" || candidate.families.count(kFamily)) &&
                    (journal.phase != "PREPARING" || journal.retired == 0), "missing destination or early retirement");
        } else Require(!layout && !candidate.families.count(kFamily), "unexplained destination layout");
        Compare(original, candidate, journal, started);
        result.phase = started ? journal.phase : "LEGACY"; result.retired_rows = journal.retired;
        if (!apply || journal.phase == "READY") {
            identity_check(); result.ok = true; result.ready = journal.phase == "READY"; return result;
        }
        candidate.Open(candidate_id.path, candidate_lock, false); identity_check();
        auto hit = [&](const char* stage) { if (checkpoint) checkpoint(stage); identity_check(); };
        auto publish = [&](const char* phase) {
            identity_check(); journal.phase = phase; rocksdb::WriteBatch batch; Save(candidate, journal, batch); candidate.Write(batch);
            result.phase = journal.phase; result.retired_rows = journal.retired;
        };
        if (!started) { publish("PREPARING"); hit("after_prepare"); }
        if (!candidate.families.count(kFamily)) {
            identity_check(); rocksdb::ColumnFamilyHandle* handle = nullptr;
            const auto status = candidate.db->CreateColumnFamily(rocksdb::ColumnFamilyOptions(), kFamily, &handle);
            if (handle) candidate.families.emplace(kFamily, handle);
            Check(status, "create destination family"); hit("after_create");
        }
        if (journal.phase == "PREPARING") publish("MOVING");
        if (journal.phase == "MOVING") {
            std::vector<std::pair<std::string, std::string>> pending; uint64_t bytes = 0, index = 0;
            auto flush = [&] {
                if (pending.empty()) return;
                identity_check(); rocksdb::WriteBatch copies;
                for (const auto& [key, value] : pending) copies.Put(candidate.families.at(kFamily), key, value);
                hit("before_copy");
                candidate.Write(copies); hit("after_copy");
                for (const auto& [key, value] : pending) {
                    const auto copied = candidate.Get(kFamily, key);
                    Require(copied && *copied == value, "copied bytes failed readback");
                }
                hit("after_readback");
                // A caller hook never grants write authority. Verify again at
                // retirement and keep retirement + progress in one sync batch.
                rocksdb::WriteBatch retire;
                for (const auto& [key, value] : pending) {
                    const auto copied = candidate.Get(kFamily, key), old = candidate.Get("utreexo", key);
                    Require(copied && *copied == value && old && *old == value, "retirement source/destination mismatch");
                    retire.Delete(candidate.families.at("utreexo"), key);
                }
                journal.retired += pending.size(); Save(candidate, journal, retire); candidate.Write(retire);
                result.retired_rows = journal.retired; hit("after_retire"); pending.clear(); bytes = 0;
            };
            original.Scan("utreexo", [&](const rocksdb::Slice& key, const rocksdb::Slice& value) {
                if (!Selected(key)) return;
                if (index++ < journal.retired) return;
                const uint64_t size = key.size() + value.size();
                if (pending.size() == limits.batch_rows || size > limits.batch_bytes - bytes) flush();
                pending.emplace_back(key.ToString(), value.ToString()); bytes += size;
            });
            flush(); publish("VERIFYING"); hit("after_verifying");
        }
        Compare(original, candidate, journal, true); identity_check(); hit("before_ready");
        publish("READY"); hit("after_ready");
        result.ok = result.ready = true;
    } catch (const std::exception& error) {
        result.ok = result.ready = false; result.error = error.what();
    }
    return result;
}

ShieldedMigrationResult MigrateShieldedStateCopy(const fs::path& original, const fs::path& candidate,
    const ShieldedMigrationLimits& limits, bool apply, const std::function<void(const char*)>& checkpoint) {
    return RunMigration(original, candidate, limits, apply, checkpoint, rocksdb::Env::Default());
}

ShieldedMigrationResult detail::MigrateBoundCopy(const fs::path& original, const fs::path& candidate,
    const ShieldedMigrationLimits& limits, bool apply, const std::string& binding, const detail::ExternalMigrationState& external,
    const std::function<void()>& ownership, const std::function<void(const char*)>& checkpoint) {
    return RunMigration(original, candidate, limits, apply, checkpoint, rocksdb::Env::Default(), binding, ownership, &external);
}

#ifdef DINERO_SHIELDED_MIGRATION_FAULT_TESTING
// Compiled only into the noninstalled qualification executable. The public
// engine cannot be given a no-lock Env or an operator-selected I/O backend.
ShieldedMigrationResult MigrateShieldedStateCopyForTesting(const fs::path& original, const fs::path& candidate,
    const ShieldedMigrationLimits& limits, const std::function<void(const char*)>& checkpoint, rocksdb::Env& env) {
    return RunMigration(original, candidate, limits, true, checkpoint, &env);
}
#endif
} // namespace dinero::storage
