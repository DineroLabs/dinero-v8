// Task 6.3 R4: standalone Orchard wallet recovery drives every reachable
// public entry point through a fake IRuntimeRecoverySource; the test
// constructs no node object and DECLARES only wallet + delivery libraries.
// (Until the queued orchard_header owner decision, the link closure still
// reaches dinero_core through account_replay->orchard_header — measured,
// documented, not relied upon.) Exit-nonzero checks only — no assert().

#include "wallet/runtime_recovery_source.h"
#include "wallet/runtime_wallet_recovery.h"
#include "wallet/utxo_index.h"
#include "wallet/wallet_manager.h"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

using dinero::RuntimeAccountReplay;
using dinero::RuntimeOutboxCursor;
using dinero::RuntimeOutboxPage;
using dinero::RuntimeWalletCoverageProjection;
using dinero::Status;
using dinero::StatusOr;

} // namespace

namespace dinero {
// Declared a friend by RuntimeWalletCoverageProjection for exactly this test.
struct RuntimeCoverageProjectionTestAccess {
    static std::shared_ptr<const RuntimeWalletCoverageProjection> Make() {
        return std::shared_ptr<const RuntimeWalletCoverageProjection>(
            new RuntimeWalletCoverageProjection());
    }
};
} // namespace dinero

namespace {

// Fake R1 source: each operation's behavior is programmable; every call is
// counted so ordering assertions are real.
class FakeRecoverySource final : public dinero::wallet::IRuntimeRecoverySource {
public:
    StatusOr<std::shared_ptr<const RuntimeOutboxPage>> GetDeliveryPage(
        const RuntimeOutboxCursor&, size_t) const override {
        ++page_calls;
        if (!page_ok) return Status::Internal;
        return std::make_shared<const RuntimeOutboxPage>(page);
    }
    StatusOr<std::shared_ptr<const RuntimeAccountReplay>> GetAccountReplay() const override {
        ++replay_calls;
        if (!replay_ok) return Status::Internal;
        return std::shared_ptr<const RuntimeAccountReplay>(replay);
    }
    StatusOr<std::shared_ptr<const RuntimeWalletCoverageProjection>> GetWalletCoverage(
        dinero::WalletManager&, uint64_t, dinero::UTXOIndex&) const override {
        ++coverage_calls;
        if (!coverage_ok) return Status::Internal;
        return coverage;
    }
    Status ReconcileWalletCoverage(dinero::WalletManager&, dinero::UTXOIndex&,
                                   const RuntimeWalletCoverageProjection&) const override {
        ++reconcile_calls;
        return reconcile_ok ? Status::Ok : Status::Internal;
    }

    bool page_ok = true;
    bool replay_ok = true;
    bool coverage_ok = true;
    bool reconcile_ok = true;
    RuntimeOutboxPage page{};
    std::shared_ptr<const RuntimeAccountReplay> replay;
    std::shared_ptr<const RuntimeWalletCoverageProjection> coverage =
        dinero::RuntimeCoverageProjectionTestAccess::Make();
    mutable int page_calls = 0;
    mutable int replay_calls = 0;
    mutable int coverage_calls = 0;
    mutable int reconcile_calls = 0;
};

std::string ThrownWhat(const std::function<void()>& f) {
    try { f(); } catch (const std::exception& e) { return e.what(); }
    return "";
}

struct Fixture {
    fs::path dir;
    std::unique_ptr<dinero::WalletManager> wallet;
    std::unique_ptr<dinero::UTXOIndex> index;
    uint64_t session = 0;
    explicit Fixture(const char* tag) {
        dir = fs::temp_directory_path() /
              (std::string("recovery_no_node_") + tag + "_" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir);
        wallet = std::make_unique<dinero::WalletManager>(dir);
        wallet->create(tag);
        wallet->open(tag);
        index = std::make_unique<dinero::UTXOIndex>((dir / "index.sqlite").string());
        index->Initialize();
        wallet->setUTXOIndex(index.get());
        session = wallet->AcquireDatabaseLease()->Session();
    }
    ~Fixture() {
        wallet.reset();
        index.reset();
        fs::remove_all(dir);
    }
};

void TestHeldLeaseRefused() {
    Fixture f("lease");
    FakeRecoverySource src;
    const auto held = f.wallet->AcquireDatabaseLease();  // caller keeps a lease
    const auto what = ThrownWhat([&] {
        (void)dinero::RuntimeWalletRecovery::ResumeCatalogWalletStores(
            src, *f.wallet, *f.index, f.session);
    });
    check(what == "Wallet recovery requires released caller lease",
          "held caller lease refuses with the exact message (got: " + what + ")");
    check(src.coverage_calls == 0 && src.replay_calls == 0,
          "no source call happens before the lease check");
}

void TestCoverageSourceUnavailable() {
    Fixture f("coverage");
    FakeRecoverySource src;
    src.coverage_ok = false;  // fresh wallet: coverage is required
    const auto what = ThrownWhat([&] {
        (void)dinero::RuntimeWalletRecovery::ResumeCatalogWalletStores(
            src, *f.wallet, *f.index, f.session);
    });
    check(what == "Wallet recovery coverage source unavailable",
          "unavailable coverage source refuses with the exact message (got: " + what + ")");
    check(src.coverage_calls == 1, "coverage was asked exactly once");
    check(src.replay_calls == 0, "the account replay is not consulted after a coverage refusal");
}

void TestCoverageReconcileRefused() {
    Fixture f("reconcile");
    FakeRecoverySource src;
    src.reconcile_ok = false;
    const auto what = ThrownWhat([&] {
        (void)dinero::RuntimeWalletRecovery::ResumeCatalogWalletStores(
            src, *f.wallet, *f.index, f.session);
    });
    check(what == "Wallet recovery coverage reconciliation refused",
          "refused reconcile surfaces the exact message (got: " + what + ")");
    check(src.coverage_calls == 1 && src.reconcile_calls == 1,
          "coverage then reconcile, in order");
}

void TestAccountSourceUnavailable() {
    Fixture f("account");
    FakeRecoverySource src;
    src.replay_ok = false;
    const auto what = ThrownWhat([&] {
        (void)dinero::RuntimeWalletRecovery::ResumeWalletStores(
            src, *f.wallet, *f.index, f.session, /*account=*/0);
    });
    check(what == "Wallet recovery checked account source unavailable",
          "unavailable account source refuses with the exact message (got: " + what + ")");
    check(src.replay_calls == 1, "the account replay was asked exactly once");
}

void TestEnrolledResumeLegs() {
    // Review I3: ResumeEnrolledWalletStores was never exercised. Both of its
    // precondition legs are reachable without node fixtures.
    {
        Fixture f("enrolled_lease");
        FakeRecoverySource src;
        const auto held = f.wallet->AcquireDatabaseLease();
        const auto what = ThrownWhat([&] {
            (void)dinero::RuntimeWalletRecovery::ResumeEnrolledWalletStores(
                src, *f.wallet, *f.index, f.session);
        });
        check(what == "Wallet recovery requires released caller lease",
              "enrolled resume refuses a held lease (got: " + what + ")");
    }
    {
        Fixture f("enrolled_replay");
        FakeRecoverySource src;
        src.replay_ok = false;
        const auto what = ThrownWhat([&] {
            (void)dinero::RuntimeWalletRecovery::ResumeEnrolledWalletStores(
                src, *f.wallet, *f.index, f.session);
        });
        check(what == "Wallet recovery checked account source unavailable",
              "enrolled resume surfaces the exact replay message (got: " + what + ")");
        check(src.replay_calls == 1 && src.page_calls == 0,
              "replay is consulted once; no page before a replay refusal");
    }
    // Still fixture-gated (documented in the stage-6 report): an enrolled
    // store resuming through fake pages, the "Wallet recovery checked source
    // unavailable" mapping (a baseline refusal precedes any page read on a
    // fresh wallet), and ResumePreparedOrchardCatalogStores (needs a real
    // RuntimeAccountReplay, buildable only from node capture internals).
}

void TestTransparentResumeRefusesWithoutBaseline() {
    Fixture f("fresh");
    FakeRecoverySource src;
    src.page_ok = false;  // would throw IF consulted; the baseline check
                          // must refuse BEFORE any source read
    const auto what = ThrownWhat([&] {
        (void)dinero::RuntimeWalletRecovery::ResumeTransparentStores(
            src, *f.wallet, *f.index, f.session);
    });
    check(what == "Wallet recovery baseline reconciliation required",
          "a fresh wallet (no baseline) refuses transparent resume rather than "
          "adopting a baseline (got: " + what + ")");
    check(src.page_calls == 0, "no delivery page is fetched before the baseline check");
    // Enrolled-store resume through real pages stays covered by the ON
    // Orchard suites (orchard_index_delivery_checks.h drives
    // RuntimeWalletRecovery::Resume with fake page sources over real
    // enrolled stores); this target deliberately links no node code.
}

} // namespace

int main() {
    TestHeldLeaseRefused();
    TestCoverageSourceUnavailable();
    TestCoverageReconcileRefused();
    TestAccountSourceUnavailable();
    TestEnrolledResumeLegs();
    TestTransparentResumeRefusesWithoutBaseline();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("WalletRecoveryWithoutNode: all checks passed");
    return 0;
}
