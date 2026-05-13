/**
 * Shielded pool round-trip test — commitment tree, nullifier set,
 * and validation boundary enforcement.
 *
 * Proves:
 *   1. Commitment tree append + root computation is deterministic
 *   2. Nullifier set prevents double-spend
 *   3. ValidateShieldedBundle rejects duplicate nullifiers
 *   4. ApplyShieldedBundle inserts commitments and nullifiers
 *   5. NullifierSet::RollbackAbove correctly reverts
 *   6. Tree serialization/deserialization round-trips
 */

#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/nullifier_set.h"
#include "consensus/shielded/shielded_tx.h"
#include "consensus/shielded/shielded_validation.h"
#include "consensus/shielded/shielded_circuit.h"
#include "consensus/shielded/shielded_commitment.h"
#include "consensus/shielded/shielded_serialization.h"
#include "consensus/shielded/shielded_block_validation.h"
#include "wallet/shielded_note_store.h"
#include "wallet/shielded_wallet_ops.h"
#include "zk/zkvm/r1cs.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace sh = dinero::consensus::shielded;

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const char* desc) {
    if (cond) {
        printf("  [PASS] %s\n", desc);
        ++g_pass;
    } else {
        printf("  [FAIL] %s\n", desc);
        ++g_fail;
    }
}

static sh::Hash make_hash(uint8_t fill) {
    sh::Hash h{};
    std::memset(h.data(), fill, sh::HASH_BYTES);
    return h;
}

struct TestNote {
    sh::Hash secret_key;
    sh::Hash public_key;
    sh::Hash value_hash;
    sh::Hash randomness;
    sh::Hash commitment;
};

static TestNote make_spendable_note(uint8_t seed_base) {
    TestNote note{};
    note.secret_key = make_hash(seed_base);
    sh::Hash zero{};
    note.public_key = sh::PoseidonHash2(note.secret_key, zero);
    note.value_hash = make_hash(static_cast<uint8_t>(seed_base + 1));
    note.randomness = make_hash(static_cast<uint8_t>(seed_base + 2));
    note.commitment = sh::NoteCommitment(
        note.value_hash, note.public_key, note.randomness);
    return note;
}

static sh::ShieldedOutput make_proven_output(const TestNote& note) {
    sh::OutputWitness ow;
    ow.value = note.value_hash;
    ow.public_key = note.public_key;
    ow.randomness = note.randomness;

    sh::OutputPublicInputs opi;
    opi.commitment = note.commitment;

    sh::ShieldedOutput out;
    out.commitment = note.commitment;
    out.zk_proof = sh::ProveOutput(ow, opi, nullptr);
    return out;
}

int main() {
    printf("\n=== Shielded Pool Round-Trip Tests ===\n\n");

    // ── Test 1: Commitment tree basics ──
    printf("Test 1: Commitment tree\n");
    {
        sh::CommitmentTree tree;
        check(tree.Size() == 0, "empty tree size=0");

        sh::Hash root0 = tree.Root();
        check(root0 == tree.Root(), "empty root is deterministic");

        sh::Hash cm1 = sh::NoteCommitment(make_hash(0x01), make_hash(0x02), make_hash(0x03));
        uint64_t idx1 = tree.Append(cm1);
        check(idx1 == 0, "first leaf index=0");
        check(tree.Size() == 1, "size=1 after first append");

        sh::Hash root1 = tree.Root();
        check(root1 != root0, "root changes after append");

        sh::Hash cm2 = sh::NoteCommitment(make_hash(0x04), make_hash(0x05), make_hash(0x06));
        uint64_t idx2 = tree.Append(cm2);
        check(idx2 == 1, "second leaf index=1");

        sh::Hash root2 = tree.Root();
        check(root2 != root1, "root changes after second append");

        // Determinism: rebuild the same tree, get the same root.
        sh::CommitmentTree tree2;
        tree2.Append(cm1);
        tree2.Append(cm2);
        check(tree2.Root() == root2, "deterministic: same leaves → same root");
    }

    // ── Test 2: Nullifier set ──
    printf("\nTest 2: Nullifier set\n");
    {
        std::string db_path = "/tmp/shielded_test_nullifiers_" +
            std::to_string(getpid()) + ".db";
        sh::NullifierSet ns;
        auto rc = ns.Open(db_path);
        check(rc == sh::NullifierSet::OpenResult::Ok, "nullifier DB opened");

        sh::Hash nf1 = sh::ComputeNullifier(make_hash(0xAA), 0);
        check(!ns.Contains(nf1), "fresh nullifier not in set");

        bool inserted = ns.Insert(nf1, 100);
        check(inserted, "insert nullifier succeeds");
        check(ns.Contains(nf1), "nullifier found after insert");

        bool dup = ns.Insert(nf1, 101);
        check(!dup, "duplicate nullifier rejected");

        sh::Hash nf2 = sh::ComputeNullifier(make_hash(0xBB), 1);
        ns.Insert(nf2, 102);
        check(ns.Size() == 2, "size=2 after two inserts");

        // Rollback: remove everything above height 100
        ns.RollbackAbove(100);
        check(ns.Contains(nf1), "nf1 at h=100 survives rollback");
        check(!ns.Contains(nf2), "nf2 at h=102 removed by rollback");
        check(ns.Size() == 1, "size=1 after rollback");

        ns.Close();
        std::remove(db_path.c_str());
    }

    // ── Test 3: Validate + Apply shielded bundle ──
    printf("\nTest 3: Bundle validation\n");
    {
        sh::CommitmentTree tree;
        std::string db_path = "/tmp/shielded_test_validate_" +
            std::to_string(getpid()) + ".db";
        sh::NullifierSet ns;
        ns.Open(db_path);

        // Shield: 1 output, 0 spends. Value flows in from transparent.
        sh::ShieldedBundle shield_bundle;
        shield_bundle.value_balance = 1000;  // 1000 una flowing in
        const TestNote note1 = make_spendable_note(0x10);
        sh::ShieldedOutput out1 = make_proven_output(note1);
        shield_bundle.outputs.push_back(out1);

        sh::ValidationContext ctx;
        ctx.nullifier_set = &ns;
        ctx.commitment_tree = &tree;
        ctx.block_height = 5000;
        ctx.transparent_value_delta = 1000;  // matches value_balance

        auto err = sh::ValidateShieldedBundle(shield_bundle, ctx);
        check(err == sh::ShieldedValidationError::Ok, "shield bundle validates");

        sh::ApplyShieldedBundle(shield_bundle, &tree, &ns, 5000);
        check(tree.Size() == 1, "tree has 1 commitment after shield");
        check(ns.Size() == 0, "no nullifiers for shield-only");

        // Transfer: spend the shielded note, create a new one.
        sh::ShieldedBundle transfer_bundle;
        transfer_bundle.value_balance = 0;  // balanced transfer
        auto path1 = tree.GetAuthPath(0);
        check(path1.has_value(), "wallet auth path available for first note");

        sh::SpendWitness spend_witness1;
        spend_witness1.secret_key = note1.secret_key;
        spend_witness1.leaf_index = 0;
        spend_witness1.value = note1.value_hash;
        spend_witness1.randomness = note1.randomness;
        spend_witness1.merkle_path = path1->siblings;

        sh::SpendPublicInputs spend_inputs1;
        spend_inputs1.nullifier = sh::ComputeNullifier(note1.secret_key, 0);
        spend_inputs1.anchor = tree.Root();

        sh::ShieldedSpend spend1;
        spend1.nullifier = spend_inputs1.nullifier;
        spend1.anchor = spend_inputs1.anchor;
        spend1.zk_proof = sh::ProveSpend(spend_witness1, spend_inputs1, nullptr);
        transfer_bundle.spends.push_back(spend1);

        const TestNote note2 = make_spendable_note(0x40);
        sh::ShieldedOutput out2 = make_proven_output(note2);
        transfer_bundle.outputs.push_back(out2);

        ctx.transparent_value_delta = 0;
        err = sh::ValidateShieldedBundle(transfer_bundle, ctx);
        check(err == sh::ShieldedValidationError::Ok, "transfer bundle validates");

        sh::ApplyShieldedBundle(transfer_bundle, &tree, &ns, 5001);
        check(tree.Size() == 2, "tree has 2 commitments after transfer");
        check(ns.Size() == 1, "1 nullifier after transfer");

        // Double-spend: reuse same nullifier → must reject.
        sh::ShieldedBundle double_spend;
        double_spend.value_balance = 0;
        double_spend.spends.push_back(spend1);  // same nullifier!
        const TestNote note3 = make_spendable_note(0x70);
        sh::ShieldedOutput out3 = make_proven_output(note3);
        double_spend.outputs.push_back(out3);
        ctx.transparent_value_delta = 0;

        err = sh::ValidateShieldedBundle(double_spend, ctx);
        check(err == sh::ShieldedValidationError::NullifierDuplicate, "double-spend rejected");

        // Value balance mismatch
        sh::ShieldedBundle bad_balance;
        bad_balance.value_balance = 999;  // claims 999
        ctx.transparent_value_delta = 1000;  // but transparent says 1000
        err = sh::ValidateShieldedBundle(bad_balance, ctx);
        // Empty bundle with non-zero balance... actually empty bundle returns Ok.
        // Let me test with a non-empty bundle.
        const TestNote note4 = make_spendable_note(0x90);
        sh::ShieldedOutput out4 = make_proven_output(note4);
        bad_balance.outputs.push_back(out4);
        err = sh::ValidateShieldedBundle(bad_balance, ctx);
        check(err == sh::ShieldedValidationError::ValueBalanceMismatch, "value balance mismatch rejected");

        ns.Close();
        std::remove(db_path.c_str());
    }

    // ── Test 4: Tree serialization round-trip ──
    printf("\nTest 4: Frontier serialization\n");
    {
        sh::CommitmentTree tree;
        for (int i = 0; i < 100; ++i) {
            tree.Append(make_hash(static_cast<uint8_t>(i)));
        }
        sh::Hash root_before = tree.Root();

        auto frontier = tree.SerializeFrontier();
        check(frontier.size() == 8 + sh::TREE_DEPTH * sh::HASH_BYTES, "frontier size correct");

        sh::CommitmentTree restored;
        bool ok = restored.DeserializeFrontier(frontier.data(), frontier.size());
        check(ok, "deserialization succeeds");
        check(restored.Size() == 100, "restored size=100");
        check(restored.Root() == root_before, "restored root matches");
    }

    // ── Test 5: ZK circuit construction ──
    printf("\nTest 5: ZK circuit construction\n");
    {
        // Build an output circuit and verify it produces a non-empty proof.
        sh::OutputWitness ow;
        ow.value      = make_hash(0x10);
        ow.public_key = make_hash(0x20);
        ow.randomness = make_hash(0x30);

        sh::OutputPublicInputs opi;
        opi.commitment = sh::NoteCommitment(ow.value, ow.public_key, ow.randomness);

        auto proof = sh::ProveOutput(ow, opi, nullptr);
        check(!proof.empty(), "output proof non-empty");
        check(proof[0] == 0x02, "output proof version byte");
        check(sh::VerifyOutput(proof, opi, nullptr), "output proof verifies");

        // Build a spend circuit. We need a tree with a commitment in it.
        sh::CommitmentTree tree;
        const TestNote spend_note1 = make_spendable_note(0xAA);
        uint64_t leaf_idx = tree.Append(spend_note1.commitment);
        auto auth_path1 = tree.GetAuthPath(leaf_idx);
        check(auth_path1.has_value(), "auth path available for first spend");

        sh::SpendWitness sw;
        sw.secret_key = spend_note1.secret_key;
        sw.leaf_index = leaf_idx;
        sw.value = spend_note1.value_hash;
        sw.randomness = spend_note1.randomness;
        sw.merkle_path = auth_path1->siblings;

        sh::SpendPublicInputs spi;
        spi.nullifier = sh::ComputeNullifier(spend_note1.secret_key, leaf_idx);
        spi.anchor    = tree.Root();

        auto sproof = sh::ProveSpend(sw, spi, nullptr);
        check(!sproof.empty(), "spend proof non-empty");
        check(sproof[0] == 0x01, "spend proof version byte");
        check(sh::VerifySpend(sproof, spi, nullptr), "spend proof verifies");

        const TestNote spend_note2 = make_spendable_note(0xB0);
        uint64_t leaf_idx2 = tree.Append(spend_note2.commitment);
        check(leaf_idx2 == 1, "second spendable note index=1");
        auto auth_path2 = tree.GetAuthPath(leaf_idx2);
        check(auth_path2.has_value(), "auth path available for second spend");

        sh::SpendWitness sw2;
        sw2.secret_key = spend_note2.secret_key;
        sw2.leaf_index = leaf_idx2;
        sw2.value = spend_note2.value_hash;
        sw2.randomness = spend_note2.randomness;
        sw2.merkle_path = auth_path2->siblings;

        sh::SpendPublicInputs spi2;
        spi2.nullifier = sh::ComputeNullifier(spend_note2.secret_key, leaf_idx2);
        spi2.anchor = tree.Root();

        auto sproof2 = sh::ProveSpend(sw2, spi2, nullptr);
        check(!sproof2.empty(), "nonzero leaf spend proof non-empty");
        check(sh::VerifySpend(sproof2, spi2, nullptr), "nonzero leaf spend proof verifies");

        const auto output_cs = sh::BuildOutputCircuit(ow, opi);
        const auto spend_cs = sh::BuildSpendCircuit(sw, spi);
        printf("  output circuit: %zu constraints\n", output_cs.num_constraints());
        check(output_cs.num_constraints() > 400,
              "output circuit has >400 constraints (2x Poseidon)");
        printf("  spend circuit:  %zu constraints\n", spend_cs.num_constraints());
        check(spend_cs.num_constraints() > 7000,
              "spend circuit has >7000 constraints (32-level Merkle + Poseidon)");
    }

    // ── Test 6: Block commitment round-trip ──
    printf("\nTest 6: Block commitment\n");
    {
        sh::CommitmentTree tree;
        tree.Append(make_hash(0x01));
        tree.Append(make_hash(0x02));
        sh::Hash root = tree.Root();
        uint64_t nul_count = 42;

        auto script = sh::BuildShieldedCommitmentScript(root, nul_count);
        check(script.size() == 2 + sh::SHIELDED_COMMITMENT_SIZE, "script size correct");
        check(script[0] == 0x6a, "OP_RETURN");

        std::vector<std::vector<uint8_t>> outputs = {script};
        auto extracted = sh::ExtractShieldedCommitment(outputs);
        check(extracted.has_value(), "commitment extracted from coinbase");
        check(extracted->tree_root == root, "extracted root matches");
        check(extracted->nullifier_count == nul_count, "extracted count matches");
        check(sh::VerifyShieldedCommitment(*extracted, tree, nul_count), "commitment verifies");
    }

    // ── Test 7: Shield + Unshield wallet round-trip ──
    printf("\nTest 7: Shield + Unshield wallet ops\n");
    {
        std::string note_db = "/tmp/shielded_test_notes_" +
            std::to_string(getpid()) + ".db";
        dinero::wallet::ShieldedNoteStore store;
        auto rc = store.Open(note_db);
        check(rc == dinero::wallet::ShieldedNoteStore::OpenResult::Ok, "note store opened");

        sh::CommitmentTree tree;

        // Shield 10 DIN (1,000,000,000 una)
        namespace ops = dinero::wallet::shielded_ops;
        ops::ShieldParams sp;
        sp.value_una = 1000000000;
        sp.current_height = 5000;

        auto shield_r = ops::Shield(sp, store, tree);
        check(shield_r.status == ops::OpStatus::Ok, "shield succeeds");
        check(tree.Size() == 1, "tree has 1 commitment after shield");
        check(!shield_r.output_proof.empty(), "shield has output proof");
        check(store.GetBalance() == 1000000000, "shielded balance = 10 DIN");

        // List unspent notes
        auto notes = store.ListUnspent();
        check(notes.size() == 1, "1 unspent note");
        check(notes[0].value_una == 1000000000, "note value matches");

        // Unshield the note
        ops::UnshieldParams up;
        up.leaf_index = shield_r.leaf_index;
        up.current_height = 5001;

        auto unshield_r = ops::Unshield(up, store, tree);
        check(unshield_r.status == ops::OpStatus::Ok, "unshield succeeds");
        check(unshield_r.value_una == 1000000000, "unshield value = 10 DIN");
        check(!unshield_r.spend_proof.empty(), "unshield has spend proof");
        check(store.GetBalance() == 0, "shielded balance = 0 after unshield");

        // Double-spend: unshield same note again
        auto dbl = ops::Unshield(up, store, tree);
        check(dbl.status == ops::OpStatus::InvalidParams, "double-unshield rejected");

        store.Close();
        std::remove(note_db.c_str());
    }

    // ── Test 8: Canonical serialization round-trip ──
    printf("\nTest 8: Canonical serialization\n");
    {
        sh::ShieldedBundle bundle;
        bundle.value_balance = 1000;

        sh::ShieldedSpend s1;
        s1.nullifier = make_hash(0xBB);
        s1.anchor    = make_hash(0xCC);
        s1.zk_proof  = {0x01, 0x02};

        sh::ShieldedSpend s2;
        s2.nullifier = make_hash(0xAA);  // should sort BEFORE s1
        s2.anchor    = make_hash(0xCC);
        s2.zk_proof  = {0x03};

        bundle.spends = {s1, s2};  // wrong order

        sh::ShieldedOutput o1;
        o1.commitment    = make_hash(0xFF);
        o1.encrypted_note = {0x10, 0x20};
        o1.zk_proof      = {0x04};
        bundle.outputs = {o1};
        bundle.binding_sig = make_hash(0xDD);

        auto bytes = sh::SerializeShieldedBundle(bundle);
        check(!bytes.empty(), "serialization produces bytes");

        sh::ShieldedBundle decoded{};
        auto err = sh::DeserializeShieldedBundle(bytes, &decoded);
        check(err == sh::BundleDecodeError::Ok, "deserialization OK");
        check(decoded.value_balance == 1000, "value_balance round-trips");
        check(decoded.spends.size() == 2, "2 spends");
        check(decoded.outputs.size() == 1, "1 output");

        // Spends must be in canonical order (sorted by nullifier).
        check(decoded.spends[0].nullifier < decoded.spends[1].nullifier,
              "spends in canonical order (nullifier ascending)");

        // Re-serialize must produce identical bytes.
        auto reserialized = sh::SerializeShieldedBundle(decoded);
        check(reserialized == bytes, "re-serialization is identical");

        // Break canonical ordering: swap the two spend entries so
        // nullifiers are descending instead of ascending. The
        // deserializer must reject this as OrderViolation.
        auto swapped = sh::SerializeShieldedBundle(decoded);
        // Find the two 32-byte nullifiers and swap them.
        // Layout: [8 bytes value_balance] [1 byte num_spends=2]
        //         [32 bytes nul_0] [32 bytes anchor_0] [varint proof_0] ...
        //         [32 bytes nul_1] ...
        // We need to swap nul_0 and nul_1 in place.
        if (swapped.size() > 9 + 32 + 32 + 3 + 32) {
            size_t nul0_off = 9;  // after 8-byte balance + 1-byte compactsize
            // Find nul1 offset: nul0 + 32 (nul) + 32 (anchor) + varint + proof
            size_t proof0_len_off = nul0_off + 32 + 32;
            uint64_t proof0_len = swapped[proof0_len_off]; // single-byte varint
            size_t nul1_off = proof0_len_off + 1 + proof0_len;
            if (nul1_off + 32 <= swapped.size()) {
                for (size_t i = 0; i < 32; ++i) {
                    std::swap(swapped[nul0_off + i], swapped[nul1_off + i]);
                }
            }
        }
        sh::ShieldedBundle bad_order{};
        auto terr = sh::DeserializeShieldedBundle(swapped, &bad_order);
        check(terr == sh::BundleDecodeError::OrderViolation ||
              terr == sh::BundleDecodeError::NotCanonical,
              "out-of-order nullifiers rejected");
    }

    // ── Test 9: Block-level validation ──
    printf("\nTest 9: Block-level validation\n");
    {
        sh::CommitmentTree tree;
        std::string db_path = "/tmp/shielded_block_test_" +
            std::to_string(getpid()) + ".db";
        sh::NullifierSet ns;
        ns.Open(db_path);

        // Two valid bundles: shield 500 each.
        sh::ShieldedBundle b1;
        b1.value_balance = 500;
        sh::ShieldedOutput o1;
        o1.commitment = make_hash(0x10);
        o1.zk_proof = {0x01};
        b1.outputs.push_back(o1);

        sh::ShieldedBundle b2;
        b2.value_balance = 300;
        sh::ShieldedOutput o2;
        o2.commitment = make_hash(0x20);
        o2.zk_proof = {0x02};
        b2.outputs.push_back(o2);

        std::vector<sh::ShieldedBundle> bundles = {b1, b2};
        std::vector<int64_t> deltas = {500, 300};

        sh::BlockShieldedContext ctx;
        ctx.existing_nullifiers = &ns;
        ctx.pre_block_tree = &tree;
        ctx.block_height = 100;

        auto err = sh::ValidateBlockShielded(bundles, deltas, ctx);
        check(err == sh::BlockValidationError::Ok, "two shield bundles pass block validation");

        // Apply and verify deterministic ordering.
        sh::ApplyBlockShielded(bundles, &tree, &ns, 100);
        check(tree.Size() == 2, "tree has 2 commitments after block apply");
        check(ns.Size() == 0, "0 nullifiers (shield-only)");

        // Inter-tx nullifier duplicate: same nullifier in two bundles.
        sh::ShieldedBundle b3;
        b3.value_balance = 0;
        sh::ShieldedSpend dup_spend;
        dup_spend.nullifier = make_hash(0xDD);
        dup_spend.anchor = tree.Root();
        dup_spend.zk_proof = {0x03};
        b3.spends.push_back(dup_spend);
        sh::ShieldedOutput o3;
        o3.commitment = make_hash(0x30);
        o3.zk_proof = {0x04};
        b3.outputs.push_back(o3);

        sh::ShieldedBundle b4 = b3;  // exact same spend!
        b4.outputs[0].commitment = make_hash(0x40);

        std::vector<sh::ShieldedBundle> dup_bundles = {b3, b4};
        std::vector<int64_t> dup_deltas = {0, 0};
        err = sh::ValidateBlockShielded(dup_bundles, dup_deltas, ctx);
        check(err == sh::BlockValidationError::InterTxNullifierDuplicate,
              "inter-tx nullifier duplicate rejected at block level");

        // Conservation violation: value_balance doesn't match delta.
        sh::ShieldedBundle bad_bal;
        bad_bal.value_balance = 999;
        sh::ShieldedOutput ob;
        ob.commitment = make_hash(0x50);
        ob.zk_proof = {0x05};
        bad_bal.outputs.push_back(ob);

        std::vector<sh::ShieldedBundle> bad_bundles = {bad_bal};
        std::vector<int64_t> bad_deltas = {1000};  // mismatch!
        err = sh::ValidateBlockShielded(bad_bundles, bad_deltas, ctx);
        check(err == sh::BlockValidationError::GlobalConservationViolation,
              "conservation violation rejected at block level");

        ns.Close();
        std::remove(db_path.c_str());
    }

    // ── Test 10: Wallet-side shielded note sync ──
    printf("\nTest 10: Wallet-side shielded note sync\n");
    {
        std::string note_db = "/tmp/shielded_test_sync_" +
            std::to_string(getpid()) + ".db";
        dinero::wallet::ShieldedNoteStore store;
        auto rc = store.Open(note_db);
        check(rc == dinero::wallet::ShieldedNoteStore::OpenResult::Ok, "sync note store opened");

        const TestNote note = make_spendable_note(0xC1);
        check(store.AddPendingNote(250000000, note.secret_key, note.public_key,
                                   note.randomness, note.commitment, 6000),
              "pending note stored");
        check(store.GetBalance() == 0, "pending note excluded from spendable balance");

        check(store.AppendChainLeaf(note.commitment, 0, 6001), "chain leaf appended");
        check(store.ConfirmNote(note.commitment, 0, 6001), "pending note confirmed");
        check(store.GetBalance() == 250000000, "confirmed note enters spendable balance");

        auto confirmed = store.GetByLeafIndex(0);
        check(confirmed.has_value(), "confirmed note retrievable by leaf index");
        check(confirmed->confirmed, "confirmed flag set");

        const auto expected_nf = sh::ComputeNullifier(note.secret_key, 0);
        check(store.MarkSpentByNullifier(expected_nf, 6002), "mark spent by nullifier");
        check(store.GetBalance() == 0, "spent note leaves spendable balance");

        check(store.UnmarkSpentByNullifier(expected_nf), "unmark spent by nullifier");
        check(store.GetBalance() == 250000000, "unspent note restored after rollback");

        check(store.UnconfirmNote(note.commitment), "note unconfirmed on disconnect");
        check(store.GetBalance() == 0, "unconfirmed note removed from spendable balance");

        auto all_notes = store.ListAll();
        check(all_notes.size() == 1, "note row preserved after unconfirm");
        check(!all_notes[0].confirmed, "note row reverted to pending");

        check(store.TruncateChainLeaves(0), "chain leaves truncated");
        check(store.LoadChainLeaves().empty(), "chain leaf stream cleared");

        store.Close();
        std::remove(note_db.c_str());
    }

    // ── Test 11: Chain-owned frontier/nullifier reopen equivalence ──
    printf("\nTest 11: Chain-owned frontier/nullifier reopen equivalence\n");
    {
        namespace fs = std::filesystem;

        const fs::path base_dir = fs::temp_directory_path() /
            ("shielded_chainstate_reopen_" + std::to_string(getpid()));
        const fs::path frontier_path = base_dir / "shielded_frontier.bin";
        const fs::path nullifier_path = base_dir / "shielded_nullifiers.db";
        std::error_code ec;
        fs::remove_all(base_dir, ec);
        fs::create_directories(base_dir, ec);

        sh::CommitmentTree tree;
        sh::NullifierSet ns;
        auto rc = ns.Open(nullifier_path.string());
        check(rc == sh::NullifierSet::OpenResult::Ok, "chain-owned nullifier DB opened");

        // Step 1: shield one note into the consensus-owned tree.
        const TestNote note1 = make_spendable_note(0xD0);
        sh::ShieldedBundle shield_bundle;
        shield_bundle.value_balance = 1000;
        shield_bundle.outputs.push_back(make_proven_output(note1));

        sh::ValidationContext ctx;
        ctx.nullifier_set = &ns;
        ctx.commitment_tree = &tree;
        ctx.block_height = 7000;
        ctx.transparent_value_delta = 1000;

        auto err = sh::ValidateShieldedBundle(shield_bundle, ctx);
        check(err == sh::ShieldedValidationError::Ok, "initial shield bundle validates");
        sh::ApplyShieldedBundle(shield_bundle, &tree, &ns, 7000);

        // Step 2: spend that note into a second note so both frontier and
        // nullifier DB have non-trivial state before persistence.
        const auto note1_path = tree.GetAuthPath(0);
        check(note1_path.has_value(), "auth path available before persistence");

        sh::SpendWitness spend_witness1;
        spend_witness1.secret_key = note1.secret_key;
        spend_witness1.leaf_index = 0;
        spend_witness1.value = note1.value_hash;
        spend_witness1.randomness = note1.randomness;
        spend_witness1.merkle_path = note1_path->siblings;

        sh::SpendPublicInputs spend_inputs1;
        spend_inputs1.nullifier = sh::ComputeNullifier(note1.secret_key, 0);
        spend_inputs1.anchor = tree.Root();

        sh::ShieldedSpend spend1;
        spend1.nullifier = spend_inputs1.nullifier;
        spend1.anchor = spend_inputs1.anchor;
        spend1.zk_proof = sh::ProveSpend(spend_witness1, spend_inputs1, nullptr);

        const TestNote note2 = make_spendable_note(0xE0);
        sh::ShieldedBundle transfer_bundle;
        transfer_bundle.value_balance = 0;
        transfer_bundle.spends.push_back(spend1);
        transfer_bundle.outputs.push_back(make_proven_output(note2));
        ctx.block_height = 7001;
        ctx.transparent_value_delta = 0;

        err = sh::ValidateShieldedBundle(transfer_bundle, ctx);
        check(err == sh::ShieldedValidationError::Ok, "pre-persist transfer bundle validates");
        sh::ApplyShieldedBundle(transfer_bundle, &tree, &ns, 7001);

        const auto expected_root = tree.Root();
        const auto expected_size = tree.Size();
        const auto expected_nullifier_count = ns.Size();
        check(expected_size == 2, "pre-persist tree size is non-trivial");
        check(expected_nullifier_count == 1, "pre-persist nullifier count is non-trivial");

        // Precompute the *next* spend while wallet-style auth-path knowledge is
        // still available. After restart, consensus only needs the frontier
        // root + nullifier DB to validate and apply the already-constructed
        // bundle; it does not reconstruct proving paths from the frontier.
        const auto note2_path = tree.GetAuthPath(1);
        check(note2_path.has_value(), "auth path available before persistence for next spend");

        sh::SpendWitness spend_witness2;
        spend_witness2.secret_key = note2.secret_key;
        spend_witness2.leaf_index = 1;
        spend_witness2.value = note2.value_hash;
        spend_witness2.randomness = note2.randomness;
        spend_witness2.merkle_path = note2_path->siblings;

        sh::SpendPublicInputs spend_inputs2;
        spend_inputs2.nullifier = sh::ComputeNullifier(note2.secret_key, 1);
        spend_inputs2.anchor = expected_root;

        sh::ShieldedSpend spend2;
        spend2.nullifier = spend_inputs2.nullifier;
        spend2.anchor = spend_inputs2.anchor;
        spend2.zk_proof = sh::ProveSpend(spend_witness2, spend_inputs2, nullptr);

        const TestNote note3 = make_spendable_note(0xF0);
        sh::ShieldedBundle post_reopen_bundle;
        post_reopen_bundle.value_balance = 0;
        post_reopen_bundle.spends.push_back(spend2);
        post_reopen_bundle.outputs.push_back(make_proven_output(note3));

        const auto frontier = tree.SerializeFrontier();
        {
            std::ofstream out(frontier_path, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(frontier.data()),
                      static_cast<std::streamsize>(frontier.size()));
        }
        ns.Close();

        // Step 3: reopen the exact persisted artifacts as chain-owned state.
        sh::CommitmentTree restored;
        std::ifstream in(frontier_path, std::ios::binary);
        std::vector<uint8_t> persisted_frontier((std::istreambuf_iterator<char>(in)),
                                                std::istreambuf_iterator<char>());
        check(!persisted_frontier.empty(), "persisted frontier bytes written");
        check(restored.DeserializeFrontier(persisted_frontier.data(),
                                           persisted_frontier.size()),
              "persisted frontier deserializes");

        sh::NullifierSet reopened;
        rc = reopened.Open(nullifier_path.string());
        check(rc == sh::NullifierSet::OpenResult::Ok, "chain-owned nullifier DB reopened");
        check(restored.Size() == expected_size, "reopened frontier size matches");
        check(restored.Root() == expected_root, "reopened frontier root matches");
        check(reopened.Size() == expected_nullifier_count, "reopened nullifier count matches");
        check(reopened.Contains(spend1.nullifier), "reopened nullifier membership matches");

        // Step 4: prove the reopened state is usable for the next consensus
        // transition, not just byte-stable. The bundle was constructed before
        // persistence using wallet/prover state, but consensus validation after
        // reopen only depends on the restored frontier root and nullifier DB.

        sh::ValidationContext restored_ctx;
        restored_ctx.nullifier_set = &reopened;
        restored_ctx.commitment_tree = &restored;
        restored_ctx.block_height = 7002;
        restored_ctx.transparent_value_delta = 0;

        err = sh::ValidateShieldedBundle(post_reopen_bundle, restored_ctx);
        check(err == sh::ShieldedValidationError::Ok, "post-reopen transfer validates");
        sh::ApplyShieldedBundle(post_reopen_bundle, &restored, &reopened, 7002);
        check(restored.Size() == expected_size + 1, "post-reopen tree stays usable");
        check(reopened.Size() == expected_nullifier_count + 1, "post-reopen nullifier DB stays usable");

        reopened.Close();
        fs::remove_all(base_dir, ec);
    }

    // ── Summary ──
    printf("\n=== Result: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
