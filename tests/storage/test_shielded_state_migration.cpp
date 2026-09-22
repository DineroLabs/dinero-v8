// Only generated, disposable stores. Never a datadir migration launcher.
#include "shielded_migration_fixture.h"
#include "storage/shielded_migration.h"
#include "consensus/shielded/shielded_root.h"
#include <cstring>
#include <fstream>
#include <atomic>
#include <rocksdb/env.h>
#include <sys/wait.h>

using namespace shielded_store_fixture;
using namespace dinero;
namespace sh = dinero::consensus::shielded;
namespace fs = std::filesystem;
using dinero::storage::MigrateShieldedStateCopy;
constexpr dinero::storage::ShieldedMigrationLimits limits{32768, 1, 16384, 100};
std::string executable;

namespace dinero::storage {
ShieldedMigrationResult MigrateShieldedStateCopyForTesting(const fs::path&, const fs::path&,
    const ShieldedMigrationLimits&, const std::function<void(const char*)>&, rocksdb::Env&);
}

// Fail real RocksDB WAL operations, including the uncertain case where Sync
// succeeded but its acknowledgement was lost. This is not a power-loss model.
struct FaultEnv : rocksdb::EnvWrapper {
    std::atomic<bool> armed{false};
    std::atomic<unsigned> fired{0};
    const fs::path candidate;
    const std::string fault;
    FaultEnv(fs::path path, std::string mode)
        : EnvWrapper(rocksdb::Env::Default()), candidate(std::move(path)), fault(std::move(mode)) {}
    struct File : rocksdb::WritableFileWrapper {
        std::unique_ptr<rocksdb::WritableFile> file;
        FaultEnv& env;
        File(std::unique_ptr<rocksdb::WritableFile> raw, FaultEnv& owner)
            : WritableFileWrapper(raw.get()), file(std::move(raw)), env(owner) {}
        bool Fail(const char* mode) {
            if (env.fault != mode || !env.armed.exchange(false)) return false;
            ++env.fired; return true;
        }
        rocksdb::Status Append(const rocksdb::Slice& bytes) override {
            if (Fail("append")) return rocksdb::Status::IOError("injected WAL append failure");
            return file->Append(bytes);
        }
        rocksdb::Status Append(const rocksdb::Slice& bytes, const rocksdb::DataVerificationInfo& info) override {
            if (Fail("append")) return rocksdb::Status::IOError("injected WAL append failure");
            return file->Append(bytes, info);
        }
        rocksdb::Status Sync() override {
            if (Fail("sync")) return rocksdb::Status::IOError("injected WAL sync failure");
            auto status = file->Sync();
            if (status.ok() && Fail("ack")) return rocksdb::Status::IOError("injected lost WAL sync acknowledgement");
            return status;
        }
        rocksdb::Status Fsync() override { return Sync(); }
    };
    rocksdb::Status NewWritableFile(const std::string& name, std::unique_ptr<rocksdb::WritableFile>* out,
                                    const rocksdb::EnvOptions& options) override {
        auto status = target()->NewWritableFile(name, out, options);
        if (status.ok() && fs::path(name).parent_path() == candidate && fs::path(name).extension() == ".log")
            *out = std::make_unique<File>(std::move(*out), *this);
        return status;
    }
};

std::map<std::string, std::string> PersistentFiles(const fs::path& path) {
    std::map<std::string, std::string> files;
    for (const auto& entry : fs::directory_iterator(path)) {
        const auto name = entry.path().filename().string();
        if (name == "LOCK" || name.rfind("LOG", 0) == 0) continue; // diagnostic logs are not DB state
        Setup(entry.is_regular_file(), "regular original storage file");
        std::ifstream input(entry.path(), std::ios::binary);
        Setup(input.good(), "read original storage file");
        files[name] = {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }
    return files;
}

bool Selected(const std::string& key) {
    return (key.size() == 37 && key[0] == 'N') || key == "Mshielded_frontier" ||
        key == "Mshielded_anchor_history" || key == "Mshielded_anchor_history_migrated_v1";
}
void Equivalent(const Rows& source, const fs::path& candidate) {
    auto actual = Inspect(candidate), expected = source;
    auto& destination = expected[shielded];
    for (auto it = expected["utreexo"].begin(); it != expected["utreexo"].end();) {
        if (Selected(it->first)) { destination.insert(*it); it = expected["utreexo"].erase(it); }
        else ++it;
    }
    CHECK(actual["meta"]["storage_layout_v1"] == ready);
    CHECK(!actual["meta"]["shielded_migration_v1"].empty());
    actual["meta"].erase("storage_layout_v1"); actual["meta"].erase("shielded_migration_v1");
    CHECK(actual == expected);
    ChainDB db; CHECK(db.init(candidate) == Status::Ok); CHECK(db.hasSeparatedShieldedState());
    CHECK(RequiredValue(db.countShieldedNullifiers()) == (source.at("utreexo").count(NullifierKey(1)) ? 2 : 0));
}
void Happy(const std::string& mode) {
    CanonicalTemp temp; const auto source = temp.path / "original", copy = temp.path / "copy";
    ValidSeed(source, mode == "empty");
    if (mode == "optional_absent") {
        Raw raw(source, legacy); Setup(raw.db->Delete({}, raw.cf("utreexo"), "Mshielded_anchor_history_migrated_v1").ok(), "remove optional");
    }
    Clone(source, copy); const auto before = Inspect(source);
    const auto files = PersistentFiles(source);
    auto budget = limits;
    if (mode == "byte_batches") { budget.batch_rows = 100; budget.batch_bytes = 1100; budget.max_record_bytes = 1100; }
    const auto report = MigrateShieldedStateCopy(source, copy, budget, mode != "inspect");
    if (!report.ok) std::cerr << report.error << "\n";
    CHECK(report.ok); CHECK(report.source_digest.size() == 64); CHECK(report.operation.size() == 64);
    CHECK(report.selected_rows == (mode == "empty" ? 3 : mode == "optional_absent" ? 4 : 5));
    CHECK(Inspect(source) == before);
    CHECK(PersistentFiles(source) == files);
    if (mode == "inspect") { CHECK(!report.ready); CHECK(Inspect(copy) == before); }
    else {
        CHECK(report.ready); CHECK(report.retired_rows == report.selected_rows); Equivalent(before, copy);
        const auto completed = Inspect(copy);
        const auto again = MigrateShieldedStateCopy(source, copy, limits, true);
        CHECK(again.ok && again.ready && again.operation == report.operation); CHECK(Inspect(copy) == completed);
    }
}
void Refusal(const std::string& fault) {
    CanonicalTemp temp; const auto source = temp.path / "original", copy = temp.path / "copy";
    ValidSeed(source);
    {
        Raw raw(source, legacy);
        if (fault == "short_nf") raw.put("utreexo", std::string("N\0", 2), "");
        if (fault == "long_nf") raw.put("utreexo", NullifierKey(1) + "x", "");
        if (fault == "nf_value") raw.put("utreexo", NullifierKey(1), "bad");
        if (fault == "duplicate_nf") { auto key = NullifierKey(1); key[4] = 2; raw.put("utreexo", key, ""); }
        if (fault == "trailing_frontier") raw.put("utreexo", "Mshielded_frontier", raw.rows().at("utreexo").at("Mshielded_frontier") + "trailing");
        if (fault == "frontier") raw.put("utreexo", "Mshielded_frontier", "invalid");
        if (fault == "anchors") raw.put("utreexo", "Mshielded_anchor_history", "invalid");
        if (fault == "missing_frontier") Setup(raw.db->Delete({}, raw.cf("utreexo"), "Mshielded_frontier").ok(), "delete");
        if (fault == "root" || fault == "count" || fault == "tree_size" || fault == "height") {
            auto bytes = raw.rows().at("meta").at("shielded_tip");
            bytes[fault == "root" ? 36 : fault == "count" ? 76 : fault == "tree_size" ? 68 : 0] ^= 1;
            raw.put("meta", "shielded_tip", bytes);
        }
        if (fault == "composite_root") {
            sh::CommitmentTree tree; sh::Hash note{}; note[31] = 7; tree.Append(note);
            sh::AnchorHistory anchors; anchors.RecordRoot(3, tree.Root());
            std::vector<sh::NullifierEntry> entries;
            for (uint32_t h : {1, 3}) {
                sh::NullifierEntry entry; entry.height = h; entry.nullifier.fill(h); entries.push_back(entry);
            }
            const auto tree_root = tree.Root();
            const auto composite = sh::ComputeShieldedRootFromParts(
                {tree_root.begin(), tree_root.end()}, tree.Size(), sh::ComputeNullifierAccumulator(entries), anchors.SerializeBytes());
            Setup(composite.has_value(), "composite root for negative control");
            auto bytes = raw.rows().at("meta").at("shielded_tip");
            Setup(std::memcmp(bytes.data() + 36, composite->data, 32) != 0, "roots must differ");
            std::memcpy(bytes.data() + 36, composite->data, 32);
            raw.put("meta", "shielded_tip", bytes);
        }
        if (fault == "forest_tip") raw.put("meta", "forest_tip", std::string(68, '\0'));
    }
    Clone(source, copy);
    if (fault == "copy_changed") { Raw raw(copy, legacy); raw.put("default", "UD:history", "changed"); }
    auto budget = limits;
    if (fault == "record_limit") budget.max_record_bytes = 1;
    if (fault == "nullifier_limit") budget.max_nullifiers = 1;
    if (fault == "zero_batch") budget.batch_rows = 0;
    const auto before = Inspect(source), candidate_before = Inspect(copy);
    const auto report = MigrateShieldedStateCopy(source, copy, budget, true);
    CHECK(!report.ok && !report.ready && !report.error.empty());
    CHECK(Inspect(source) == before && Inspect(copy) == candidate_before);
}
void Ownership(const std::string& fault) {
    CanonicalTemp temp; const auto source = temp.path / "original", copy = temp.path / "copy";
    ValidSeed(source); Clone(source, copy);
    const auto before = Inspect(source), candidate_before = Inspect(copy);
    fs::path src_arg = source, dst_arg = copy;
    ChainDB held;
    if (fault == "source_locked") Setup(held.init(source) == Status::Ok, "hold original");
    if (fault == "copy_locked") Setup(held.init(copy) == Status::Ok, "hold copy");
    if (fault == "same") dst_arg = source;
    if (fault == "nested") { dst_arg = source / "nested"; Clone(copy, dst_arg); }
    if (fault == "symlink") { dst_arg = temp.path / "link"; fs::create_directory_symlink(copy, dst_arg); }
    if (fault == "hardlink") fs::create_hard_link(source / "CURRENT", copy / "extra-link");
    const auto report = MigrateShieldedStateCopy(src_arg, dst_arg, limits, true);
    CHECK(!report.ok); held.close(); CHECK(Inspect(source) == before && Inspect(copy) == candidate_before);
}
int Child(const fs::path& source, const fs::path& copy, const std::string& stage, const char* mode = "--child") {
    const auto pid = fork(); Setup(pid >= 0, "fork");
    if (pid == 0) {
        execl(executable.c_str(), executable.c_str(), mode, source.c_str(), copy.c_str(), stage.c_str(), nullptr);
        _exit(127);
    }
    int status = 0; Setup(waitpid(pid, &status, 0) == pid, "waitpid");
    Setup(WIFEXITED(status), "child exited normally"); return WEXITSTATUS(status);
}
void Handoff() {
    CanonicalTemp temp; const auto source = temp.path / "original", copy = temp.path / "copy";
    ValidSeed(source); Clone(source, copy); unsigned probes = 0;
    const auto result = MigrateShieldedStateCopy(source, copy, limits, true, [&](const char* step) {
        const std::string stage(step);
        if (stage != "after_prepare" && stage != "after_create" && stage != "after_ready") return;
        ChainDB one, two; CHECK(one.init(source) != Status::Ok && two.init(copy) != Status::Ok);
        CHECK(Child(source, copy, "probe", "--probe") == 0); ++probes;
    });
    CHECK(result.ok && result.ready && probes == 3);
}
void ChangedSource() {
    CanonicalTemp temp; const auto source = temp.path / "original", copy = temp.path / "copy";
    ValidSeed(source); Clone(source, copy);
    CHECK(Child(source, copy, "after_retire") == 86);
    { Raw raw(source, legacy); raw.put("default", "UD:history", "changed rollback source"); }
    const auto before = Inspect(copy), source_before = Inspect(source);
    const auto result = MigrateShieldedStateCopy(source, copy, limits, true);
    CHECK(!result.ok); CHECK(Inspect(copy) == before && Inspect(source) == source_before);
}
void ReplacedIdentity() {
    CanonicalTemp temp; const auto source = temp.path / "original", copy = temp.path / "copy";
    ValidSeed(source); Clone(source, copy);
    CHECK(Child(source, copy, "after_retire") == 86);
    const auto before = Inspect(copy);
    fs::rename(copy, temp.path / "displaced");
    auto names = legacy; names.push_back(shielded);
    { Raw raw(copy, names); for (const auto& [name, rows] : before) for (const auto& [key, value] : rows) raw.put(name, key, value); }
    const auto result = MigrateShieldedStateCopy(source, copy, limits, true);
    CHECK(!result.ok); CHECK(Inspect(copy) == before && Inspect(temp.path / "displaced") == before);
}
void IOFailure(const std::string& fault, const std::string& stage) {
    CanonicalTemp temp; const auto source = temp.path / "original", copy = temp.path / "copy";
    ValidSeed(source); Clone(source, copy); const auto before = Inspect(source);
    const auto physical = PersistentFiles(source);
    FaultEnv env(copy, fault);
    const auto result = dinero::storage::MigrateShieldedStateCopyForTesting(source, copy, limits,
        [&](const char* step) { if (stage == step) env.armed = true; }, env);
    CHECK(env.fired == 1); CHECK(!result.ok && !result.ready && !result.error.empty());
    CHECK(Inspect(source) == before && PersistentFiles(source) == physical);
    // Reopen in another process: don't rely on the failed writer's memtables.
    CHECK(Child(source, copy, "resume") == 0); Equivalent(before, copy);
}
void Interrupt(const std::string& stage, const std::string& corruption = "") {
    CanonicalTemp temp; const auto source = temp.path / "original", copy = temp.path / "copy";
    ValidSeed(source); Clone(source, copy); const auto before = Inspect(source);
    CHECK(Child(source, copy, stage) == 86);
    const auto interrupted = Inspect(copy);
    ChainDB db;
    if (stage == "after_ready") { CHECK(db.init(copy) == Status::Ok); db.close(); }
    else { CHECK(db.init(copy) != Status::Ok); CHECK(Inspect(copy) == interrupted); }
    if (!corruption.empty()) {
        auto names = legacy; names.push_back(shielded); Raw raw(copy, names);
        if (corruption == "conflict") raw.put(shielded, "Mshielded_anchor_history", "conflicting");
        if (corruption == "unknown") raw.put(shielded, "unexpected", "record");
        if (corruption == "unselected") raw.put("utreexo", "Sreplay", "changed");
        if (corruption == "lost_both") {
            Setup(raw.db->Delete({}, raw.cf("utreexo"), "Mshielded_frontier").ok(), "delete source copy");
            Setup(raw.db->Delete({}, raw.cf(shielded), "Mshielded_frontier").ok(), "delete target copy");
        }
        if (corruption == "journal") raw.put("meta", "shielded_migration_v1", "invalid");
    }
    const auto damaged = Inspect(copy);
    CHECK(Child(source, copy, "resume") == (corruption.empty() ? 0 : 1));
    CHECK(Inspect(source) == before);
    if (corruption.empty()) Equivalent(before, copy); else CHECK(Inspect(copy) == damaged);
}

int main(int argc, char** argv) {
    executable = fs::absolute(argv[0]).string();
    if (argc == 5 && std::string(argv[1]) == "--probe") {
        ChainDB one, two; return one.init(argv[2]) != Status::Ok && two.init(argv[3]) != Status::Ok ? 0 : 1;
    }
    if (argc == 5 && std::string(argv[1]) == "--child") {
        const auto result = MigrateShieldedStateCopy(argv[2], argv[3], limits, true, [&](const char* stage) {
            if (stage == std::string(argv[4])) _exit(86);
        });
        if (!result.ok) std::cerr << result.error << '\n'; return result.ok ? 0 : 1;
    }
    std::map<std::string, std::function<void()>> cases;
    for (const std::string mode : {"inspect", "move", "empty", "optional_absent", "byte_batches"}) cases[mode] = [=] { Happy(mode); };
    cases["lock_handoff"] = Handoff;
    cases["changed_source"] = ChangedSource;
    cases["replaced_identity"] = ReplacedIdentity;
    for (const std::string fault : {"append", "sync", "ack"})
        for (const std::string stage : {"after_create", "before_copy", "after_readback", "before_ready"})
            cases["io_" + fault + "_" + stage] = [=] { IOFailure(fault, stage); };
    for (const std::string fault : {"short_nf", "long_nf", "nf_value", "duplicate_nf", "frontier", "trailing_frontier", "anchors", "missing_frontier", "root", "composite_root", "count", "tree_size", "height", "forest_tip", "copy_changed", "record_limit", "nullifier_limit", "zero_batch"}) cases[fault] = [=] { Refusal(fault); };
    for (const std::string fault : {"source_locked", "copy_locked", "same", "nested", "symlink", "hardlink"}) cases[fault] = [=] { Ownership(fault); };
    for (const std::string stage : {"after_prepare", "after_create", "after_copy", "after_readback", "after_retire", "after_verifying", "before_ready", "after_ready"}) cases[stage] = [=] { Interrupt(stage); };
    for (const std::string fault : {"conflict", "unknown", "unselected", "lost_both", "journal"}) cases[fault] = [=] { Interrupt("after_copy", fault); };
    if (argc == 2 && std::string(argv[1]) == "--list") { for (const auto& [name, test] : cases) std::cout << name << '\n'; return 0; }
    unsigned passed = 0, failed = 0;
    for (const auto& [name, test] : cases) {
        if (argc == 2 && name != argv[1]) continue;
        try { test(); ++passed; std::cout << "PASS " << name << '\n'; }
        catch (const Failure& e) { ++failed; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
        catch (const std::exception& e) { std::cerr << "SETUP ERROR " << name << ": " << e.what() << '\n'; return 2; }
    }
    if (!passed && !failed) return 2;
    std::cout << passed << " passed, " << failed << " failed\n"; return failed ? 1 : 0;
}
