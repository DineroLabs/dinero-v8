// ============================================================================
// ABC-CSN reorg replay: shielded delta recomputation parity (Task 5)
// ============================================================================
//
// BlockValidator::ComputeShieldedDeltasForStoredBlock must reproduce, BIT FOR
// BIT, the pending_shielded_deltas that the forward STATELESS per-tx loop in
// ConnectBlockInternal computes — the CSN reorg replay leg feeds them into
// ApplyBlockShieldedSection, and any divergence forks the shielded pool.
//
// Oracle construction (strongest practical form per the task brief): build a
// two-block chain with real Taproot signatures, real Utreexo proofs and real
// Spartan shielded bundles; validator A runs the FULL forward stateless
// connect (ConnectBlock, verify_root=true) — this exercises the live per-tx
// loop and applies its deltas to A's shielded state. Validator B, holding a
// clone of the pre-block shielded state, runs ComputeShieldedDeltasForStored
// Block + ApplyBlockShieldedSection on the same stored blocks. The tests then
// assert (1) the helper's deltas equal the hand-computed transparent deltas
// of the fixture and (2) B's post-state (tree root/size, nullifiers, anchors)
// is IDENTICAL to A's after every block — which can only happen if the deltas
// and the shielded section inputs match the forward path exactly.
//
// Coverage demanded by the brief: transparent-only tx interleaved BEFORE the
// shielded txs (offsets the global spent_outputs index), shield tx (+delta),
// unshield tx (-delta), multiple shielded txs in one block (global-index
// walk), spent_outputs underrun -> false, missing metadata -> false for
// shielded-bearing blocks, fallback spent_outputs source, and the
// no-shielded-tx short circuit (legacy hash-only CSN replay records must not
// brick transparent-only reorgs).
//
// Throwing/exit-nonzero checks only (gtest ASSERT/EXPECT) — never bare
// assert(), which is a no-op under NDEBUG in this Release build.
// ============================================================================

#include <gtest/gtest.h>

#include "consensus/block_validation.h"
#include "consensus/chainparams.h"
#include "consensus/consensus_utxo_set.h"
#include "consensus/shielded/anchor_history.h"
#include "consensus/shielded/binding_sig.h"
#include "consensus/shielded/bundle_builder.h"
#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/nullifier_set.h"
#include "consensus/shielded/shielded_circuit.h"
#include "consensus/shielded/shielded_serialization.h"
#include "consensus/subsidy.h"
#include "consensus/utreexo_accumulator.h"
#include "consensus/utreexo_maturity_leaf_activation.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "wallet/canonical_wallet_utxo.h"
#include "wallet/taproot_keys.h"
#include "wallet/taproot_tx_signer.h"

#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace sh = dinero::consensus::shielded;
using namespace dinero;
using namespace dinero::consensus;

namespace {

// ─────────────────────────────────────────────────────────────────────────
// Small deterministic helpers
// ─────────────────────────────────────────────────────────────────────────

uint256 MakeTestHash(uint64_t seed) {
    uint256 hash;
    for (int i = 0; i < 4; i++) {
        reinterpret_cast<uint64_t*>(hash.data)[i] = seed + i * 0x123456789ABCDEFULL;
    }
    return hash;
}

sh::Hash MakeShieldedHash(uint8_t fill) {
    sh::Hash h{};
    std::memset(h.data(), fill, sh::HASH_BYTES);
    return h;
}

sh::Hash ValueAsHash(uint64_t value) {
    sh::Hash h{};
    for (int i = 0; i < 8; ++i) {
        h[31 - i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
    }
    return h;
}

// A spendable shielded note (mirrors the reindex-equivalence fixture).
struct TestNote {
    sh::Hash secret_key;
    sh::Hash public_key;
    sh::Hash value_hash;
    sh::Hash randomness;
    sh::Hash commitment;
};

TestNote MakeSpendableNote(uint8_t seed_base) {
    TestNote note{};
    note.secret_key = MakeShieldedHash(seed_base);
    sh::Hash zero{};
    note.public_key = sh::PoseidonHash2(note.secret_key, zero);
    note.value_hash = ValueAsHash(100'000'000 + seed_base);
    note.randomness = MakeShieldedHash(static_cast<uint8_t>(seed_base + 2));
    note.commitment = sh::NoteCommitment(sh::Hash{}, note.public_key,
                                         note.value_hash, note.randomness);
    return note;
}

sh::PlannedOutput MakePlannedOutput(const TestNote& note, uint64_t value_una,
                                    uint8_t blind_seed) {
    sh::OutputWitness witness;
    witness.value = note.value_hash;
    witness.public_key = note.public_key;
    witness.randomness = note.randomness;
    witness.d = sh::Hash{};

    sh::OutputPublicInputs pub;
    pub.commitment = note.commitment;

    sh::PlannedOutput po;
    po.commitment = note.commitment;
    po.value_una = value_una;
    po.rcv = MakeShieldedHash(blind_seed);
    po.encrypted_note = std::vector<uint8_t>(32, 0xAA);
    po.output_proof = sh::ProveOutput(witness, pub, nullptr);
    EXPECT_FALSE(po.output_proof.empty()) << "failed to prove shielded output";
    po.nonce = MakeShieldedHash(static_cast<uint8_t>(blind_seed + 1));
    return po;
}

sh::PlannedSpend MakePlannedSpend(const TestNote& note, uint64_t leaf_index,
                                  const sh::CommitmentTree& tree,
                                  uint64_t value_una, uint8_t blind_seed) {
    auto auth_path = tree.GetAuthPath(leaf_index);
    EXPECT_TRUE(auth_path.has_value()) << "missing auth path for shielded spend";

    sh::SpendWitness witness;
    witness.secret_key = note.secret_key;
    witness.leaf_index = leaf_index;
    witness.value = note.value_hash;
    witness.randomness = note.randomness;
    witness.d = sh::Hash{};
    witness.merkle_path = auth_path->siblings;

    sh::SpendPublicInputs pub;
    pub.nullifier = sh::ComputeNullifier(note.secret_key, leaf_index);
    pub.anchor = tree.Root();

    sh::PlannedSpend ps;
    ps.nullifier = pub.nullifier;
    ps.anchor = pub.anchor;
    ps.value_una = value_una;
    ps.rcv = MakeShieldedHash(blind_seed);
    ps.spend_proof = sh::ProveSpend(witness, pub, nullptr);
    EXPECT_FALSE(ps.spend_proof.empty()) << "failed to prove shielded spend";
    ps.nonce = MakeShieldedHash(static_cast<uint8_t>(blind_seed + 1));
    return ps;
}

// Coinbase paying exactly the consensus subsidy to a zero-key P2TR script
// (mirrors tests/consensus/test_block_validation_invariants.cpp).
Transaction MakeCoinbase(uint32_t height) {
    Transaction coinbase;
    coinbase.version = 1;
    coinbase.lockTime = 0;
    coinbase.witness_version = 1;

    TxInput input;
    input.prevout.txid = TxId();
    input.prevout.vout = 0xffffffff;
    input.scriptSig.push_back(static_cast<uint8_t>(height & 0xFF));
    input.sequence = 0xffffffff;
    coinbase.vin.push_back(input);

    TxOutput output;
    output.value = ConsensusSubsidy::GetBlockSubsidy(height);
    output.scriptPubKey = {0x51, 0x20};
    output.scriptPubKey.resize(34, 0x00);
    coinbase.vout.push_back(output);
    return coinbase;
}

// Minimal Taproot key-path signing (deterministic, test-only). The wallet's
// TaprootKeys/TaprootTxSigner are the REAL production signing path — this is
// no consensus bypass; ValidateTaprootSpend verifies these signatures.
struct TestKey {
    std::array<uint8_t, 32> privkey{};
    std::array<uint8_t, 32> xonly_pubkey{};
    int parity = 0;
};

TestKey GenerateTestKey(uint8_t seed) {
    TestKey key;
    key.privkey.fill(seed);  // any fixed nonzero pattern is a valid scalar
    EXPECT_TRUE(TaprootKeys::DeriveXOnlyPubkey(key.privkey, key.xonly_pubkey,
                                               key.parity))
        << "failed to derive x-only pubkey";
    return key;
}

std::vector<uint8_t> CreateTaprootScriptPubKey(
    const std::array<uint8_t, 32>& xonly_pubkey) {
    std::vector<uint8_t> spk{0x51, 0x20};  // OP_1 PUSH32
    spk.insert(spk.end(), xonly_pubkey.begin(), xonly_pubkey.end());
    return spk;
}

// A funding UTXO the fixture controls end to end: consensus-set coin, forest
// leaf, and the miner-side metadata needed to produce spent_outputs entries.
struct FundingUtxo {
    OutPoint outpoint{TxId(), 0};
    uint64_t value = 0;
    std::vector<uint8_t> script;
    uint32_t created_height = 0;
    bool is_coinbase = false;
    TestKey key;
};

SpentOutputData ToSpentOutputData(const FundingUtxo& u) {
    return SpentOutputData(u.value, u.script, u.created_height, u.is_coinbase,
                           /*confidential=*/false, /*commitment=*/{});
}

UtreexoHash LeafHash(const FundingUtxo& u) {
    return HashUTXOForCreationHeight(u.outpoint.txid.AsUint256(), u.outpoint.vout,
                                     u.value, u.script, u.created_height,
                                     u.is_coinbase);
}

// Sign input `idx` of `tx` as a Taproot key-path spend. `inputs` are the
// funding metadata for ALL of tx's inputs, in order (BIP341 sighash needs
// every input's amount + scriptPubKey).
void SignInput(Transaction& tx, size_t idx,
               const std::vector<const FundingUtxo*>& inputs) {
    std::vector<CanonicalWalletUTXO> wallet_utxos;
    wallet_utxos.reserve(inputs.size());
    for (const auto* u : inputs) {
        CanonicalWalletUTXO w;
        w.txid = u->outpoint.txid.AsUint256();
        w.vout = u->outpoint.vout;
        w.value = AmountUna::Una(u->value);
        w.spk = u->script;
        w.height = u->created_height;
        w.is_coinbase = u->is_coinbase;
        wallet_utxos.push_back(std::move(w));
    }

    std::vector<uint8_t> sighash = TaprootTxSigner::ComputeTaprootSighash(
        tx, idx, wallet_utxos, TaprootTxSigner::SIGHASH_DEFAULT);
    ASSERT_EQ(sighash.size(), 32u) << "taproot sighash computation failed";

    std::array<uint8_t, 32> msg32;
    std::memcpy(msg32.data(), sighash.data(), 32);
    std::array<uint8_t, 64> sig64;
    ASSERT_TRUE(TaprootKeys::SignSchnorr(sig64, msg32, inputs[idx]->key.privkey))
        << "schnorr signing failed";

    tx.vin[idx].witness.clear();
    tx.vin[idx].witness.emplace_back(sig64.begin(), sig64.end());
}

// ─────────────────────────────────────────────────────────────────────────
// Fixture: a two-block chain, forward-connected by validator A (the oracle)
// ─────────────────────────────────────────────────────────────────────────
//
// Block 1 (height H1): coinbase
//   + tx1 transparent   (spend U1=100000 -> out 99000, implicit fee 1000)
//   + tx2 shield        (spend U2=50000  -> out 30000, fee 500, delta +19500)
//   + tx3 shield        (spend U3=70000  -> out 60000, fee 700, delta +9300)
// Block 2 (height H2): coinbase
//   + tx4 unshield      (spend tx2's 30000 change -> out 34000, fee 800,
//                        delta -4800, spends note1)
//
// Funding values are all DISTINCT so any global-index misalignment in the
// helper's spent_outputs walk changes a shielded tx's total_input_value and
// therefore its delta (caught as a value-balance mismatch or a wrong value).
struct ChainFixture {
    ConsensusUTXOSet set_a;
    std::unique_ptr<BlockValidator> validator_a;
    sh::CommitmentTree tree_a;
    sh::NullifierSet nullifiers_a;
    sh::AnchorHistory anchors_a;

    uint32_t h1 = 0;
    uint32_t h2 = 0;
    Block block1;
    Block block2;

    // Hand-computed forward deltas (second oracle, independent of any code
    // path shared between forward loop and helper).
    std::vector<int64_t> expected_deltas_block1{+19500, +9300};
    std::vector<int64_t> expected_deltas_block2{-4800};

    sh::Hash root_after_block1{};
    sh::Hash root_after_block2{};
    sh::Hash note1_nullifier{};

    bool built = false;
    std::string build_error;

    ChainFixture() { Build(); }

    void Build() {
        SelectParams(Chain::REGTEST);
        EXPECT_EQ(nullifiers_a.Open(":memory:"), sh::NullifierSet::OpenResult::Ok);

        validator_a = std::make_unique<BlockValidator>(&set_a);
        validator_a->setValidationMode(ValidationMode::STATELESS);
        validator_a->setShieldedState(&tree_a, &nullifiers_a, &anchors_a);

        const uint32_t activation = GetUtreexoMaturityLeafActivationHeight();
        h1 = activation + 1;
        h2 = activation + 2;

        // ── Funding UTXOs (v2 maturity leaves, non-coinbase) ──
        FundingUtxo u1, u2, u3;
        u1.key = GenerateTestKey(0x01);
        u2.key = GenerateTestKey(0x02);
        u3.key = GenerateTestKey(0x03);
        u1.outpoint = OutPoint(TxId(MakeTestHash(101)), 0);
        u2.outpoint = OutPoint(TxId(MakeTestHash(102)), 0);
        u3.outpoint = OutPoint(TxId(MakeTestHash(103)), 0);
        u1.value = 100000;
        u2.value = 50000;
        u3.value = 70000;
        for (FundingUtxo* u : {&u1, &u2, &u3}) {
            u->script = CreateTaprootScriptPubKey(u->key.xonly_pubkey);
            u->created_height = activation;
            u->is_coinbase = false;
            ASSERT_TRUE(set_a.AddCoin(
                u->outpoint,
                UTXOEntry(AmountUna::Una(u->value), u->script, u->created_height,
                          u->is_coinbase)));
            ASSERT_NE(set_a.GetForest().add(LeafHash(*u)), UINT64_MAX);
        }

        // ── Block 1 transactions ──
        Transaction tx1;  // transparent-only: pushes NO delta
        tx1.version = 2;
        tx1.lockTime = 0;
        tx1.witness_version = 1;
        {
            TxInput in;
            in.prevout = TxOutPoint(u1.outpoint.txid, u1.outpoint.vout);
            in.sequence = 0xfffffffe;
            tx1.vin.push_back(in);
            TxOutput out;
            out.value = AmountUna::Una(99000);
            out.scriptPubKey = u1.script;
            tx1.vout.push_back(out);
        }

        const TestNote note1 = MakeSpendableNote(0x10);
        const TestNote note2 = MakeSpendableNote(0x40);
        note1_nullifier = sh::ComputeNullifier(note1.secret_key, 0);

        Transaction tx2 = MakeShieldedSpendTx(u2, /*out_value=*/30000, /*fee=*/500);
        AttachBundle(tx2, /*spends=*/{},
                     {MakePlannedOutput(note1, /*value_una=*/19500, 0x30)});

        Transaction tx3 = MakeShieldedSpendTx(u3, /*out_value=*/60000, /*fee=*/700);
        AttachBundle(tx3, /*spends=*/{},
                     {MakePlannedOutput(note2, /*value_una=*/9300, 0x34)});

        // Witnesses only after bundles: the shielded tx sighash covers
        // version/prevouts/sequences/outputs/locktime, never witness data.
        SignInput(tx1, 0, {&u1});
        SignInput(tx2, 0, {&u2});
        SignInput(tx3, 0, {&u3});

        block1 = AssembleBlock(h1, MakeTestHash(9000), {tx1, tx2, tx3},
                               {&u1, &u2, &u3});
        ASSERT_TRUE(built) << build_error;

        BlockUndo undo1;
        std::string error;
        ASSERT_TRUE(validator_a->ConnectBlock(block1, h1, MakeTestHash(1),
                                              undo1, error, nullptr))
            << "forward connect of block1 failed: " << error;
        root_after_block1 = tree_a.Root();

        // STATELESS ConnectBlock validates but does not mutate the forest —
        // in the daemon that is StatelessNode's job. Advance it here so the
        // next block's proof generation and root continuity line up.
        AdvanceForest(block1, h1, {&u1, &u2, &u3});
        ASSERT_TRUE(built) << build_error;
        {
            // Self-check: the manually-advanced forest must reproduce the
            // committed header root, proving the fixture's leaf conventions
            // match the consensus paths'.
            const auto commitment = set_a.GetForest().getCommitment();
            ASSERT_EQ(commitment.size(), 32u);
            uint256 advanced_root;
            std::memcpy(advanced_root.data, commitment.data(), 32);
            ASSERT_EQ(advanced_root, block1.header.utreexo_root)
                << "fixture forest advance diverged from consensus root";
        }

        // ── Block 2: unshield note1, spending tx2's transparent change ──
        FundingUtxo change2;
        change2.key = u2.key;  // change went back to K2's script
        change2.outpoint = OutPoint(tx2.GetTxid(), 0);
        change2.value = 30000;
        change2.script = u2.script;
        change2.created_height = h1;
        change2.is_coinbase = false;

        // STATELESS connect advances only the forest, not the coin map, but
        // ComputeUtreexoRootPure (the miner-side root builder) resolves
        // spent inputs through the map — seed the miner's view of tx2's
        // change output before assembling block 2.
        ASSERT_TRUE(set_a.AddCoin(
            change2.outpoint,
            UTXOEntry(AmountUna::Una(change2.value), change2.script,
                      change2.created_height, change2.is_coinbase)));

        Transaction tx4 = MakeShieldedSpendTx(change2, /*out_value=*/34000,
                                              /*fee=*/800);
        AttachBundle(tx4,
                     {MakePlannedSpend(note1, /*leaf_index=*/0, tree_a,
                                       /*value_una=*/4800, 0x36)},
                     /*outputs=*/{});
        SignInput(tx4, 0, {&change2});

        block2 = AssembleBlock(h2, block1.GetHash(), {tx4}, {&change2});
        ASSERT_TRUE(built) << build_error;

        BlockUndo undo2;
        ASSERT_TRUE(validator_a->ConnectBlock(block2, h2, MakeTestHash(2),
                                              undo2, error, nullptr))
            << "forward connect of block2 failed: " << error;
        root_after_block2 = tree_a.Root();
    }

    static Transaction MakeShieldedSpendTx(const FundingUtxo& funding,
                                           uint64_t out_value, uint64_t fee) {
        Transaction tx;
        tx.version = Transaction::TX_VERSION_SHIELDED;
        tx.lockTime = 0;
        tx.witness_version = 1;
        TxInput in;
        in.prevout = TxOutPoint(funding.outpoint.txid, funding.outpoint.vout);
        in.sequence = 0xfffffffe;
        tx.vin.push_back(in);
        TxOutput out;
        out.value = AmountUna::Una(out_value);
        out.scriptPubKey = funding.script;
        tx.vout.push_back(out);
        tx.SetExplicitFee(fee);
        return tx;
    }

    static void AttachBundle(Transaction& tx,
                             const std::vector<sh::PlannedSpend>& spends,
                             const std::vector<sh::PlannedOutput>& outputs) {
        const sh::Hash sighash = sh::ComputeShieldedTxSighash(tx);
        sh::ShieldedBundle bundle;
        ASSERT_EQ(sh::BuildShieldedBundle(spends, outputs, sighash, bundle),
                  sh::BundleBuildResult::Ok)
            << "BuildShieldedBundle failed";
        tx.shielded_bundle_bytes = sh::SerializeShieldedBundle(bundle);
    }

    // Mimic StatelessNode's forest maintenance: remove the block's spent
    // leaves, then add its created outputs (same leaf-hash convention the
    // consensus paths use). No intra-block spends exist in this fixture.
    void AdvanceForest(const Block& block, uint32_t height,
                       const std::vector<const FundingUtxo*>& spent_funding) {
        built = false;
        auto& forest = set_a.GetForest();
        for (const auto* u : spent_funding) {
            auto pos = forest.findLeafPosition(LeafHash(*u));
            ASSERT_TRUE(pos.has_value()) << "spent leaf missing from forest";
            ASSERT_TRUE(forest.removeAtKnownPosition(pos.value(), LeafHash(*u)));
        }
        for (const auto& tx : block.vtx) {
            const TxId txid = tx.GetTxid();
            for (uint32_t n = 0; n < tx.vout.size(); ++n) {
                const auto& output = tx.vout[n];
                const UtreexoHash leaf = HashUTXOForCreationHeight(
                    txid.AsUint256(), n,
                    output.is_confidential ? 0 : output.value.GetUna(),
                    std::vector<uint8_t>(output.scriptPubKey.begin(),
                                         output.scriptPubKey.end()),
                    height, tx.IsCoinbase());
                ASSERT_NE(forest.add(leaf), UINT64_MAX);
            }
        }
        built = true;
    }

    // Miner-side block assembly: coinbase + txs, spent_outputs in global
    // input order, batched forest proof, root continuity + header root.
    Block AssembleBlock(uint32_t height, const uint256& prev_hash,
                        const std::vector<Transaction>& txs,
                        const std::vector<const FundingUtxo*>& spent_funding) {
        built = false;
        Block block;
        block.header.version = 1;
        block.header.prev_block_hash = prev_hash;
        block.header.timestamp = 1772496000 + height * 120;
        block.header.difficulty = 0x1d00ffff;
        block.header.nonce = 0;
        block.header.ZeroReserved();

        Transaction coinbase = MakeCoinbase(height);
        block.vtx.push_back(coinbase);
        for (const auto& tx : txs) block.vtx.push_back(tx);
        block.header.merkle_root = coinbase.GetTxid().AsUint256();

        BlockUtreexoData data;
        data.accumulator_root_before = set_a.GetForest().getCommitment();
        std::vector<UtreexoHash> targets;
        for (const auto* u : spent_funding) {
            data.spent_outputs.push_back(ToSpentOutputData(*u));
            targets.push_back(LeafHash(*u));
        }
        data.spend_proof = set_a.GetForest().generateBlockProof(
            targets, GetUtreexoProofFormatVersion(height));
        block.utreexo = data;

        uint256 computed_root;
        std::string root_error;
        if (!validator_a->ComputeUtreexoRootPure(block, height, computed_root,
                                                 root_error)) {
            build_error = "ComputeUtreexoRootPure failed: " + root_error;
            return block;
        }
        block.header.utreexo_root = computed_root;
        built = true;
        return block;
    }
};

ChainFixture& Fixture() {
    static ChainFixture* fixture = new ChainFixture();
    return *fixture;
}

// A validator-B bundle: independent BlockValidator with its own shielded
// state, replaying stored blocks through the helper + shared apply funnel.
struct ReplayValidator {
    ConsensusUTXOSet set;
    BlockValidator validator{&set};
    sh::CommitmentTree tree;
    sh::NullifierSet nullifiers;
    sh::AnchorHistory anchors;

    ReplayValidator() {
        EXPECT_EQ(nullifiers.Open(":memory:"), sh::NullifierSet::OpenResult::Ok);
        validator.setValidationMode(ValidationMode::STATELESS);
        validator.setShieldedState(&tree, &nullifiers, &anchors);
    }
};

// ─────────────────────────────────────────────────────────────────────────
// (1) THE parity test: helper deltas == hand-computed forward deltas, and
//     replaying them through ApplyBlockShieldedSection reproduces validator
//     A's post-state EXACTLY after every block.
// ─────────────────────────────────────────────────────────────────────────
TEST(CsnShieldedDeltaParity, ReplayReproducesForwardShieldedStateExactly) {
    ChainFixture& f = Fixture();
    ASSERT_FALSE(::testing::Test::HasFatalFailure());

    ReplayValidator b;

    // Block 1: transparent tx pushes no delta; two shield txs push +deltas in
    // block order. The global spent_outputs walk must skip over tx1's entry.
    std::vector<int64_t> deltas1;
    std::string error;
    ASSERT_TRUE(b.validator.ComputeShieldedDeltasForStoredBlock(
        f.block1, f.h1, deltas1, error))
        << error;
    EXPECT_EQ(deltas1, f.expected_deltas_block1);

    // Helper must not mutate validator state: same call again, same result,
    // shielded state untouched.
    const sh::Hash root_before = b.tree.Root();
    const uint64_t tree_size_before = b.tree.Size();
    const uint64_t nf_before = b.nullifiers.Size();
    std::vector<int64_t> deltas1_again;
    ASSERT_TRUE(b.validator.ComputeShieldedDeltasForStoredBlock(
        f.block1, f.h1, deltas1_again, error))
        << error;
    EXPECT_EQ(deltas1_again, deltas1);
    EXPECT_EQ(b.tree.Root(), root_before);
    EXPECT_EQ(b.tree.Size(), tree_size_before);
    EXPECT_EQ(b.nullifiers.Size(), nf_before);

    // Frontier accessor mirrors the pre-block capture in ConnectBlockInternal.
    EXPECT_EQ(b.validator.SerializeShieldedFrontier(), b.tree.SerializeFrontier());

    BlockUndo undo1;
    undo1.height = f.h1;
    const std::vector<uint8_t> frontier1 = b.validator.SerializeShieldedFrontier();
    if (!frontier1.empty()) undo1.pre_block_shielded_frontier = frontier1;
    ASSERT_TRUE(b.validator.ApplyBlockShieldedSection(f.block1, f.h1, deltas1,
                                                      undo1, error))
        << error;

    // Post-block-1 parity with the forward oracle.
    EXPECT_EQ(b.tree.Root(), f.root_after_block1);
    EXPECT_EQ(b.tree.Size(), 2u);  // note1 + note2 commitments appended
    EXPECT_EQ(b.nullifiers.Size(), 0u);
    EXPECT_TRUE(b.anchors.Contains(f.root_after_block1));

    // Block 2: unshield pushes a NEGATIVE delta; its bundle spend is
    // validated against B's own (replayed) tree/anchors — parity with A's
    // state after block 1 is what makes that validation pass.
    std::vector<int64_t> deltas2;
    ASSERT_TRUE(b.validator.ComputeShieldedDeltasForStoredBlock(
        f.block2, f.h2, deltas2, error))
        << error;
    EXPECT_EQ(deltas2, f.expected_deltas_block2);

    BlockUndo undo2;
    undo2.height = f.h2;
    ASSERT_TRUE(b.validator.ApplyBlockShieldedSection(f.block2, f.h2, deltas2,
                                                      undo2, error))
        << error;

    EXPECT_EQ(b.tree.Root(), f.root_after_block2);
    EXPECT_EQ(b.nullifiers.Size(), 1u);
    EXPECT_TRUE(b.nullifiers.Contains(f.note1_nullifier));
    EXPECT_TRUE(f.nullifiers_a.Contains(f.note1_nullifier))
        << "oracle self-check: forward connect must have inserted the nullifier";
    EXPECT_TRUE(b.anchors.Contains(f.root_after_block2));
}

// ─────────────────────────────────────────────────────────────────────────
// (2) spent_outputs underrun -> false with a distinct error, no deltas.
// ─────────────────────────────────────────────────────────────────────────
TEST(CsnShieldedDeltaParity, SpentOutputsUnderrunFailsClosed) {
    ChainFixture& f = Fixture();
    ASSERT_FALSE(::testing::Test::HasFatalFailure());

    ReplayValidator b;
    Block truncated = f.block1;
    ASSERT_TRUE(truncated.utreexo.has_value());
    ASSERT_FALSE(truncated.utreexo->spent_outputs.empty());
    truncated.utreexo->spent_outputs.pop_back();

    std::vector<int64_t> deltas;
    std::string error;
    EXPECT_FALSE(b.validator.ComputeShieldedDeltasForStoredBlock(
        truncated, f.h1, deltas, error));
    EXPECT_NE(error.find("underrun"), std::string::npos) << error;
}

// ─────────────────────────────────────────────────────────────────────────
// (3) Shielded-bearing block with NO spend metadata at all -> false, loud.
// ─────────────────────────────────────────────────────────────────────────
TEST(CsnShieldedDeltaParity, MissingSpentOutputsFailsClosedForShieldedBlock) {
    ChainFixture& f = Fixture();
    ASSERT_FALSE(::testing::Test::HasFatalFailure());

    ReplayValidator b;
    Block stripped = f.block1;
    stripped.utreexo.reset();

    std::vector<int64_t> deltas;
    std::string error;
    EXPECT_FALSE(b.validator.ComputeShieldedDeltasForStoredBlock(
        stripped, f.h1, deltas, error));
    EXPECT_NE(error.find("missing-spent-outputs"), std::string::npos) << error;
}

// ─────────────────────────────────────────────────────────────────────────
// (4) Fallback source: when block.utreexo is absent, the CSN replay-data
//     spent_outputs must yield IDENTICAL deltas.
// ─────────────────────────────────────────────────────────────────────────
TEST(CsnShieldedDeltaParity, FallbackSpentOutputsYieldIdenticalDeltas) {
    ChainFixture& f = Fixture();
    ASSERT_FALSE(::testing::Test::HasFatalFailure());

    ReplayValidator b;
    ASSERT_TRUE(f.block1.utreexo.has_value());
    const std::vector<SpentOutputData> fallback = f.block1.utreexo->spent_outputs;

    Block stripped = f.block1;
    stripped.utreexo.reset();

    std::vector<int64_t> deltas;
    std::string error;
    ASSERT_TRUE(b.validator.ComputeShieldedDeltasForStoredBlock(
        stripped, f.h1, deltas, error, &fallback))
        << error;
    EXPECT_EQ(deltas, f.expected_deltas_block1);
}

// ─────────────────────────────────────────────────────────────────────────
// (5) Transparent-only block: no deltas, and NO spend-metadata requirement
//     (legacy hash-only CSN replay records must not brick transparent
//     reorgs). Coinbase-only and transparent-spend blocks both qualify.
// ─────────────────────────────────────────────────────────────────────────
TEST(CsnShieldedDeltaParity, TransparentOnlyBlockNeedsNoSpendMetadata) {
    ChainFixture& f = Fixture();
    ASSERT_FALSE(::testing::Test::HasFatalFailure());

    ReplayValidator b;

    // Coinbase-only block without utreexo data.
    Block coinbase_only;
    coinbase_only.header.version = 1;
    coinbase_only.vtx.push_back(MakeCoinbase(f.h1));

    std::vector<int64_t> deltas{123};  // pre-poison: must come back empty
    std::string error;
    ASSERT_TRUE(b.validator.ComputeShieldedDeltasForStoredBlock(
        coinbase_only, f.h1, deltas, error))
        << error;
    EXPECT_TRUE(deltas.empty());

    // Transparent-spend block with utreexo stripped: still fine — the block
    // carries no shielded tx, so no delta computation needs the metadata.
    Block transparent = f.block1;
    transparent.utreexo.reset();
    transparent.vtx.erase(transparent.vtx.begin() + 2, transparent.vtx.end());
    ASSERT_EQ(transparent.vtx.size(), 2u);  // coinbase + tx1

    deltas = {456};
    ASSERT_TRUE(b.validator.ComputeShieldedDeltasForStoredBlock(
        transparent, f.h1, deltas, error))
        << error;
    EXPECT_TRUE(deltas.empty());
}

// ─────────────────────────────────────────────────────────────────────────
// (6) Frontier accessor: empty when shielded state is unwired.
// ─────────────────────────────────────────────────────────────────────────
TEST(CsnShieldedDeltaParity, SerializeShieldedFrontierEmptyWhenUnwired) {
    ConsensusUTXOSet set;
    BlockValidator validator(&set);
    EXPECT_TRUE(validator.SerializeShieldedFrontier().empty());
}

}  // namespace
