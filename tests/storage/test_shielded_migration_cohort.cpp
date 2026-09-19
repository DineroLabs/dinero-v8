// Generated datadirs only. Companion bytes are opaque fixtures, not a claim
// that a real daemon/snapshot or forest reconstruction was qualified here.
#include "shielded_migration_fixture.h"
#include "storage/shielded_migration_cohort.h"
#include "daemon/datadir_guard.h"
#include <fstream>
#include <sys/wait.h>

using namespace shielded_store_fixture;
using dinero::daemon::DatadirGuard;
using dinero::storage::MigrateShieldedDatadirCopy;
constexpr dinero::storage::ShieldedMigrationLimits db_limits{32768, 1, 16384, 100};
constexpr dinero::storage::ShieldedCompanionLimits file_limits{100, 1024 * 1024};
std::string executable;

void Write(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path()); std::ofstream out(path, std::ios::binary);
    out << bytes; Setup(out.good(), "write fixture");
}
std::string Read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary); Setup(in.good(), "read fixture");
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
struct Fixture {
    CanonicalTemp temp;
    fs::path original = temp.path / "original", candidate = temp.path / "candidate";
    Fixture() {
        fs::create_directories(original / "blockchain"); fs::create_directories(candidate / "blockchain");
        ValidSeed(original / "blockchain/chaindb");
        Clone(original / "blockchain/chaindb", candidate / "blockchain/chaindb");
        for (const auto& root : {original, candidate}) {
            DatadirGuard guard; std::string error; Setup(guard.Acquire(root, error), "seed daemon lock");
            guard.Release();
            Write(root / "blocks/blk00000.dat", "retained body");
            Write(root / "headers/fixture", "retained headers");
            Write(root / "blockchain/utxo", "opaque SQLite fixture");
            Write(root / "blockchain/shielded_frontier.bin", "unchanged legacy file");
            Write(root / "checkpoints/mainnet/snapshot", "retained import input");
        }
        // Outside the migration's chain-companion inventory: do not open these.
        fs::create_symlink("missing-secret", original / "wallets");
        Write(original / "dinerod.pid", "30000000\n");
        Write(candidate / "dinerod.pid", "30000001\n");
    }
    auto Run(bool apply = true, const std::function<void(const char*)>& hook = {}) {
        return MigrateShieldedDatadirCopy(original, candidate, db_limits, file_limits, apply, hook);
    }
};
int Child(const std::vector<std::string>& args) {
    std::vector<char*> argv{const_cast<char*>(executable.c_str())};
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    const pid_t pid = fork(); Setup(pid >= 0, "fork");
    if (pid == 0) {
        // No allocation or RocksDB calls after fork with engine threads alive.
        execv(executable.c_str(), argv.data()); _exit(127);
    }
    int status = 0; Setup(waitpid(pid, &status, 0) == pid && WIFEXITED(status), "child exit");
    return WEXITSTATUS(status);
}
void Success(bool inspect) {
    Fixture f; const auto original = Inspect(f.original / "blockchain/chaindb");
    const auto result = f.Run(!inspect);
    if (!result.ok) std::cerr << result.error << '\n';
    CHECK(result.ok); CHECK(result.ready == !inspect);
    CHECK(Inspect(f.original / "blockchain/chaindb") == original);
    CHECK(Read(f.original / "dinerod.pid") == "30000000\n");
    CHECK(Read(f.candidate / "dinerod.pid") == "30000001\n");
    CHECK(Read(f.candidate / "blocks/blk00000.dat") == "retained body");
    CHECK(fs::is_symlink(f.original / "wallets"));
    if (!inspect) CHECK(f.Run().ok);
    DatadirGuard guard; std::string error; CHECK(guard.Acquire(f.candidate, error));
}
void Refusal(const std::string& fault) {
    Fixture f; DatadirGuard guard; std::string error;
    auto source = f.original, copy = f.candidate;
    auto budget = file_limits;
    if (fault == "source_running" || fault == "copy_running")
        Setup(guard.Acquire(fault == "source_running" ? source : copy, error), "hold daemon guard");
    else if (fault == "recovery") Write(copy / "chainstate_recovery.marker", "incomplete");
    else if (fault == "reindex") Write(source / "blockchain/reindex_promotion.marker", "incomplete");
    else if (fault == "reindex_temporary") Write(copy / "blockchain/shielded_frontier.bin.reindex.tmp", "partial");
    else if (fault == "maintenance") Write(f.temp.path / ".nodecore-maintenance-v1/intent.json", "unknown");
    else if (fault == "different_body") Write(copy / "blocks/blk00000.dat", "mismatched body");
    else if (fault == "missing_header") fs::remove(copy / "headers/fixture");
    else if (fault == "extra_companion") Write(copy / "blockchain/future-file", "unexplained");
    else if (fault == "missing_lock") fs::remove(copy / "dinerod.lock");
    else if (fault == "lock_symlink") { fs::remove(copy / "dinerod.lock"); fs::create_symlink(source / "dinerod.lock", copy / "dinerod.lock"); }
    else if (fault == "file_symlink") { fs::remove(copy / "blocks/blk00000.dat"); fs::create_symlink(source / "blocks/blk00000.dat", copy / "blocks/blk00000.dat"); }
    else if (fault == "file_hardlink") { fs::remove(copy / "blocks/blk00000.dat"); fs::create_hard_link(source / "blocks/blk00000.dat", copy / "blocks/blk00000.dat"); }
    else if (fault == "same") copy = source;
    else if (fault == "root_symlink") { fs::create_directory_symlink(copy, f.temp.path / "alias"); copy = f.temp.path / "alias"; }
    else if (fault == "entry_budget") budget.max_entries = 1;
    else if (fault == "byte_budget") budget.max_bytes = 1;
    else if (fault == "zero_budget") budget.max_bytes = 0;
    else if (fault == "dirty_sqlite") for (const auto& root : {source, copy}) Write(root / "blockchain/utxo-wal", "pending");
    else Setup(false, "unknown refusal");
    const auto before = Inspect(f.candidate / "blockchain/chaindb");
    const auto result = MigrateShieldedDatadirCopy(source, copy, db_limits, budget, true);
    CHECK(!result.ok); CHECK(!result.ready); CHECK(!result.error.empty());
    CHECK(Inspect(f.candidate / "blockchain/chaindb") == before);
}
void Ownership() {
    Fixture f; unsigned stages = 0;
    const auto result = f.Run(true, [&](const char* stage) {
        if (std::string(stage) != "after_prepare" && std::string(stage) != "after_ready") return;
        ++stages;
        for (const auto& root : {f.original, f.candidate}) {
            DatadirGuard guard; std::string error; CHECK(!guard.Acquire(root, error));
            CHECK(Child({"--acquire", root.string()}) == 10);
        }
    });
    CHECK(result.ok); CHECK(stages == 2);
}
void Interrupt(const std::string& mutation) {
    Fixture f;
    CHECK(Child({"--interrupt", f.original.string(), f.candidate.string()}) == 86);
    const auto before = Inspect(f.candidate / "blockchain/chaindb");
    if (mutation == "source" || mutation == "both") Write(f.original / "blocks/blk00000.dat", "new retained body");
    if (mutation == "candidate" || mutation == "both") Write(f.candidate / "blocks/blk00000.dat", "new retained body");
    const auto result = f.Run();
    if (mutation.empty()) CHECK(result.ok && result.ready);
    else { CHECK(!result.ok); CHECK(Inspect(f.candidate / "blockchain/chaindb") == before); }
}
void During(const std::string& fault) {
    Fixture f;
    const auto result = f.Run(true, [&](const char* stage) {
        if (std::string(stage) != "before_ready") return;
        if (fault == "replace_lock") {
            fs::rename(f.candidate / "dinerod.lock", f.candidate / "old.lock");
            Write(f.candidate / "dinerod.lock", "");
        } else if (fault == "add_marker") Write(f.candidate / "chainstate_recovery.marker", "unexpected");
        else for (const auto& root : {f.original, f.candidate}) Write(root / "blocks/blk00000.dat", "new retained body");
    });
    CHECK(!result.ok); CHECK(!result.ready);
    CHECK(Inspect(f.candidate / "blockchain/chaindb")["meta"]["storage_layout_v1"] != ready);
}
void CannotBypassBinding() {
    Fixture f; CHECK(Child({"--interrupt", f.original.string(), f.candidate.string()}) == 86);
    const auto before = Inspect(f.candidate / "blockchain/chaindb");
    const auto result = dinero::storage::MigrateShieldedStateCopy(f.original / "blockchain/chaindb",
        f.candidate / "blockchain/chaindb", db_limits, true);
    CHECK(!result.ok); CHECK(Inspect(f.candidate / "blockchain/chaindb") == before);
}
int main(int argc, char** argv) {
    executable = fs::canonical(argv[0]);
    if (argc == 3 && std::string(argv[1]) == "--acquire") {
        DatadirGuard guard; std::string error; return guard.Acquire(argv[2], error) ? 0 : 10;
    }
    if (argc == 4 && std::string(argv[1]) == "--interrupt") {
        const auto result = MigrateShieldedDatadirCopy(argv[2], argv[3], db_limits, file_limits, true,
            [](const char* stage) { if (std::string(stage) == "after_copy") _exit(86); });
        return result.ok ? 0 : 1;
    }
    std::map<std::string, std::function<void()>> cases;
    cases["inspect"] = [] { Success(true); }; cases["move"] = [] { Success(false); };
    cases["ownership"] = Ownership; cases["resume"] = [] { Interrupt(""); };
    cases["no_unbound_resume"] = CannotBypassBinding;
    for (const std::string change : {"source", "candidate", "both"}) cases["resume_changed_" + change] = [=] { Interrupt(change); };
    for (const std::string change : {"replace_lock", "add_marker", "bytes"}) cases["during_" + change] = [=] { During(change); };
    for (const std::string fault : {"source_running", "copy_running", "recovery", "reindex", "reindex_temporary", "maintenance", "different_body", "missing_header", "extra_companion", "missing_lock", "lock_symlink", "file_symlink", "file_hardlink", "same", "root_symlink", "entry_budget", "byte_budget", "zero_budget", "dirty_sqlite"}) cases[fault] = [=] { Refusal(fault); };
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
