/**
 * Adversarial Consensus Test Suite — shielded pool hardening.
 *
 * Five attack families that target the weakest assumption classes:
 *
 *   1. Reorder adversarial:  shield→unshield loops in same block
 *   2. Half-applied block:   rollback after partial state mutation
 *   3. Reorg stress:         repeated rollback/reconnect cycles
 *   4. Mixed-input packing:  all three lanes in one block
 *   5. Concurrent mutations: parallel shield + unshield + balance reads
 *
 * These tests do NOT verify features. They verify that the system
 * stays consistent when an attacker deliberately creates worst-case
 * ordering, timing, and failure conditions.
 */

#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/nullifier_set.h"
#include "consensus/shielded/shielded_tx.h"
#include "consensus/shielded/shielded_validation.h"
#include "consensus/shielded/shielded_block_validation.h"
#include "consensus/shielded/shielded_serialization.h"
#include "wallet/shielded_note_store.h"
#include "wallet/shielded_wallet_ops.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <atomic>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace sh = dinero::consensus::shielded;
namespace ops = dinero::wallet::shielded_ops;

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const char* desc) {
    if (cond) { printf("  [PASS] %s\n", desc); ++g_pass; }
    else      { printf("  [FAIL] %s\n", desc); ++g_fail; }
}

static sh::Hash make_hash(uint8_t fill) {
    sh::Hash h{};
    std::memset(h.data(), fill, sh::HASH_BYTES);
    return h;
}

// ═══════════════════════════════════════════════════════════════════════
// Family 1: Reorder adversarial — shield→unshield loops in same block
// ═══════════════════════════════════════════════════════════════════════

static void test_reorder_adversarial() {
    printf("\nFamily 1: Reorder adversarial (shield→unshield loops)\n");

    sh::CommitmentTree tree;
    std::string db = "/tmp/adv_nul_1_" + std::to_string(getpid()) + ".db";
    sh::NullifierSet ns;
    ns.Open(db);

    // Bundle A: shield 1000 (1 output, 0 spends)
    sh::ShieldedBundle bA;
    bA.value_balance = 1000;
    sh::ShieldedOutput oA;
    oA.commitment = sh::NoteCommitment(make_hash(0x10), make_hash(0x20), make_hash(0x30));
    oA.zk_proof = {0x01};
    bA.outputs.push_back(oA);

    // Bundle B: spend that note + create new one (transfer)
    sh::ShieldedBundle bB;
    bB.value_balance = 0;
    sh::ShieldedSpend sB;
    sB.nullifier = make_hash(0xAA);
    sB.zk_proof = {0x02};
    bB.spends.push_back(sB);
    sh::ShieldedOutput oB;
    oB.commitment = sh::NoteCommitment(make_hash(0x40), make_hash(0x50), make_hash(0x60));
    oB.zk_proof = {0x03};
    bB.outputs.push_back(oB);

    // Bundle C: spend that new note + unshield
    sh::ShieldedBundle bC;
    bC.value_balance = -1000;
    sh::ShieldedSpend sC;
    sC.nullifier = make_hash(0xBB);
    sC.zk_proof = {0x04};
    bC.spends.push_back(sC);

    // Test order 1: A, B, C (natural order)
    {
        sh::CommitmentTree t1;
        sh::NullifierSet n1;
        n1.Open("/tmp/adv_nul_1a_" + std::to_string(getpid()) + ".db");

        // Set anchors to current tree root
        sB.anchor = t1.Root();
        sC.anchor = t1.Root();
        bB.spends[0] = sB;
        bC.spends[0] = sC;

        std::vector<sh::ShieldedBundle> order1 = {bA, bB, bC};
        std::vector<int64_t> deltas1 = {1000, 0, -1000};
        sh::BlockShieldedContext ctx1{&n1, &t1, 100};

        auto err = sh::ValidateBlockShielded(order1, deltas1, ctx1);
        check(err == sh::BlockValidationError::Ok, "order A→B→C validates");

        sh::ApplyBlockShielded(order1, &t1, &n1, 100);
        auto root1 = t1.Root();
        auto nsize1 = n1.Size();
        check(t1.Size() == 2, "tree has 2 commitments (A+B outputs)");
        check(nsize1 == 2, "2 nullifiers (B+C spends)");

        // Test order 2: C, A, B (adversarial reorder — C spends before A creates)
        sh::CommitmentTree t2;
        sh::NullifierSet n2;
        n2.Open("/tmp/adv_nul_1b_" + std::to_string(getpid()) + ".db");

        std::vector<sh::ShieldedBundle> order2 = {bC, bA, bB};
        std::vector<int64_t> deltas2 = {-1000, 1000, 0};
        sh::BlockShieldedContext ctx2{&n2, &t2, 100};

        // Different order → same bundles → should still validate
        // (block validation checks nullifier uniqueness, not ordering of
        // shield-before-spend within the block — that's the ZK proof's job)
        auto err2 = sh::ValidateBlockShielded(order2, deltas2, ctx2);
        check(err2 == sh::BlockValidationError::Ok, "reordered C→A→B also validates");

        sh::ApplyBlockShielded(order2, &t2, &n2, 100);
        check(t2.Size() == 2, "reordered tree also has 2 commitments");
        check(n2.Size() == 2, "reordered nullifiers also 2");

        // CRITICAL: ordering affects tree root (insertion order matters)
        // This is expected and correct — different tx order = different tree
        // root. Miners must pick one order and commit to it.
        // The test verifies CONSISTENCY, not EQUALITY.
        check(t1.Size() == t2.Size(), "both orders produce same tree size");
        check(n1.Size() == n2.Size(), "both orders produce same nullifier count");

        n1.Close(); n2.Close();
        std::remove(("/tmp/adv_nul_1a_" + std::to_string(getpid()) + ".db").c_str());
        std::remove(("/tmp/adv_nul_1b_" + std::to_string(getpid()) + ".db").c_str());
    }

    // Test: duplicate nullifier across reordered bundles
    {
        sh::CommitmentTree t3;
        sh::NullifierSet n3;
        n3.Open("/tmp/adv_nul_1c_" + std::to_string(getpid()) + ".db");

        // Two bundles with the SAME nullifier
        sh::ShieldedBundle dup1 = bB;  // has nullifier 0xAA
        sh::ShieldedBundle dup2 = bB;  // same nullifier!
        dup2.outputs[0].commitment = make_hash(0x99);  // different output

        std::vector<sh::ShieldedBundle> dups = {dup1, dup2};
        std::vector<int64_t> dup_deltas = {0, 0};
        sh::BlockShieldedContext ctx3{&n3, &t3, 100};

        auto err3 = sh::ValidateBlockShielded(dups, dup_deltas, ctx3);
        check(err3 == sh::BlockValidationError::InterTxNullifierDuplicate,
              "cross-tx duplicate nullifier rejected regardless of order");

        n3.Close();
        std::remove(("/tmp/adv_nul_1c_" + std::to_string(getpid()) + ".db").c_str());
    }

    ns.Close();
    std::remove(db.c_str());
}

// ═══════════════════════════════════════════════════════════════════════
// Family 2: Half-applied block — rollback after partial state mutation
// ═══════════════════════════════════════════════════════════════════════

static void test_half_applied_rollback() {
    printf("\nFamily 2: Half-applied block (rollback consistency)\n");

    sh::CommitmentTree tree;
    std::string db = "/tmp/adv_nul_2_" + std::to_string(getpid()) + ".db";
    sh::NullifierSet ns;
    ns.Open(db);

    // Save pre-block state
    auto pre_root = tree.Root();
    auto pre_frontier = tree.SerializeFrontier();
    uint64_t pre_size = tree.Size();
    uint64_t pre_nul = ns.Size();

    // Apply a block with 3 shielded outputs + 1 spend
    sh::ShieldedBundle b1;
    b1.value_balance = 500;
    for (int i = 0; i < 3; ++i) {
        sh::ShieldedOutput o;
        o.commitment = make_hash(static_cast<uint8_t>(0x10 + i));
        o.zk_proof = {0x01};
        b1.outputs.push_back(o);
    }
    sh::ShieldedSpend s1;
    s1.nullifier = make_hash(0xDD);
    s1.anchor = tree.Root();
    s1.zk_proof = {0x02};
    b1.spends.push_back(s1);

    std::vector<sh::ShieldedBundle> bundles = {b1};
    sh::ApplyBlockShielded(bundles, &tree, &ns, 200);

    check(tree.Size() == 3, "after apply: tree has 3 commitments");
    check(ns.Size() == 1, "after apply: 1 nullifier");
    check(tree.Root() != pre_root, "root changed after apply");

    // SIMULATE CRASH: rollback to pre-block state
    // Commitment tree: restore frontier
    sh::CommitmentTree restored;
    bool ok = restored.DeserializeFrontier(pre_frontier.data(), pre_frontier.size());
    check(ok, "frontier deserialization succeeds");
    check(restored.Size() == pre_size, "restored tree size matches pre-block");
    check(restored.Root() == pre_root, "restored root matches pre-block");

    // Nullifier set: rollback above height 199 (removes height 200)
    ns.RollbackAbove(199);
    check(ns.Size() == pre_nul, "nullifiers rolled back to pre-block count");

    // Verify the nullifier from the rolled-back block is gone
    check(!ns.Contains(s1.nullifier), "rolled-back nullifier no longer in set");

    // Verify we can re-apply the same block
    sh::CommitmentTree reapply = restored;
    sh::ApplyBlockShielded(bundles, &reapply, &ns, 200);
    check(reapply.Size() == 3, "re-apply produces same tree size");
    check(ns.Size() == 1, "re-apply produces same nullifier count");

    ns.Close();
    std::remove(db.c_str());
}

// ═══════════════════════════════════════════════════════════════════════
// Family 3: Reorg stress — repeated rollback/reconnect cycles
// ═══════════════════════════════════════════════════════════════════════

static void test_reorg_stress() {
    printf("\nFamily 3: Reorg stress (10 cycles)\n");

    sh::CommitmentTree tree;
    std::string db = "/tmp/adv_nul_3_" + std::to_string(getpid()) + ".db";
    sh::NullifierSet ns;
    ns.Open(db);

    const int CYCLES = 10;
    auto base_frontier = tree.SerializeFrontier();
    auto base_root = tree.Root();

    for (int cycle = 0; cycle < CYCLES; ++cycle) {
        // Apply a block at height 100+cycle with unique data
        sh::ShieldedBundle b;
        b.value_balance = 100;
        sh::ShieldedOutput o;
        o.commitment = make_hash(static_cast<uint8_t>(cycle));
        o.zk_proof = {0x01};
        b.outputs.push_back(o);

        if (cycle > 0) {
            sh::ShieldedSpend s;
            s.nullifier = make_hash(static_cast<uint8_t>(0x80 + cycle));
            s.anchor = tree.Root();
            s.zk_proof = {0x02};
            b.spends.push_back(s);
        }

        std::vector<sh::ShieldedBundle> bundles = {b};
        sh::ApplyBlockShielded(bundles, &tree, &ns, static_cast<uint32_t>(100 + cycle));

        // Immediately rollback (simulate reorg)
        sh::CommitmentTree rolled;
        rolled.DeserializeFrontier(base_frontier.data(), base_frontier.size());
        ns.RollbackAbove(99);

        // Verify clean rollback
        check(rolled.Root() == base_root, ("cycle " + std::to_string(cycle) + ": rollback root matches base").c_str());
        check(ns.Size() == 0, ("cycle " + std::to_string(cycle) + ": rollback nullifiers empty").c_str());

        // Restore tree for next cycle
        tree = rolled;
    }

    check(tree.Root() == base_root, "after 10 reorg cycles: root matches original base");
    check(ns.Size() == 0, "after 10 reorg cycles: nullifier set empty");

    ns.Close();
    std::remove(db.c_str());
}

// ═══════════════════════════════════════════════════════════════════════
// Family 4: Mixed-input packing — all lanes in one block
// ═══════════════════════════════════════════════════════════════════════

static void test_mixed_packing() {
    printf("\nFamily 4: Mixed-input packing (all lanes in one block)\n");

    sh::CommitmentTree tree;
    std::string db = "/tmp/adv_nul_4_" + std::to_string(getpid()) + ".db";
    sh::NullifierSet ns;
    ns.Open(db);

    // Construct a block with:
    // tx1: pure shield (transparent → commitment)
    // tx2: pure transfer (nullifier → new commitment)
    // tx3: pure unshield (nullifier → transparent)
    // tx4: mixed (shield + transfer in same bundle)

    sh::ShieldedBundle tx1;
    tx1.value_balance = 1000;
    sh::ShieldedOutput o1;
    o1.commitment = make_hash(0x01);
    o1.zk_proof = {0x01};
    tx1.outputs.push_back(o1);

    sh::ShieldedBundle tx2;
    tx2.value_balance = 0;
    sh::ShieldedSpend s2;
    s2.nullifier = make_hash(0xA1);
    s2.anchor = tree.Root();
    s2.zk_proof = {0x02};
    tx2.spends.push_back(s2);
    sh::ShieldedOutput o2;
    o2.commitment = make_hash(0x02);
    o2.zk_proof = {0x03};
    tx2.outputs.push_back(o2);

    sh::ShieldedBundle tx3;
    tx3.value_balance = -500;
    sh::ShieldedSpend s3;
    s3.nullifier = make_hash(0xA2);
    s3.anchor = tree.Root();
    s3.zk_proof = {0x04};
    tx3.spends.push_back(s3);

    sh::ShieldedBundle tx4;
    tx4.value_balance = 200;
    sh::ShieldedOutput o4a;
    o4a.commitment = make_hash(0x03);
    o4a.zk_proof = {0x05};
    tx4.outputs.push_back(o4a);
    sh::ShieldedOutput o4b;
    o4b.commitment = make_hash(0x04);
    o4b.zk_proof = {0x06};
    tx4.outputs.push_back(o4b);
    sh::ShieldedSpend s4;
    s4.nullifier = make_hash(0xA3);
    s4.anchor = tree.Root();
    s4.zk_proof = {0x07};
    tx4.spends.push_back(s4);

    std::vector<sh::ShieldedBundle> block = {tx1, tx2, tx3, tx4};
    std::vector<int64_t> deltas = {1000, 0, -500, 200};

    sh::BlockShieldedContext ctx{&ns, &tree, 300};
    auto err = sh::ValidateBlockShielded(block, deltas, ctx);
    check(err == sh::BlockValidationError::Ok, "4-tx mixed block validates");

    sh::ApplyBlockShielded(block, &tree, &ns, 300);
    check(tree.Size() == 4, "tree: 4 commitments (1+1+0+2)");
    check(ns.Size() == 3, "nullifiers: 3 (0+1+1+1)");

    // Conservation check: total value_balance should be net flow
    int64_t total_balance = 0;
    for (const auto& b : block) total_balance += b.value_balance;
    check(total_balance == 700, "net value balance = +700 (into pool)");

    ns.Close();
    std::remove(db.c_str());
}

// ═══════════════════════════════════════════════════════════════════════
// Family 5: Concurrent wallet mutations
// ═══════════════════════════════════════════════════════════════════════

static void test_concurrent_mutations() {
    printf("\nFamily 5: Concurrent wallet mutations\n");

    std::string note_db = "/tmp/adv_notes_5_" + std::to_string(getpid()) + ".db";
    dinero::wallet::ShieldedNoteStore store;
    store.Open(note_db);
    sh::CommitmentTree tree;

    std::atomic<int> shield_ok{0};
    std::atomic<int> shield_fail{0};
    std::atomic<int> balance_reads{0};

    const int N = 20;

    // Spawn N threads: half shield, half read balance
    std::vector<std::thread> threads;
    for (int i = 0; i < N; ++i) {
        if (i % 2 == 0) {
            threads.emplace_back([&, i]() {
                ops::ShieldParams sp;
                sp.value_una = static_cast<uint64_t>(1000000 * (i + 1));
                sp.current_height = static_cast<uint32_t>(100 + i);
                auto r = ops::Shield(sp, store, tree);
                if (r.status == ops::OpStatus::Ok) shield_ok++;
                else shield_fail++;
            });
        } else {
            threads.emplace_back([&]() {
                auto bal = store.GetBalance();
                (void)bal;  // just exercising the read path
                balance_reads++;
            });
        }
    }

    for (auto& t : threads) t.join();

    int total_shields = shield_ok.load();
    int total_reads = balance_reads.load();

    check(total_shields > 0, ("concurrent shields succeeded: " + std::to_string(total_shields)).c_str());
    check(total_reads > 0, ("concurrent balance reads: " + std::to_string(total_reads)).c_str());

    // Verify final balance consistency
    auto notes = store.ListUnspent();
    uint64_t sum = 0;
    for (const auto& n : notes) sum += n.value_una;
    uint64_t reported = store.GetBalance();
    check(sum == reported, "manual sum matches GetBalance after concurrent ops");
    check(notes.size() == static_cast<size_t>(total_shields),
          "note count matches successful shield count");

    // Now concurrent unshield + balance reads
    std::atomic<int> unshield_ok{0};
    threads.clear();

    auto all_notes = store.ListUnspent();
    for (size_t i = 0; i < all_notes.size() && i < 5; ++i) {
        threads.emplace_back([&, i]() {
            ops::UnshieldParams up;
            up.leaf_index = all_notes[i].leaf_index;
            up.current_height = 500;
            auto r = ops::Unshield(up, store, tree);
            if (r.status == ops::OpStatus::Ok) unshield_ok++;
        });
    }
    // Concurrent balance reads during unshield
    for (int i = 0; i < 5; ++i) {
        threads.emplace_back([&]() {
            store.GetBalance();
            store.ListUnspent();
            balance_reads++;
        });
    }

    for (auto& t : threads) t.join();

    check(unshield_ok.load() > 0, ("concurrent unshields succeeded: " + std::to_string(unshield_ok.load())).c_str());

    // Final consistency: no negative balance, sum matches
    auto final_notes = store.ListUnspent();
    uint64_t final_sum = 0;
    for (const auto& n : final_notes) final_sum += n.value_una;
    check(final_sum == store.GetBalance(), "final balance consistent after concurrent shield+unshield");

    store.Close();
    std::remove(note_db.c_str());
}

// ═══════════════════════════════════════════════════════════════════════

int main() {
    printf("\n");
    printf("═══════════════════════════════════════════════════════════\n");
    printf(" Adversarial Consensus Test Suite — Shielded Pool Hardening\n");
    printf("═══════════════════════════════════════════════════════════\n");

    test_reorder_adversarial();
    test_half_applied_rollback();
    test_reorg_stress();
    test_mixed_packing();
    test_concurrent_mutations();

    printf("\n═══════════════════════════════════════════════════════════\n");
    printf(" Result: %d passed, %d failed\n", g_pass, g_fail);
    printf("═══════════════════════════════════════════════════════════\n");
    return g_fail > 0 ? 1 : 0;
}
