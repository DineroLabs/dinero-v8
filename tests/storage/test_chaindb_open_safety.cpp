// Safe-opening regressions using only generated temporary RocksDB stores.
// Only generated temporary stores are opened; no migration is implemented here.
#include "storage/chain_db.h"
#include "storage/chain_write_token.h"
#include <rocksdb/db.h>
#include <rocksdb/env.h>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

using dinero::ChainDB;
using dinero::Status;
using Records = std::map<std::string, std::map<std::string, std::string>>;
const std::vector<std::string> canonical{
    "default", "meta", "blocks", "headers", "height", "txindex", "utxo", "utreexo", "prebase_coins"};

void Setup(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::string Version(uint32_t version) {
    return std::string(reinterpret_cast<const char*>(&version), sizeof(version));
}

struct RawStore {
    std::unique_ptr<rocksdb::DB> db;
    std::vector<rocksdb::ColumnFamilyHandle*> handles;
    ~RawStore() {
        for (auto* handle : handles) db->DestroyColumnFamilyHandle(handle);
    }
    void open(const std::filesystem::path& path, const std::vector<std::string>& names, bool create) {
        rocksdb::Options options;
        options.create_if_missing = create;
        options.create_missing_column_families = create;
        std::vector<rocksdb::ColumnFamilyDescriptor> descriptors;
        for (const auto& name : names) descriptors.emplace_back(name, options);
        rocksdb::DB* raw = nullptr;
        const auto status = create
            ? rocksdb::DB::Open(options, path.string(), descriptors, &handles, &raw)
            : rocksdb::DB::OpenForReadOnly(options, path.string(), descriptors, &handles, &raw);
        db.reset(raw);
        Setup(status.ok(), "raw fixture open failed");
    }
};

void Seed(const std::filesystem::path& path, const std::vector<std::string>& names,
          const std::optional<std::string>& schema, const std::optional<std::string>& layout) {
    RawStore store;
    store.open(path, names, true);
    rocksdb::WriteOptions writes;
    writes.sync = true;
    for (size_t i = 0; i < names.size(); ++i) {
        Setup(store.db->Put(writes, store.handles[i], "Mopen_safety_identity", names[i]).ok(), "seed identity");
        if (names[i] == "meta") {
            if (schema) Setup(store.db->Put(writes, store.handles[i], "schema_version", *schema).ok(), "seed schema");
            if (layout) Setup(store.db->Put(writes, store.handles[i], "storage_layout_v1", *layout).ok(), "seed layout");
        }
    }
}

struct Snapshot {
    Records records;
    uint64_t sequence = 0;
    std::map<std::string, std::string> engine_files;
};

Snapshot Inspect(const std::filesystem::path& path) {
    Snapshot result;
    std::vector<std::string> names;
    Setup(rocksdb::DB::ListColumnFamilies(rocksdb::Options(), path.string(), &names).ok(), "list families");
    {
        RawStore store;
        store.open(path, names, false);
        result.sequence = store.db->GetLatestSequenceNumber();
        for (size_t i = 0; i < names.size(); ++i) {
            auto& records = result.records[names[i]];
            std::unique_ptr<rocksdb::Iterator> it(store.db->NewIterator(rocksdb::ReadOptions(), store.handles[i]));
            for (it->SeekToFirst(); it->Valid(); it->Next()) records[it->key().ToString()] = it->value().ToString();
            Setup(it->status().ok(), "inspect iterator");
        }
    }
    // Ignore diagnostic LOG/LOCK/OPTIONS churn. CURRENT, manifest, table and WAL
    // bytes belong to engine state and must not change on a refused layout.
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
        const auto name = entry.path().filename().string();
        const auto extension = entry.path().extension().string();
        if (name != "CURRENT" && name.rfind("MANIFEST-", 0) != 0 &&
            extension != ".sst" && extension != ".log") continue;
        std::ifstream file(entry.path(), std::ios::binary);
        Setup(file.good(), "read engine file");
        result.engine_files[name] = std::string(std::istreambuf_iterator<char>(file), {});
        Setup(!file.bad(), "read engine file failed");
    }
    return result;
}

struct Case {
    std::string name;
    std::vector<std::string> names;
    std::optional<std::string> schema = Version(4);
    std::optional<std::string> layout;
    bool accept = false;
    bool fresh = false;
};

std::vector<Case> Cases() {
    auto legacy = canonical; legacy.pop_back();
    auto permuted = canonical; std::swap(permuted[6], permuted[7]);
    auto permuted_legacy = legacy; std::swap(permuted_legacy[6], permuted_legacy[7]);
    auto unknown = canonical; unknown.push_back("utreexo_checkpoints_v1");
    auto swapped_unknown = unknown; std::swap(swapped_unknown[6], swapped_unknown[7]);
    auto substituted = legacy; substituted.push_back("utreexo_checkpoints_v1");
    auto missing = canonical; missing.erase(missing.begin() + 3); // headers missing
    auto failed_append = missing;
    failed_append.erase(std::find(failed_append.begin(), failed_append.end(), "txindex"));
    failed_append.push_back("utreexo_checkpoints_v1"); // eight CFs, but prebase already exists
    return {
        {"fresh", {}, {}, {}, true, true},
        {"canonical9", canonical, Version(4), {}, true},
        {"legacy8", legacy, Version(4), {}, true},
        {"permuted_known9", permuted, Version(4), {}, true},
        {"permuted_known8", permuted_legacy, Version(4), {}, true},
        {"known9_missing_schema", canonical, {}, {}, true},
        {"future_schema9", canonical, Version(999)},
        {"future_schema8_no_append", legacy, Version(999)},
        {"unsigned_future_schema", canonical, Version(UINT32_MAX)},
        {"malformed_schema9", canonical, std::string("bad")},
        {"malformed_schema8_no_append", legacy, std::string("bad")},
        {"unknown_cf", unknown},
        {"unknown_cf_permuted", swapped_unknown},
        {"unknown_substitutes_prebase", substituted},
        {"missing_headers", missing},
        {"failed_append_must_close", failed_append},
        {"incomplete_layout_known9", canonical, Version(4), std::string("MOVING")},
        {"ready_layout_known9", canonical, Version(4), std::string("READY")},
        {"empty_layout_known9", canonical, Version(4), std::string()},
        {"old_schema", canonical, Version(3)},
        {"ownership_and_move", {}},
        {"read_failure", {}},
        {"append_failure", {}},
        {"schema_write_failure", {}},
        {"missing_current", {}},
    };
}

// Inject real read/manifest/WAL failures through the existing Env seam. Also
// probe the actual LOCK during CURRENT inspection; this is a deterministic
// check/open interleave, with no sleeps or fake ChainDB implementation.
class InspectEnv : public rocksdb::EnvWrapper {
public:
    InspectEnv() : EnvWrapper(rocksdb::Env::Default()) {}
    std::string fail_word;
    bool fail_read = false;
    bool probe_lock = false;
    std::atomic<unsigned> hits{0}, probes{0}, acquired{0}, unlocks{0};
    rocksdb::Status FileExists(const std::string& path) override {
        if (std::filesystem::path(path).filename() == "CURRENT") {
            if (fail_read) {
                ++hits;
                return rocksdb::Status::IOError("injected CURRENT read failure");
            }
            if (probe_lock) {
                ++probes;
                rocksdb::FileLock* lock = nullptr;
                auto st = target()->LockFile((std::filesystem::path(path).parent_path() / "LOCK").string(), &lock);
                if (st.ok()) { ++acquired; target()->UnlockFile(lock).PermitUncheckedError(); }
            }
        }
        return target()->FileExists(path);
    }
    rocksdb::Status UnlockFile(rocksdb::FileLock* lock) override {
        ++unlocks;
        return target()->UnlockFile(lock);
    }
    class FaultFile : public rocksdb::WritableFileWrapper {
    public:
        FaultFile(std::unique_ptr<rocksdb::WritableFile> file, InspectEnv& env)
            : WritableFileWrapper(file.get()), file_(std::move(file)), env_(env) {}
        rocksdb::Status Append(const rocksdb::Slice& data) override {
            if (!env_.fail_word.empty() && data.ToString().find(env_.fail_word) != std::string::npos) {
                ++env_.hits;
                return rocksdb::Status::IOError("injected record write failure");
            }
            return file_->Append(data);
        }
        rocksdb::Status Append(const rocksdb::Slice& data, const rocksdb::DataVerificationInfo&) override {
            return Append(data);
        }
    private:
        std::unique_ptr<rocksdb::WritableFile> file_;
        InspectEnv& env_;
    };
    rocksdb::Status NewWritableFile(const std::string& path,
                                   std::unique_ptr<rocksdb::WritableFile>* file,
                                   const rocksdb::EnvOptions& options) override {
        const auto status = target()->NewWritableFile(path, file, options);
        if (status.ok() && (std::filesystem::path(path).extension() == ".log" ||
                           std::filesystem::path(path).filename().string().rfind("MANIFEST-", 0) == 0)) {
            *file = std::make_unique<FaultFile>(std::move(*file), *this);
        }
        return status;
    }
};

int Special(const std::string& name, const std::filesystem::path& root) {
    auto names = canonical;
    std::optional<std::string> schema = Version(4);
    if (name == "append_failure") names.pop_back();
    if (name == "schema_write_failure") schema.reset();
    const auto path = root / name;
    Seed(path, names, schema, {});
    std::vector<std::string> failures;
    auto require = [&](bool ok, const std::string& message) { if (!ok) failures.push_back(message); };
    InspectEnv env;
    ChainDB db;
    db.setEnvForTesting(&env);
    const auto token = dinero::ChainWriteToken::CreateForTesting();
    if (name == "ownership_and_move") {
        env.probe_lock = true;
        require(db.init(path) == Status::Ok, "first owner refused");
        require(env.probes > 0 && env.acquired == 0, "preflight did not hold LOCK");
        require(env.unlocks == 0, "ownership released before open completed");
        ChainDB competing;
        require(competing.init(path) != Status::Ok, "second owner accepted");
        const auto alias = root / "alias";
        std::filesystem::create_directory_symlink(path, alias);
        require(competing.init(alias) != Status::Ok, "alias admitted second owner");
        ChainDB moved(std::move(db));
        require(moved.putUtreexoMeta(token, "moved", "alive") == Status::Ok, "move lost ownership");
        require(env.unlocks == 0, "move released LOCK");
        require(competing.init(path) != Status::Ok, "move admitted second owner");
        ChainDB assigned;
        require(assigned.init(root / "other") == Status::Ok, "move-assignment fixture failed");
        assigned = std::move(moved);
        require(assigned.getUtreexoMeta("moved").ok(), "move assignment lost DB");
        assigned.close();
        require(env.unlocks == 1, "close did not release exactly once");
        require(competing.init(path) == Status::Ok, "close did not permit next owner");
    } else if (name == "missing_current") {
        std::filesystem::remove(path / "CURRENT");
        std::map<std::string, std::string> before;
        for (const auto& entry : std::filesystem::directory_iterator(path)) {
            std::ifstream f(entry.path(), std::ios::binary);
            before[entry.path().filename().string()] = std::string(std::istreambuf_iterator<char>(f), {});
        }
        require(db.init(path) != Status::Ok, "damaged store treated as fresh");
        for (const auto& [file, bytes] : before) {
            std::ifstream f(path / file, std::ios::binary);
            require(f.good() && std::string(std::istreambuf_iterator<char>(f), {}) == bytes,
                    "damaged store bytes changed: " + file);
        }
        require(!std::filesystem::exists(path / "CURRENT"), "CURRENT recreated");
    } else {
        if (name == "read_failure") env.fail_read = true;
        if (name == "append_failure") env.fail_word = "prebase_coins";
        if (name == "schema_write_failure") env.fail_word = "schema_version";
        require(db.init(path) != Status::Ok, "injected failure was accepted");
        require(env.hits > 0, "fault was never reached");
        require(!db.getUtreexoMeta("open_safety_identity").ok(), "failed object readable");
        require(db.putUtreexoMeta(token, "must_not_write", "rejected") != Status::Ok, "failed object writable");
        env.fail_read = false;
        env.fail_word.clear();
        require(db.init(path) == Status::Ok, "same object could not retry after IO failure");
        const auto identity = db.getUtreexoMeta("open_safety_identity");
        require(identity.ok() && identity.value() == "utreexo", "retry lost records");
    }
    for (const auto& failure : failures) std::cout << "FAIL " << name << ": " << failure << '\n';
    if (failures.empty()) std::cout << "PASS " << name << '\n';
    return failures.empty() ? 0 : 1;
}

int Run(const Case& test, const std::filesystem::path& root) {
    if (!test.fresh && test.names.empty()) return Special(test.name, root);
    const auto path = root / test.name;
    if (!test.fresh) Seed(path, test.names, test.schema, test.layout);
    const auto before = test.fresh ? Snapshot{} : Inspect(path);
    if (!test.fresh) {
        const auto repeated = Inspect(path);
        Setup(before.records == repeated.records && before.sequence == repeated.sequence &&
              before.engine_files == repeated.engine_files, "read-only inspection mutated fixture");
    }
    std::vector<std::string> failures;
    const auto require = [&](bool ok, const std::string& message) {
        if (!ok) failures.push_back(message);
    };
    const auto token = dinero::ChainWriteToken::CreateForTesting();
    {
        ChainDB db;
        const auto opened = db.init(path);
        if (test.accept) {
            require(opened == Status::Ok, "supported store was refused");
            if (opened == Status::Ok) {
                int version = 0;
                require(db.getSchemaVersion(version) == Status::Ok && version == 4, "wrong schema after supported open");
                if (!test.fresh) {
                    const auto identity = db.getUtreexoMeta("open_safety_identity");
                    require(identity.ok() && identity.value() == "utreexo", "read routed to wrong family");
                }
                require(db.putUtreexoMeta(token, "open_safety_write", "expected") == Status::Ok, "supported write failed");
            }
        } else {
            require(opened != Status::Ok, "unsupported store was accepted");
            if (opened != Status::Ok) {
                require(!db.getUtreexoMeta("open_safety_identity").ok(), "refused object remains readable");
                require(db.putUtreexoMeta(token, "must_not_write", "rejected") != Status::Ok, "refused object remains writable");
            }
        }
    }
    auto after = Inspect(path);
    if (test.accept) {
        std::vector<std::string> names;
        for (const auto& [name, records] : after.records) names.push_back(name);
        auto expected = canonical; std::sort(expected.begin(), expected.end());
        require(names == expected, "supported store has wrong family set");
        require(after.records["utreexo"]["Mopen_safety_write"] == "expected", "write routed to wrong family");
        after.records["utreexo"].erase("Mopen_safety_write");
        for (const auto& [name, records] : after.records) {
            if (name != "utreexo") require(records.count("Mopen_safety_write") == 0, "write leaked into another family");
        }
        if (!test.fresh) {
            auto expected_records = before.records;
            expected_records.try_emplace("prebase_coins");
            if (!test.schema) expected_records["meta"]["schema_version"] = Version(4);
            require(after.records == expected_records, "supported open changed existing records");
        }
        // Verify named read/write still works after reopen.
        ChainDB reopened;
        require(reopened.init(path) == Status::Ok, "supported reopen failed");
        const auto value = reopened.getUtreexoMeta("open_safety_write");
        require(value.ok() && value.value() == "expected", "reopen lost/misrouted value");
    } else {
        require(before.records == after.records, "refused open changed CF names or stored records");
        require(before.sequence == after.sequence, "refused open advanced sequence number");
        require(before.engine_files == after.engine_files, "refused open changed manifest/table/WAL state");
    }
    for (const auto& failure : failures) std::cout << "FAIL " << test.name << ": " << failure << '\n';
    if (failures.empty()) std::cout << "PASS " << test.name << '\n';
    return failures.empty() ? 0 : 1;
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--list") {
            for (const auto& test : Cases()) std::cout << test.name << '\n';
            return 0;
        }
        if (argc == 1) {
            int failures = 0;
            for (const auto& test : Cases()) {
                char path_template[] = "/tmp/dinero-open-safety-XXXXXX";
                char* created = mkdtemp(path_template);
                Setup(created != nullptr, "mkdtemp failed");
                const std::filesystem::path root(created);
                failures += Run(test, root);
                std::filesystem::remove_all(root);
            }
            return failures ? 1 : 0;
        }
        Setup(argc == 2, "expected one case name");
        const auto cases = Cases();
        const auto found = std::find_if(cases.begin(), cases.end(), [&](const auto& test) { return test.name == argv[1]; });
        Setup(found != cases.end(), "unknown case");
        char path_template[] = "/tmp/dinero-open-safety-XXXXXX";
        char* created = mkdtemp(path_template);
        Setup(created != nullptr, "mkdtemp failed");
        const std::filesystem::path root(created);
        const int result = Run(*found, root);
        std::filesystem::remove_all(root);
        return result;
    } catch (const std::exception& e) {
        std::cerr << "SETUP ERROR: " << e.what() << '\n';
        return 2;
    }
}
