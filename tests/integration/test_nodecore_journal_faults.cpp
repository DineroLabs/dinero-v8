// The separate subject object compiles the production journal verbatim, with
// POSIX calls redirected only in that object. No hooks enter nodecore_ffi.
#include "nodecore/maintenance_journal.h"
#include "nodecore/nodecore_ffi.h"
#include <gtest/gtest.h>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
using dinero::nodecore::MaintenanceJournal;
std::string executable;
struct Fault {
    std::string call;
    unsigned occurrence = 0;
    unsigned seen = 0;
    bool always = false;
    std::string mode = "error";
    int error = EIO;
} fault;
bool Selected(const char* call) {
    if (fault.call != call) return false;
    ++fault.seen;
    if (!fault.always && fault.seen != fault.occurrence) return false;
    return true;
}
void Arm(const std::string& call, unsigned occurrence, bool always = false,
         const std::string& mode = "error", int error = EIO) {
    fault = {call, occurrence, 0, always, mode, error};
}
bool Before(bool selected) {
    if (!selected) return false;
    if (fault.mode == "crash-before") ::_exit(86);
    if (fault.mode == "error") { errno = fault.error; return true; }
    return false;
}
int After(int result, bool selected) {
    if (selected && result == 0) {
        if (fault.mode == "crash-after") ::_exit(86);
        if (fault.mode == "error-after") { errno = fault.error; return -1; }
    }
    return result;
}
struct Cut {
    const char* call;
    unsigned occurrence;
    const char* mode;
    bool completed_visible;
    int error = EIO;
};
int Spawn(const std::string& action, const fs::path& target, Cut cut) {
    const auto occurrence = std::to_string(cut.occurrence);
    const auto error = std::to_string(cut.error);
    const auto child = ::fork();
    if (child < 0) return -1000;
    if (child == 0) {
        ::execl(executable.c_str(), executable.c_str(), "--journal-child", action.c_str(),
                target.c_str(), cut.call, occurrence.c_str(), cut.mode, error.c_str(), nullptr);
        ::_exit(127);
    }
    int status = 0;
    if (::waitpid(child, &status, 0) != child || !WIFEXITED(status)) return -1001;
    return WEXITSTATUS(status);
}

class JournalFaults : public testing::Test {
protected:
    fs::path parent;
    fs::path target;
    MaintenanceJournal journal;
    void SetUp() override {
        char pattern[] = "/tmp/dinero-journal-fault-XXXXXX";
        const auto created = ::mkdtemp(pattern);
        ASSERT_NE(created, nullptr);
        parent = fs::canonical(created);
        target = parent / "node";
        fs::create_directory(target);
        std::ofstream(target / "protected-state") << protected_bytes;
        fault = {};
        ASSERT_EQ(journal.Prepare(target.string()), NODECORE_OK);
    }
    void TearDown() override {
        fault = {};
        if (!target.empty()) {
            std::ifstream input(target / "protected-state");
            const std::string actual{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
            EXPECT_EQ(actual, protected_bytes);
        }
        if (!parent.empty()) fs::remove_all(parent);
    }
    int FreshGate(bool fail_sync) {
        return Spawn("gate", target, {fail_sync ? "fsync" : "none", 1, "error", false});
    }
    const std::string protected_bytes = "utreexo/shielded fixture stays unchanged\n";
};

TEST_F(JournalFaults, VisibleCompletionCannotBypassContinuingSyncFailureAfterExec) {
    // Store syncs parent, staged file, then control directory after rename.
    Arm("fsync", 3);
    EXPECT_EQ(journal.CompleteUnchanged(), NODECORE_ERROR_MAINTENANCE_IO);
    EXPECT_EQ(fault.seen, 3U);
    fault = {};
    EXPECT_EQ(FreshGate(true), 9) << "visible is not a successful durability check";
    EXPECT_EQ(FreshGate(false), 0) << "verified completion can roll forward after persistence succeeds";
}

TEST_F(JournalFaults, SameOwnerCanRetryCompletionAfterUnacknowledgedRename) {
    Arm("fsync", 3);
    ASSERT_EQ(journal.CompleteUnchanged(), NODECORE_ERROR_MAINTENANCE_IO);
    fault = {};
    EXPECT_EQ(journal.CompleteUnchanged(), NODECORE_OK);
    EXPECT_EQ(FreshGate(false), 0);
}

TEST_F(JournalFaults, RetryDoesNotAcceptChangedContents) {
    Arm("fsync", 3);
    ASSERT_EQ(journal.CompleteUnchanged(), NODECORE_ERROR_MAINTENANCE_IO);
    fault = {};
    std::ofstream(target / "protected-state") << "unexpected modification";
    EXPECT_EQ(journal.CompleteUnchanged(), NODECORE_ERROR_MAINTENANCE_VALIDATION);
    std::ofstream(target / "protected-state") << protected_bytes;
    EXPECT_EQ(journal.CompleteUnchanged(), NODECORE_OK);
}

TEST_F(JournalFaults, RetryDoesNotAcceptAnotherOperationsReceipt) {
    Arm("fsync", 3);
    ASSERT_EQ(journal.CompleteUnchanged(), NODECORE_ERROR_MAINTENANCE_IO);
    fault = {};
    MaintenanceJournal other;
    ASSERT_EQ(other.Prepare(target.string()), NODECORE_OK);
    ASSERT_EQ(other.CompleteUnchanged(), NODECORE_OK);
    EXPECT_EQ(journal.CompleteUnchanged(), NODECORE_ERROR_MAINTENANCE_VALIDATION);
}

TEST_F(JournalFaults, ShortWritesAndEintrAreRetriedWithoutChangingRecord) {
    Arm("write", 1, false, "short");
    EXPECT_EQ(journal.CompleteUnchanged(), NODECORE_OK);
    EXPECT_GE(fault.seen, 2U);
    fault = {};
    ASSERT_EQ(journal.Prepare(target.string()), NODECORE_OK);
    Arm("write", 1, false, "error", EINTR);
    EXPECT_EQ(journal.CompleteUnchanged(), NODECORE_OK);
    EXPECT_GE(fault.seen, 2U);
    fault = {};
    EXPECT_EQ(FreshGate(false), 0);
}

TEST_F(JournalFaults, FailedNewPreparationGrantsNoNewPermission) {
    ASSERT_EQ(journal.CompleteUnchanged(), NODECORE_OK);
    // First three syncs acknowledge the prior completed receipt. The next
    // fails before a new prepared record or token can be issued. It need not
    // invalidate the old verified completion: no maintenance was authorized.
    Arm("fsync", 4);
    EXPECT_EQ(journal.Prepare(target.string()), NODECORE_ERROR_MAINTENANCE_IO);
    EXPECT_EQ(fault.seen, 4U);
    fault = {};
    EXPECT_EQ(FreshGate(false), 0);
}

class CompletionErrors : public JournalFaults, public testing::WithParamInterface<Cut> {};
TEST_P(CompletionErrors, ErrorNeverBypassesAnUnresolvedBarrier) {
    const auto c = GetParam();
    Arm(c.call, c.occurrence, false, c.mode, c.error);
    EXPECT_EQ(journal.CompleteUnchanged(), NODECORE_ERROR_MAINTENANCE_IO);
    EXPECT_EQ(fault.seen, c.occurrence);
    fault = {};
    EXPECT_EQ(FreshGate(true), 9);
    EXPECT_EQ(FreshGate(false), c.completed_visible ? 0 : 9);
    if (c.completed_visible) EXPECT_EQ(journal.CompleteUnchanged(), NODECORE_OK);
}
INSTANTIATE_TEST_SUITE_P(WriteBoundaries, CompletionErrors, testing::Values(
    Cut{"fsync", 1, "error", false}, Cut{"write", 1, "error", false},
    Cut{"write", 1, "error", false, ENOSPC}, Cut{"write", 1, "zero", false},
    Cut{"fsync", 2, "error", false}, Cut{"renameat", 1, "error", false},
    Cut{"renameat", 1, "error-after", true}, Cut{"fsync", 3, "error", true},
    Cut{"fsync", 3, "error-after", true}));

class ProcessCuts : public JournalFaults, public testing::WithParamInterface<Cut> {};
TEST_P(ProcessCuts, AbruptExitThenExecReadsOnlyAValidTransition) {
    const auto c = GetParam();
    ASSERT_EQ(Spawn("complete", target, c), 86) << "cut must actually execute";
    EXPECT_EQ(FreshGate(true), 9);
    EXPECT_EQ(FreshGate(false), c.completed_visible ? 0 : 9);
}
INSTANTIATE_TEST_SUITE_P(CompletionCuts, ProcessCuts, testing::Values(
    Cut{"fsync", 1, "crash-before", false}, Cut{"fsync", 1, "crash-after", false},
    Cut{"write", 1, "crash-before", false}, Cut{"write", 1, "crash-after", false},
    Cut{"fsync", 2, "crash-before", false}, Cut{"fsync", 2, "crash-after", false},
    Cut{"renameat", 1, "crash-before", false}, Cut{"renameat", 1, "crash-after", true},
    Cut{"fsync", 3, "crash-before", true}, Cut{"fsync", 3, "crash-after", true}));

class StartupSyncErrors : public JournalFaults, public testing::WithParamInterface<unsigned> {};
TEST_P(StartupSyncErrors, EachDurabilityBoundaryMustSucceed) {
    ASSERT_EQ(journal.CompleteUnchanged(), NODECORE_OK);
    EXPECT_EQ(Spawn("gate", target, {"fsync", GetParam(), "error", true}), 9);
    EXPECT_EQ(FreshGate(false), 0);
}
INSTANTIATE_TEST_SUITE_P(CompletedReceipt, StartupSyncErrors, testing::Values(1U, 2U, 3U));

class PrepareErrors : public JournalFaults, public testing::WithParamInterface<Cut> {};
TEST_P(PrepareErrors, NoPermissionFollowsFailedPreparation) {
    const auto other = parent / "separate" / "node";
    fs::create_directories(other);
    const auto c = GetParam();
    ASSERT_EQ(Spawn("prepare", other, c), 12);
    EXPECT_EQ(Spawn("gate", other, {"none", 1, "error", false}), 9);
}
INSTANTIATE_TEST_SUITE_P(PreparationBoundaries, PrepareErrors, testing::Values(
    Cut{"fsync", 1, "error", false}, Cut{"write", 1, "error", false},
    Cut{"fsync", 2, "error", false}, Cut{"renameat", 1, "error", false},
    Cut{"renameat", 1, "error-after", false}, Cut{"fsync", 3, "error", false}));

class PrepareCuts : public JournalFaults, public testing::WithParamInterface<Cut> {};
TEST_P(PrepareCuts, InterruptedPreparationAlwaysBlocksTheFreshReader) {
    const auto other = parent / "separate" / "node";
    fs::create_directories(other);
    ASSERT_EQ(Spawn("prepare", other, GetParam()), 86);
    EXPECT_EQ(Spawn("gate", other, {"none", 1, "error", false}), 9);
}
INSTANTIATE_TEST_SUITE_P(PreparationCuts, PrepareCuts, testing::Values(
    Cut{"fsync", 1, "crash-before", false}, Cut{"fsync", 1, "crash-after", false},
    Cut{"write", 1, "crash-before", false}, Cut{"write", 1, "crash-after", false},
    Cut{"fsync", 2, "crash-before", false}, Cut{"fsync", 2, "crash-after", false},
    Cut{"renameat", 1, "crash-before", false}, Cut{"renameat", 1, "crash-after", false},
    Cut{"fsync", 3, "crash-before", false}, Cut{"fsync", 3, "crash-after", false}));
} // namespace

extern "C" int nodecore_test_fsync(int fd) {
    const bool selected = Selected("fsync");
    if (Before(selected)) return -1;
    return After(::fsync(fd), selected);
}
extern "C" int nodecore_test_renameat(int from_dir, const char* from, int to_dir, const char* to) {
    const bool selected = Selected("renameat");
    if (Before(selected)) return -1;
    return After(::renameat(from_dir, from, to_dir, to), selected);
}
extern "C" ssize_t nodecore_test_write(int fd, const void* data, size_t size) {
    const bool selected = Selected("write");
    if (Before(selected)) return -1;
    if (selected && fault.mode == "zero") return 0;
    const auto result = ::write(fd, data, selected && fault.mode == "short" ? size / 2 : size);
    if (selected && result >= 0 && fault.mode == "crash-after") ::_exit(86);
    return result;
}

int main(int argc, char** argv) {
    executable = fs::canonical(argv[0]).string();
    if (argc == 8 && std::string(argv[1]) == "--journal-child") {
        const std::string action = argv[2];
        MaintenanceJournal journal;
        if (action == "complete") {
            Json::Value info;
            if (MaintenanceJournal::StartupGate(argv[3], &info) != NODECORE_ERROR_RECOVERY_REQUIRED ||
                journal.Resume(argv[3], info["operation_id"].asString()) != NODECORE_OK) return 91;
        }
        Arm(argv[4], static_cast<unsigned>(std::stoul(argv[5])), false, argv[6], std::stoi(argv[7]));
        const int result = action == "gate" ? MaintenanceJournal::StartupGate(argv[3])
            : action == "prepare" ? journal.Prepare(argv[3]) : journal.CompleteUnchanged();
        return result == NODECORE_OK ? 0 : result == NODECORE_ERROR_RECOVERY_REQUIRED ? 9
            : result == NODECORE_ERROR_MAINTENANCE_IO ? 12 : 90;
    }
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
