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
#include <filesystem>
#include <string>
#include <vector>

#include "consensus/shielded/anchor_history.h"
#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/nullifier_set.h"
#include "consensus/shielded/shielded_root.h"
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
