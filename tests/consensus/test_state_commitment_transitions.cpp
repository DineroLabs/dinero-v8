// Connect / disconnect / reorg state transitions for state_commitment_v1.
//
// These exercise the state MODEL directly -- commitment tree, nullifier set,
// anchor history -- rather than driving a daemon, so every assertion is
// deterministic and a failure points at the transition rather than at timing.
// Daemon-level behaviour is covered by the integration suites
// (test_shielded_reorg_invertibility.sh, ...disconnect_restart_equivalence.sh,
// ...multinode_deep_reorg.sh); this file covers what those cannot isolate.
//
// Activation stays disabled throughout, and one test pins that.
#include <gtest/gtest.h>

#include <cstdio>
#include <unistd.h>
#include <sqlite3.h>
#include <filesystem>
#include <string>
#include <vector>

#include "consensus/shielded/anchor_history.h"
#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/nullifier_set.h"
#include "consensus/shielded/shielded_root.h"
#include "consensus/shielded/shielded_validation.h"
#include "consensus/state_commitment.h"

namespace sh = dinero::consensus::shielded;
using dinero::uint256;

namespace {

using Hash = sh::Hash;

Hash MakeHash(uint8_t seed, uint8_t salt = 0) {
    Hash h{};
    for (size_t i = 0; i < h.size(); ++i) {
        h[i] = static_cast<uint8_t>(seed * 31u + i * 7u + salt);
    }
    return h;
}

/// One block's shielded effects: notes created, nullifiers spent.
struct BlockEffects {
    uint32_t height{0};
    std::vector<Hash> notes;
    std::vector<Hash> nullifiers;
};

BlockEffects Block(uint32_t height, uint8_t seed, size_t notes = 2, size_t nfs = 1) {
    BlockEffects b;
    b.height = height;
    for (size_t i = 0; i < notes; ++i) b.notes.push_back(MakeHash(seed, static_cast<uint8_t>(i)));
    for (size_t i = 0; i < nfs; ++i) {
        b.nullifiers.push_back(MakeHash(static_cast<uint8_t>(seed + 100), static_cast<uint8_t>(i)));
    }
    return b;
}

/// Tree + nullifiers + anchors, with the connect/disconnect primitives the
/// chainstate uses. Owns a temp sqlite file for the nullifier set.
class ShieldedState {
public:
    explicit ShieldedState(const std::string& tag) {
        dir_ = std::filesystem::temp_directory_path() /
               ("dinero_sc_tx_" + tag + "_" + std::to_string(::getpid()) +
                "_" + std::to_string(counter_++));
        std::filesystem::create_directories(dir_);
        const auto db = (dir_ / "nullifiers.sqlite").string();
        open_ok_ = (nullifiers_.Open(db) == sh::NullifierSet::OpenResult::Ok);
    }
    ~ShieldedState() {
        nullifiers_.Close();
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    bool ok() const { return open_ok_; }

    /// Connect: append notes, insert nullifiers, record the post-block anchor.
    /// Returns false if any nullifier was already present (a double spend, or
    /// a double APPLY of the same block).
    bool Connect(const BlockEffects& b) {
        for (const auto& n : b.notes) tree_.Append(n);
        bool all_new = true;
        for (const auto& nf : b.nullifiers) {
            if (!nullifiers_.Insert(nf, b.height)) all_new = false;
        }
        anchors_.RecordRoot(b.height, tree_.Root());
        heights_.push_back({b.height, tree_.Size() - b.notes.size()});
        return all_new;
    }

    /// Disconnect back to the state before the block at `height`.
    void DisconnectTo(uint32_t height, uint64_t tree_size_before) {
        tree_.Truncate(tree_size_before);
        nullifiers_.RollbackAbove(height - 1);
        anchors_.RollbackAbove(height - 1);
    }

    uint256 Root() {
        const auto r = sh::ComputeShieldedRoot(tree_, nullifiers_, anchors_);
        return r.value_or(uint256());
    }

    uint64_t TreeSize() const { return tree_.Size(); }
    uint64_t NullifierCount() const { return nullifiers_.Size(); }
    size_t AnchorCount() const { return anchors_.Size(); }

private:
    static int counter_;
    std::filesystem::path dir_;
    bool open_ok_{false};
    sh::CommitmentTree tree_;
    sh::NullifierSet nullifiers_;
    sh::AnchorHistory anchors_;
    std::vector<std::pair<uint32_t, uint64_t>> heights_;
};
int ShieldedState::counter_ = 0;

}  // namespace

// --- item 1: connect applies exactly once ----------------------------------

TEST(Transitions, ConnectingTheSameBlockTwiceIsDetectable) {
    ShieldedState s("once");
    ASSERT_TRUE(s.ok());
    const auto b = Block(1, 5);

    EXPECT_TRUE(s.Connect(b)) << "first connect must accept every nullifier";
    const uint256 after_one = s.Root();

    // A second application must NOT silently produce the same root. If the
    // state model were accidentally idempotent, a connect path that applied a
    // block twice would be invisible here and would diverge in production.
    EXPECT_FALSE(s.Connect(b)) << "re-inserting a spent nullifier must be refused";
    EXPECT_NE(s.Root(), after_one)
        << "double-applying a block must change the root, so the error is detectable";
}

// --- items 2 & 3: disconnect restores, reconnect reproduces ----------------

TEST(Transitions, DisconnectRestoresRootTreeNullifiersAndAnchorsExactly) {
    ShieldedState s("disc");
    ASSERT_TRUE(s.ok());
    ASSERT_TRUE(s.Connect(Block(1, 1)));

    const uint256 before = s.Root();
    const uint64_t tree_before = s.TreeSize();
    const uint64_t nf_before = s.NullifierCount();
    const size_t anchors_before = s.AnchorCount();

    ASSERT_TRUE(s.Connect(Block(2, 2)));
    ASSERT_NE(s.Root(), before) << "the block must actually change state";

    s.DisconnectTo(2, tree_before);

    EXPECT_EQ(s.TreeSize(), tree_before);
    EXPECT_EQ(s.NullifierCount(), nf_before);
    EXPECT_EQ(s.AnchorCount(), anchors_before);
    EXPECT_EQ(s.Root(), before) << "disconnect must restore the exact prior root";
}

TEST(Transitions, ReconnectReproducesByteIdenticalState) {
    ShieldedState s("recon");
    ASSERT_TRUE(s.ok());
    ASSERT_TRUE(s.Connect(Block(1, 1)));
    const uint64_t base_size = s.TreeSize();

    ASSERT_TRUE(s.Connect(Block(2, 2)));
    const uint256 connected = s.Root();

    s.DisconnectTo(2, base_size);
    ASSERT_TRUE(s.Connect(Block(2, 2)));

    EXPECT_EQ(s.Root(), connected) << "connect -> disconnect -> connect must be identical";
}

// --- item 4: competing branches stay independent ---------------------------

TEST(Transitions, CompetingBranchesMaintainIndependentState) {
    ShieldedState a("brA");
    ShieldedState b("brB");
    ASSERT_TRUE(a.ok() && b.ok());

    const auto ancestor = Block(1, 1);
    ASSERT_TRUE(a.Connect(ancestor));
    ASSERT_TRUE(b.Connect(ancestor));
    ASSERT_EQ(a.Root(), b.Root()) << "shared history must agree";

    ASSERT_TRUE(a.Connect(Block(2, 10)));
    ASSERT_TRUE(b.Connect(Block(2, 20)));

    EXPECT_NE(a.Root(), b.Root()) << "divergent branches must not share a root";

    // And extending one must not perturb the other.
    const uint256 b_root = b.Root();
    ASSERT_TRUE(a.Connect(Block(3, 11)));
    EXPECT_EQ(b.Root(), b_root) << "branch B must be untouched by work on branch A";
}

// --- item 5: a winning reorg equals direct validation ----------------------

TEST(Transitions, ReorgResultEqualsDirectValidationOfTheWinningBranch) {
    // Path 1: connect losing branch, disconnect it, connect winner.
    ShieldedState viaReorg("reorg");
    ASSERT_TRUE(viaReorg.ok());
    ASSERT_TRUE(viaReorg.Connect(Block(1, 1)));
    const uint64_t fork_size = viaReorg.TreeSize();
    ASSERT_TRUE(viaReorg.Connect(Block(2, 10)));   // loser
    viaReorg.DisconnectTo(2, fork_size);
    ASSERT_TRUE(viaReorg.Connect(Block(2, 20)));   // winner

    // Path 2: the winning branch validated directly, never seeing the loser.
    ShieldedState direct("direct");
    ASSERT_TRUE(direct.ok());
    ASSERT_TRUE(direct.Connect(Block(1, 1)));
    ASSERT_TRUE(direct.Connect(Block(2, 20)));

    EXPECT_EQ(viaReorg.Root(), direct.Root())
        << "a node that reorged must reach the same state as one that never forked";
    EXPECT_EQ(viaReorg.TreeSize(), direct.TreeSize());
    EXPECT_EQ(viaReorg.NullifierCount(), direct.NullifierCount());
}

TEST(Transitions, DeepReorgEqualsDirectValidation) {
    constexpr uint32_t kDepth = 25;
    ShieldedState viaReorg("deep_r");
    ShieldedState direct("deep_d");
    ASSERT_TRUE(viaReorg.ok() && direct.ok());

    ASSERT_TRUE(viaReorg.Connect(Block(1, 1)));
    ASSERT_TRUE(direct.Connect(Block(1, 1)));
    const uint64_t fork_size = viaReorg.TreeSize();

    for (uint32_t h = 2; h < 2 + kDepth; ++h) {
        ASSERT_TRUE(viaReorg.Connect(Block(h, static_cast<uint8_t>(h + 50))));
    }
    viaReorg.DisconnectTo(2, fork_size);

    for (uint32_t h = 2; h < 2 + kDepth; ++h) {
        ASSERT_TRUE(viaReorg.Connect(Block(h, static_cast<uint8_t>(h + 150))));
        ASSERT_TRUE(direct.Connect(Block(h, static_cast<uint8_t>(h + 150))));
    }

    EXPECT_EQ(viaReorg.Root(), direct.Root())
        << kDepth << "-block reorg must converge on the direct-validation state";
}

// --- item 8: activation stays disabled -------------------------------------

TEST(Transitions, ActivationRemainsDisabledThroughout) {
    for (uint64_t h : {uint64_t{0}, uint64_t{1}, uint64_t{61000}, uint64_t{99677},
                       uint64_t{1000000}}) {
        EXPECT_FALSE(dinero::consensus::RequiresStateCommitment(h));
    }
}

// --- prevention, not merely detection --------------------------------------

TEST(Transitions, DuplicateApplyIsRefusedBEFOREAnyStateMutation) {
    // Detectability is not safety. ApplyShieldedBundle used to append outputs
    // to the tree FIRST and only discover the already-spent nullifier on
    // insert, leaving the caller's rollback as the only thing standing between
    // a double-applied block and a corrupted tree. This asserts the safe
    // ordering: on refusal, nothing was mutated.
    ShieldedState s("preapply");
    ASSERT_TRUE(s.ok());

    sh::CommitmentTree tree;
    sh::NullifierSet nulls;
    const auto dir = std::filesystem::temp_directory_path() /
                     ("dinero_sc_preapply_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    ASSERT_EQ(nulls.Open((dir / "n.sqlite").string()), sh::NullifierSet::OpenResult::Ok);

    sh::ShieldedBundle bundle;
    sh::ShieldedOutput out{};
    out.commitment = MakeHash(9, 1);
    bundle.outputs.push_back(out);
    sh::ShieldedSpend spend{};
    spend.nullifier = MakeHash(9, 2);
    bundle.spends.push_back(spend);

    ASSERT_TRUE(sh::ApplyShieldedBundle(bundle, &tree, &nulls, 1));
    const uint64_t size_after_first = tree.Size();
    const auto root_after_first = tree.Root();
    ASSERT_EQ(size_after_first, 1u);

    // Second application: must be refused, and must not have appended.
    EXPECT_FALSE(sh::ApplyShieldedBundle(bundle, &tree, &nulls, 1));
    EXPECT_EQ(tree.Size(), size_after_first)
        << "a refused bundle must not have appended its outputs";
    EXPECT_EQ(tree.Root(), root_after_first)
        << "a refused bundle must leave the commitment tree byte-identical";

    nulls.Close();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(Transitions, IntraBundleDuplicateNullifierIsRejectedWithoutMutation) {
    // Contains() is a PERSISTENCE check. It cannot see a nullifier repeated
    // inside the incoming bundle: on an empty set both preflight lookups
    // return false, outputs get appended, the first Insert succeeds and the
    // second fails -- the same partial-mutation defect the preflight was added
    // to remove, one layer in.
    sh::CommitmentTree tree;
    sh::NullifierSet nulls;
    const auto dir = std::filesystem::temp_directory_path() /
                     ("dinero_sc_intra_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    ASSERT_EQ(nulls.Open((dir / "n.sqlite").string()), sh::NullifierSet::OpenResult::Ok);

    const uint64_t size_before = tree.Size();
    const auto root_before = tree.Root();
    const uint64_t nf_before = nulls.Size();

    sh::ShieldedBundle bundle;
    sh::ShieldedOutput out{};
    out.commitment = MakeHash(21, 1);
    bundle.outputs.push_back(out);
    sh::ShieldedSpend a{};
    a.nullifier = MakeHash(21, 2);
    sh::ShieldedSpend b{};
    b.nullifier = a.nullifier;          // the same nullifier twice
    bundle.spends.push_back(a);
    bundle.spends.push_back(b);

    EXPECT_FALSE(sh::ApplyShieldedBundle(bundle, &tree, &nulls, 1))
        << "a bundle spending the same nullifier twice must be refused";
    EXPECT_EQ(tree.Size(), size_before)
        << "refusal must not have appended outputs";
    EXPECT_EQ(tree.Root(), root_before)
        << "refusal must leave the tree byte-identical";
    EXPECT_EQ(nulls.Size(), nf_before)
        << "refusal must not have persisted a partial nullifier insert";

    nulls.Close();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(Transitions, IntraBundleDuplicateLeavesNothingPersistedAcrossRestart) {
    // The in-process assertions above could pass while a partial write sat in
    // sqlite. Reopen the same database and confirm the refusal persisted
    // nothing -- this is the check that would catch a rejected bundle whose
    // first nullifier survived a crash.
    const auto dir = std::filesystem::temp_directory_path() /
                     ("dinero_sc_restart_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto db = (dir / "n.sqlite").string();

    sh::ShieldedSpend a{};
    a.nullifier = MakeHash(31, 2);
    sh::ShieldedBundle bundle;
    sh::ShieldedOutput out{};
    out.commitment = MakeHash(31, 1);
    bundle.outputs.push_back(out);
    bundle.spends.push_back(a);
    bundle.spends.push_back(a);  // duplicate within the bundle

    {
        sh::CommitmentTree tree;
        sh::NullifierSet nulls;
        ASSERT_EQ(nulls.Open(db), sh::NullifierSet::OpenResult::Ok);
        EXPECT_FALSE(sh::ApplyShieldedBundle(bundle, &tree, &nulls, 7));
        nulls.Close();
    }
    {
        sh::NullifierSet reopened;
        ASSERT_EQ(reopened.Open(db), sh::NullifierSet::OpenResult::Ok);
        EXPECT_EQ(reopened.Size(), 0u)
            << "a refused bundle must persist no nullifier rows";
        EXPECT_FALSE(reopened.Contains(a.nullifier))
            << "the refused nullifier must not be spendable-blocked after restart";
        reopened.Close();
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(Transitions, DuplicateOutputCommitmentIsAllowedNoConsensusRuleForbidsIt) {
    // Pinning the ABSENCE of a rule, deliberately. ValidateShieldedBundle has
    // no duplicate-output check and ShieldedValidationError has no code for
    // one, so ApplyShieldedBundle must not invent it: refusing here would
    // reject a bundle that other nodes accept. Appending the same leaf twice
    // is well defined. If the protocol ever forbids this, the rule belongs in
    // ValidateShieldedBundle with its own error code, and this test should
    // then be inverted deliberately rather than quietly.
    sh::CommitmentTree tree;
    const uint64_t before = tree.Size();
    sh::ShieldedBundle bundle;
    sh::ShieldedOutput o{};
    o.commitment = MakeHash(41, 1);
    bundle.outputs.push_back(o);
    bundle.outputs.push_back(o);
    EXPECT_TRUE(sh::ApplyShieldedBundle(bundle, &tree, nullptr, 1));
    EXPECT_EQ(tree.Size(), before + 2) << "both leaves must be appended";
}

// --- fault injection: first / intermediate / final insert failure ----------
//
// A duplicate inside the batch violates the nullifier PRIMARY KEY, so sqlite
// fails that exact step. Placing the duplicate at position 1, 2 or 3 injects
// a failure at the first, intermediate and final insert with no test hook in
// production code.

namespace {

struct BatchFaultFixture {
    std::filesystem::path dir;
    std::string db;
    explicit BatchFaultFixture(const char* tag) {
        dir = std::filesystem::temp_directory_path() /
              (std::string("dinero_batch_") + tag + "_" + std::to_string(::getpid()));
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        db = (dir / "n.sqlite").string();
    }
    ~BatchFaultFixture() { std::error_code ec; std::filesystem::remove_all(dir, ec); }
};

// Batch of three where entry `dup_index` repeats entry 0, so THAT insert fails.
std::vector<std::pair<sh::Hash, uint32_t>> BatchFailingAt(size_t dup_index) {
    std::vector<std::pair<sh::Hash, uint32_t>> b;
    for (size_t i = 0; i < 3; ++i) b.emplace_back(MakeHash(60, static_cast<uint8_t>(i)), 5);
    b[dup_index].first = b[0].first;
    return b;
}

}  // namespace

TEST(BatchFault, FirstInsertFailureLeavesNothingWritten) {
    BatchFaultFixture f("first");
    sh::NullifierSet n;
    ASSERT_EQ(n.Open(f.db), sh::NullifierSet::OpenResult::Ok);
    // Pre-seed the value so the FIRST insert of the batch collides.
    ASSERT_TRUE(n.Insert(MakeHash(60, 0), 4));
    const uint64_t before = n.Size();

    std::vector<std::pair<sh::Hash, uint32_t>> batch;
    for (size_t i = 0; i < 3; ++i) batch.emplace_back(MakeHash(60, static_cast<uint8_t>(i)), 5);

    EXPECT_FALSE(n.InsertBatch(batch));
    EXPECT_EQ(n.Size(), before) << "a failed batch must not leave later rows behind";
    n.Close();
}

TEST(BatchFault, IntermediateInsertFailureRollsBackTheWholeBatch) {
    BatchFaultFixture f("mid");
    sh::NullifierSet n;
    ASSERT_EQ(n.Open(f.db), sh::NullifierSet::OpenResult::Ok);
    const uint64_t before = n.Size();
    ASSERT_EQ(before, 0u);

    // Fails on entry 1: entry 0 has ALREADY been inserted inside the
    // transaction, which is precisely the partial-write case.
    EXPECT_FALSE(n.InsertBatch(BatchFailingAt(1)));
    EXPECT_EQ(n.Size(), before)
        << "the already-inserted first row must be rolled back, not retained";
    n.Close();
}

TEST(BatchFault, FinalInsertFailureRollsBackTheWholeBatch) {
    BatchFaultFixture f("last");
    sh::NullifierSet n;
    ASSERT_EQ(n.Open(f.db), sh::NullifierSet::OpenResult::Ok);
    const uint64_t before = n.Size();

    // Fails on the LAST entry, after two successful inserts.
    EXPECT_FALSE(n.InsertBatch(BatchFailingAt(2)));
    EXPECT_EQ(n.Size(), before) << "two committed-in-transaction rows must both vanish";
    n.Close();
}

TEST(BatchFault, RollbackSurvivesRestartReconstruction) {
    // The in-process Size() could read a cache. Reopen and confirm the
    // rollback is what is actually durable.
    BatchFaultFixture f("restart");
    {
        sh::NullifierSet n;
        ASSERT_EQ(n.Open(f.db), sh::NullifierSet::OpenResult::Ok);
        EXPECT_FALSE(n.InsertBatch(BatchFailingAt(1)));
        n.Close();
    }
    {
        sh::NullifierSet re;
        ASSERT_EQ(re.Open(f.db), sh::NullifierSet::OpenResult::Ok);
        EXPECT_EQ(re.Size(), 0u) << "no row from the failed batch may survive restart";
        EXPECT_FALSE(re.Contains(MakeHash(60, 0)));
        EXPECT_FALSE(re.Contains(MakeHash(60, 2)));
        re.Close();
    }
}

TEST(BatchFault, SuccessfulBatchCommitsEveryRow) {
    // The negative tests would also pass if InsertBatch never wrote anything.
    BatchFaultFixture f("ok");
    sh::NullifierSet n;
    ASSERT_EQ(n.Open(f.db), sh::NullifierSet::OpenResult::Ok);
    std::vector<std::pair<sh::Hash, uint32_t>> batch;
    for (size_t i = 0; i < 3; ++i) batch.emplace_back(MakeHash(70, static_cast<uint8_t>(i)), 9);
    EXPECT_TRUE(n.InsertBatch(batch));
    EXPECT_EQ(n.Size(), 3u);
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_TRUE(n.Contains(MakeHash(70, static_cast<uint8_t>(i)))) << "entry " << i;
    }
    n.Close();
}

// --- provenance: legacy migration vs post-crash residue --------------------
//
// "ChainDB empty, sqlite populated" is produced by BOTH a genuine legacy
// database AND the crash window (first shielded block commits its nullifier
// batch, dies before the ChainDB write). Row counts cannot tell them apart.
// Promoting crash residue would make an unconnected block's nullifiers
// authoritative and its notes permanently unspendable.

TEST(Provenance, FreshDatabaseIsStampedAsACacheImmediately) {
    BatchFaultFixture f("prov_fresh");
    sh::NullifierSet n;
    ASSERT_EQ(n.Open(f.db), sh::NullifierSet::OpenResult::Ok);
    // Empty and unstamped -> stamped NOW, before anything can populate it.
    // It reports Cache rather than FreshCache because the stamp is already
    // durable; FreshCache is only the transient pre-stamp classification.
    // The load-bearing property is that it is not, and can never become,
    // a migration candidate.
    EXPECT_EQ(n.GetProvenance(), sh::NullifierSet::Provenance::Cache);
    EXPECT_NE(n.GetProvenance(), sh::NullifierSet::Provenance::LegacyCandidate);
    n.Close();
}

TEST(Provenance, CrashResidueIsRecognisedAsCacheNotLegacy) {
    // The exact hazard. Create a database this build owns, put rows in it as
    // the crash window would, reopen, and require it to still read as a cache.
    BatchFaultFixture f("prov_crash");
    {
        sh::NullifierSet n;
        ASSERT_EQ(n.Open(f.db), sh::NullifierSet::OpenResult::Ok);
        ASSERT_TRUE(n.Insert(MakeHash(80, 1), 5));
        ASSERT_TRUE(n.Insert(MakeHash(80, 2), 5));
        n.Close();  // stands in for the crash: rows committed, ChainDB never written
    }
    {
        sh::NullifierSet re;
        ASSERT_EQ(re.Open(f.db), sh::NullifierSet::OpenResult::Ok);
        EXPECT_EQ(re.Size(), 2u) << "rows are present, as after a crash";
        EXPECT_EQ(re.GetProvenance(), sh::NullifierSet::Provenance::Cache)
            << "populated + ChainDB-empty must NOT read as legacy; these rows "
               "must never be promoted to authoritative";
        EXPECT_NE(re.GetProvenance(), sh::NullifierSet::Provenance::LegacyCandidate);
        re.Close();
    }
}

TEST(Provenance, GenuineLegacyDatabaseIsStillMigratable) {
    // Do not fix the crash case by breaking real legacy wallets. A file with
    // rows and NO cache stamp is a pre-authority database and must remain
    // eligible for the one-shot migration.
    BatchFaultFixture f("prov_legacy");
    {
        sh::NullifierSet n;
        ASSERT_EQ(n.Open(f.db), sh::NullifierSet::OpenResult::Ok);
        ASSERT_TRUE(n.Insert(MakeHash(81, 1), 3));
        n.Close();
    }
    // Strip the stamp to reproduce a genuine pre-authority file.
    {
        sqlite3* raw = nullptr;
        ASSERT_EQ(sqlite3_open(f.db.c_str(), &raw), SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(raw, "PRAGMA user_version = 0", nullptr, nullptr, nullptr),
                  SQLITE_OK);
        sqlite3_close(raw);
    }
    {
        sh::NullifierSet legacy;
        ASSERT_EQ(legacy.Open(f.db), sh::NullifierSet::OpenResult::Ok);
        EXPECT_EQ(legacy.GetProvenance(), sh::NullifierSet::Provenance::LegacyCandidate)
            << "an unstamped populated file is a real legacy database";
        legacy.Close();
    }
}

TEST(Provenance, MigrationStampIsPermanentSoItCannotRunTwice) {
    BatchFaultFixture f("prov_once");
    {
        sh::NullifierSet n;
        ASSERT_EQ(n.Open(f.db), sh::NullifierSet::OpenResult::Ok);
        ASSERT_TRUE(n.Insert(MakeHash(82, 1), 3));
        n.Close();
    }
    {
        sqlite3* raw = nullptr;
        ASSERT_EQ(sqlite3_open(f.db.c_str(), &raw), SQLITE_OK);
        sqlite3_exec(raw, "PRAGMA user_version = 0", nullptr, nullptr, nullptr);
        sqlite3_close(raw);
    }
    {
        sh::NullifierSet legacy;
        ASSERT_EQ(legacy.Open(f.db), sh::NullifierSet::OpenResult::Ok);
        ASSERT_EQ(legacy.GetProvenance(), sh::NullifierSet::Provenance::LegacyCandidate);
        ASSERT_TRUE(legacy.MarkAsCache());  // what a completed migration does
        EXPECT_EQ(legacy.GetProvenance(), sh::NullifierSet::Provenance::Cache);
        legacy.Close();
    }
    {
        sh::NullifierSet again;
        ASSERT_EQ(again.Open(f.db), sh::NullifierSet::OpenResult::Ok);
        EXPECT_EQ(again.GetProvenance(), sh::NullifierSet::Provenance::Cache)
            << "a migrated file must never be eligible for migration again";
        again.Close();
    }
}
