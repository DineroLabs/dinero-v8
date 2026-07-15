/**
 * #371: ChainDB::writeBatch must recover from a latched RocksDB background
 * error where possible, and fail LOUDLY where not — never zombie.
 *
 * Incident (EU1, 2026-07-04): one transient EBADF during an sst flush latched
 * RocksDB's background error; every subsequent Write() failed silently until a
 * manual daemon restart 3+ hours later. writeBatch discarded the status string,
 * never attempted DB::Resume(), and never escalated.
 *
 * RocksDB latch severities (db/error_handler.cc) dictate the two test shapes:
 *  - kHardError (e.g. flush NoSpace, no SstFileManager): recoverable by a
 *    MANUAL DB::Resume() once the cause clears — rocksdb does NOT auto-heal
 *    this class and pre-#371 dinerod never called Resume() → zombie.
 *    → Test 1: Resume + retry must recover writeBatch after the env heals.
 *  - kFatalError (plain flush IOError — the literal EU1 EBADF class): even
 *    Resume() refuses; only a process restart (DB reopen) clears it. The node
 *    must escalate loudly so systemd/watchdog restarts it.
 *    → Test 2: loud-failure hook fires at the consecutive-failure threshold,
 *      and a reopen (the restart analogue) restores service + resets streak.
 *
 * NOTE: checks exit non-zero (no assert(); assert is a no-op under NDEBUG).
 */

#include "storage/chain_db.h"
#include "storage/chain_write_token.h"

#include <rocksdb/env.h>
#include <rocksdb/status.h>
#include <rocksdb/write_batch.h>

#include <atomic>
#include <memory>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>

using dinero::ChainDB;
using dinero::ChainWriteToken;
using dinero::Status;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::cerr << "FAIL: " << msg << " (" << #cond << ") at line "    \
                      << __LINE__ << std::endl;                              \
            std::exit(1);                                                    \
        }                                                                    \
    } while (0)

namespace {

// Fails creation of NEW .sst files (and nothing else) while `fail_sst` is set —
// the precise EU1 failure point: WAL and manifest writes stay healthy, only the
// background flush's sst write dies, so rocksdb latches with reason kFlush.
class SstFaultEnv : public rocksdb::EnvWrapper {
public:
    explicit SstFaultEnv(rocksdb::Env* base) : rocksdb::EnvWrapper(base) {}

    std::atomic<bool> fail_sst{false};
    rocksdb::Status injected_error = rocksdb::Status::IOError("injected sst failure");

    rocksdb::Status NewWritableFile(const std::string& fname,
                                    std::unique_ptr<rocksdb::WritableFile>* result,
                                    const rocksdb::EnvOptions& options) override {
        if (fail_sst.load() && fname.size() >= 4 &&
            fname.compare(fname.size() - 4, 4, ".sst") == 0) {
            return injected_error;
        }
        return target()->NewWritableFile(fname, result, options);
    }
};

std::filesystem::path MakeTempDir(const std::string& tag) {
    auto dir = std::filesystem::temp_directory_path() /
               ("dinero_chaindb_371_" + tag + "_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

Status PutOne(ChainDB& db, const ChainWriteToken& token, const std::string& key,
              const std::string& value, bool sync) {
    rocksdb::WriteBatch batch;
    batch.Put(key, value);
    return db.writeBatch(token, std::move(batch), sync);
}

void TestResumeRecoversHardError() {
    std::cout << "[Test 1] bounded recovery wait clears a NoSpace-class latch after env heals"
              << std::endl;

    SstFaultEnv env(rocksdb::Env::Default());
    auto dir = MakeTempDir("recover");

    ChainDB db;
    db.setEnvForTesting(&env);
    CHECK(db.init(dir) == Status::Ok, "ChainDB::init on healthy env");

    const ChainWriteToken token = ChainWriteToken::CreateForTesting();

    CHECK(PutOne(db, token, "k_healthy", "v1", /*sync=*/true) == Status::Ok,
          "baseline write on healthy env");

    // Fail the flush's sst write with NoSpace: rocksdb latches a kHardError
    // background error (writes blocked) and arms its NoSpace auto-recovery
    // (SstFileManager free-space polling). Pre-#371 code fails the caller
    // immediately and forever until some later tick happens to win the race;
    // fixed code rides the recovery out within one writeBatch call.
    env.injected_error = rocksdb::Status::NoSpace("injected no-space");
    env.fail_sst = true;
    CHECK(db.flushForTesting() != Status::Ok,
          "flush while sst writes are broken must fail (latches the background error)");
    CHECK(PutOne(db, token, "k_broken", "v2", /*sync=*/true) != Status::Ok,
          "write under the latch must fail");

    // Heal the filesystem. The ONLY thing standing between the node and
    // progress is the latched error; fixed code must Resume() and succeed.
    env.fail_sst = false;
    CHECK(PutOne(db, token, "k_after_heal", "v3", /*sync=*/true) == Status::Ok,
          "write after env heals must succeed (Resume + retry)");

    std::string readback;
    CHECK(db.getRaw("k_after_heal", readback) == Status::Ok && readback == "v3",
          "healed write must be durable and readable");

    db.close();
    std::filesystem::remove_all(dir);
    std::cout << "  PASS" << std::endl;
}

void TestFatalHookFiresAfterThreshold() {
    std::cout << "[Test 2] loud-failure hook fires at threshold on an unrecoverable latch"
              << std::endl;

    SstFaultEnv env(rocksdb::Env::Default());
    auto dir = MakeTempDir("fatal");

    ChainDB db;
    db.setEnvForTesting(&env);
    CHECK(db.init(dir) == Status::Ok, "ChainDB::init on healthy env");

    const ChainWriteToken token = ChainWriteToken::CreateForTesting();
    CHECK(PutOne(db, token, "k_seed", "v", /*sync=*/true) == Status::Ok, "seed write");

    int hook_calls = 0;
    std::string hook_reason;
    db.setFatalWriteFailureHook([&](const std::string& reason) {
        ++hook_calls;
        hook_reason = reason;
    });

    // Plain flush IOError = the literal EU1 EBADF class → kFatalError latch.
    // Resume() cannot clear it (verified severity in db/error_handler.cc);
    // only a reopen can. The node must escalate, not spin.
    env.injected_error = rocksdb::Status::IOError("injected io failure");
    env.fail_sst = true;
    CHECK(db.flushForTesting() != Status::Ok,
          "flush while sst writes are broken must fail (latches fatal background error)");

    const int threshold = ChainDB::kMaxConsecutiveWriteFailures;
    for (int i = 0; i < threshold - 1; ++i) {
        CHECK(PutOne(db, token, "k_fail", "v", /*sync=*/true) != Status::Ok,
              "write must fail under the fatal latch");
        CHECK(hook_calls == 0, "hook must NOT fire below the threshold");
    }
    CHECK(PutOne(db, token, "k_fail", "v", /*sync=*/true) != Status::Ok,
          "threshold-crossing write must fail");
    CHECK(hook_calls == 1, "hook must fire exactly once at the threshold");
    CHECK(!hook_reason.empty(), "hook must receive the rocksdb reason string");

    CHECK(PutOne(db, token, "k_fail", "v", /*sync=*/true) != Status::Ok,
          "post-threshold write still fails");
    CHECK(hook_calls == 1, "hook must not re-fire while the same streak continues");

    // The restart analogue: heal the env and REOPEN the DB — this is the
    // recovery that healed EU1. Service resumes and the failure streak resets.
    env.fail_sst = false;
    db.close();
    CHECK(db.init(dir) == Status::Ok, "reopen after heal (the restart analogue)");
    CHECK(PutOne(db, token, "k_reset", "v", /*sync=*/true) == Status::Ok,
          "write after reopen succeeds");

    env.fail_sst = true;
    CHECK(db.flushForTesting() != Status::Ok, "re-latch via second broken flush");
    CHECK(PutOne(db, token, "k_fail2", "v", /*sync=*/true) != Status::Ok,
          "single failure after reset");
    CHECK(hook_calls == 1, "streak reset: one post-recovery failure must not re-fire hook");

    env.fail_sst = false;
    db.close();
    std::filesystem::remove_all(dir);
    std::cout << "  PASS" << std::endl;
}

void TestFatalHookOnDatadirDeletedUnderLiveDB() {
    std::cout << "[Test 3] datadir deleted under a live DB escalates via hook, not exit"
              << std::endl;

    // The on-device 2026-07-15 incident shape: the host app's self-heal wiped
    // the node datadir while the node was RUNNING. RocksDB latched
    // ("No such file or directory: While open a file for appending"), and —
    // with no hook installed — the loop-breaker's std::exit(1) took the whole
    // host app down. An EMBEDDED node must escalate through the hook and
    // leave the process alive; this test IS the process-alive assertion (it
    // keeps executing past the threshold).
    auto dir = MakeTempDir("wiped");

    ChainDB db;
    CHECK(db.init(dir) == Status::Ok, "ChainDB::init on real filesystem");

    const ChainWriteToken token = ChainWriteToken::CreateForTesting();
    CHECK(PutOne(db, token, "k_seed", "v", /*sync=*/true) == Status::Ok, "seed write");

    int hook_calls = 0;
    db.setFatalWriteFailureHook([&](const std::string&) { ++hook_calls; });

    // Delete the entire datadir under the open DB (the self-heal wipe).
    std::filesystem::remove_all(dir);

    // Force filesystem activity in the vanished directory: the flush must
    // fail (no directory to create the sst in) and latch a background error.
    CHECK(db.flushForTesting() != Status::Ok,
          "flush into a deleted datadir must fail and latch");

    const int threshold = ChainDB::kMaxConsecutiveWriteFailures;
    for (int i = 0; i < threshold; ++i) {
        CHECK(PutOne(db, token, "k_gone", "v", /*sync=*/true) != Status::Ok,
              "write into a deleted datadir must fail");
    }
    CHECK(hook_calls == 1,
          "hook must fire exactly once at the threshold (and the process must "
          "still be alive to check it — exit(1) here killed the iOS app)");

    db.close();
    std::filesystem::remove_all(dir);
    std::cout << "  PASS" << std::endl;
}

}  // namespace

int main() {
    TestResumeRecoversHardError();
    TestFatalHookFiresAfterThreshold();
    TestFatalHookOnDatadirDeletedUnderLiveDB();
    std::cout << "All #371 write-failure recovery tests passed" << std::endl;
    return 0;
}
