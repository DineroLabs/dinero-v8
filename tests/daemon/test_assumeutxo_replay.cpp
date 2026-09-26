// ============================================================================
// AssumeUTXO Replay Engine unit tests (plan Task 6)
// ============================================================================
//
// Drives the REAL consensus stack: BlockValidator::ConnectBlock (full
// validation, verify_root=true) over a fresh ConsensusUTXOSet, with a
// deterministic synthetic coinbase-only chain.
//
// Chain builder provenance (plan Step 1 findings): the Phase 2.1
// Deterministic Consensus Fuzzer (tests/consensus/consensus_fuzzer.cpp)
// builds blocks but connects them via ConsensusUTXOSet::ApplyBlock — it
// leaves header.utreexo_root null, so its blocks FAIL ConnectBlock's
// consensus-critical root check ("bad-utreexo-root"), and it has no
// reusable header (inline class in a main()-bearing binary). Its coinbase
// construction is lifted here, completed with the real mining-path step:
// BlockValidator::ComputeUtreexoRootPure fills header.utreexo_root, after
// which the block passes FULL ConnectBlock validation (coinbase subsidy
// rule, 128-byte header rule, witness-commitment rules, utreexo root
// commitment). ConnectBlock does not check PoW/merkle/prev-hash linkage —
// the replay owner now runs header acceptance and identity checks itself.
//
// Genesis handling (mirrors fuzzer AND production ConnectTip): genesis
// (height 0) is NOT UTXO-neutral — genesis coinbase outputs ARE persisted in
// ChainDB (genesis_init.cpp) and carried into the live ConsensusUTXOSet via
// BulkLoad, so ExportSnapshot includes them. SeedGenesis mirrors that seeding:
// every genesis coinbase output is inserted as a coin at height 0 / coinbase=true,
// INCLUDING OP_RETURN outputs that normal ConnectBlock would skip. This was
// discovered during Task 8 e2e regression (regtest genesis carries a 100-DIN
// OP_RETURN record at height 0 that caused every honest snapshot to mismatch
// before the fix). The deterministic chain here starts at height 1 over a
// fresh set seeded via SeedGenesis; chain[i] is the block at height i+1.
//
// SHIELDED COVERAGE: this chain is transparent-only (building consensus-valid
// shielded bundles in a unit test is heavyweight). The engine's genesis-fresh
// shielded state (CommitmentTree/NullifierSet/AnchorHistory wired via
// setShieldedState) is exercised end-to-end by the Task 8 regtest integration
// test, which replays real shielded blocks through the engine.
// ============================================================================

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "daemon/services/assumeutxo_replay.h"
#include "consensus/utxo_set_digest.h"
#include "consensus/block_validation.h"
#include "consensus/chainparams.h"
#include "consensus/consensus_utxo_set.h"
#include "consensus/subsidy.h"
#include "consensus/genesis_canonical.h"
#include "consensus/merkle_root.h"
#include "primitives/block.h"
#include "primitives/transaction.h"

namespace dinero {

namespace {

Block SelectedGenesis() {
    Block block;
    block.header = BuildCanonicalGenesis(Params()).header;
    Transaction coinbase;
    if (!TransactionSerializer::Deserialize(coinbase, Params().genesis.genesisCoinbaseHex))
        throw std::runtime_error("fixture genesis decode failed");
    block.vtx.push_back(std::move(coinbase));
    return block;
}

void SeedDirect(consensus::ConsensusUTXOSet& set) {
    const auto genesis = SelectedGenesis();
    const auto& tx = genesis.vtx.front();
    for (uint32_t i = 0; i < tx.vout.size(); ++i) {
        const auto& output = tx.vout[i];
        ASSERT_TRUE(set.AddCoin(OutPoint(tx.GetTxid(), i), consensus::UTXOEntry(
            output.value, output.scriptPubKey, 0, true, output.is_confidential, output.commitment)));
    }
}

// Real coinbase, fuzzer-style (BIP34 height in scriptSig), paying exactly
// the consensus subsidy to a deterministic height-keyed script.
Transaction MakeCoinbase(uint32_t height) {
    Transaction tx;
    tx.version = 2;
    tx.lockTime = 0;
    tx.witness_version = 0;

    TxInput in;
    in.prevout.txid = TxId();  // null txid
    in.prevout.vout = 0xffffffff;
    in.sequence = 0xffffffff;
    in.scriptSig = {static_cast<uint8_t>(height & 0xff),
                    static_cast<uint8_t>((height >> 8) & 0xff),
                    static_cast<uint8_t>((height >> 16) & 0xff),
                    static_cast<uint8_t>((height >> 24) & 0xff)};
    tx.vin.push_back(in);

    TxOutput out;
    out.value = ConsensusSubsidy::GetBlockSubsidy(height);
    std::vector<uint8_t> script(22, 0x00);  // P2WPKH shape: OP_0 PUSH20 <h>
    script[1] = 0x14;
    script[2] = static_cast<uint8_t>(height & 0xff);
    script[3] = static_cast<uint8_t>((height >> 8) & 0xff);
    out.scriptPubKey = script;
    tx.vout.push_back(out);

    return tx;
}

// Deterministic coinbase-only chain, consensus-valid from height 1.
// Mining path per block: ComputeUtreexoRootPure -> header.utreexo_root,
// then full ConnectBlock on the builder's own set to advance state.
// Returns fewer than n blocks only on builder-side validation failure.
std::vector<Block> BuildDeterministicChain(uint32_t n) {
    std::vector<Block> chain;
    consensus::ConsensusUTXOSet set;
    consensus::BlockValidator validator(&set);

    SeedDirect(set);
    uint256 prev_hash = SelectedGenesis().GetHash();
    for (uint32_t h = 1; h <= n; ++h) {
        Block b;
        b.header.version = 1;
        b.header.prev_block_hash = prev_hash;
        b.header.timestamp = SelectedGenesis().header.timestamp + h * 120;  // fixed past base
        b.header.difficulty = 0x1d00ffff;
        b.header.nonce = 0;
        b.header.ZeroReserved();
        b.vtx.push_back(MakeCoinbase(h));
        b.header.merkle_root = b.vtx[0].GetTxid().AsUint256();

        uint256 root;
        std::string err;
        if (!validator.ComputeUtreexoRootPure(b, h, root, err)) {
            ADD_FAILURE() << "ComputeUtreexoRootPure failed at height " << h
                          << ": " << err;
            break;
        }
        b.header.utreexo_root = root;

        consensus::BlockUndo undo;
        if (!validator.ConnectBlock(b, h, b.GetHash(), undo, err)) {
            ADD_FAILURE() << "builder ConnectBlock failed at height " << h
                          << ": " << err;
            break;
        }
        prev_hash = b.GetHash();
        chain.push_back(std::move(b));
    }
    return chain;
}

}  // namespace

// Replaying N valid blocks must reproduce exactly the digest of a directly
// applied set (same blocks, same order, independent instance).
TEST(AssumeUtxoReplay, ReplayReproducesDirectDigest) {
    const auto chain = BuildDeterministicChain(20);
    ASSERT_EQ(chain.size(), 20u);

    // Direct application — the "snapshot creator's" view.
    consensus::ConsensusUTXOSet direct;
    SeedDirect(direct);
    consensus::BlockValidator direct_validator(&direct);
    for (uint32_t i = 0; i < chain.size(); ++i) {
        const uint32_t h = i + 1;
        consensus::BlockUndo undo;
        std::string err;
        ASSERT_TRUE(direct_validator.ConnectBlock(chain[i], h,
                                                  chain[i].GetHash(), undo, err))
            << "height " << h << ": " << err;
    }
    const std::string expected =
        consensus::ComputeUtxoRecordsDigest(direct.GetUTXOs()).GetHex();
    ASSERT_FALSE(direct.GetUTXOs().empty());

    // Replay engine — the verifier's view.
    assumeutxo::AssumeUtxoReplayEngine engine;
    std::string err;
    ASSERT_TRUE(engine.SeedGenesis(SelectedGenesis(), err)) << err;
    for (uint32_t i = 0; i < chain.size(); ++i) {
        const uint32_t h = i + 1;
        ASSERT_TRUE(engine.ConnectAndAdvance(chain[i], h, chain[i].GetHash(), err))
            << "height " << h << ": " << err;
    }
    EXPECT_EQ(engine.RecordsDigestHex(), expected);
    EXPECT_EQ(engine.Height(), 20u);
    EXPECT_EQ(engine.UtxoCount(), direct.GetUTXOs().size());
    EXPECT_FALSE(engine.UtreexoRootHex().empty());
    // ConnectBlock proved computed root == header root at every block, so the
    // tip header's utreexo_root IS the expected final forest root.
    EXPECT_EQ(engine.UtreexoRootHex(), chain.back().header.utreexo_root.GetHex());
}

// A tampered block must fail ConnectBlock with a non-empty error, and the
// engine must stay at the last good height.
//
// Tamper mechanism (genuine ConnectBlock refusal, named per plan): decrement
// the coinbase output value of block at height 5 by 1 una. Lowering the value
// keeps the "coinbase pays too much" rule silent (value < subsidy is legal),
// but changes the coinbase's utreexo leaf hash, so the recomputed forest root
// no longer matches header.utreexo_root — ConnectBlock rejects with
// "bad-utreexo-root (ROOT_MISMATCH)". This is the same consensus commitment
// that catches a content-tampered (poisoned) snapshot during replay.
TEST(AssumeUtxoReplay, TamperedBlockFailsValidation) {
    auto chain = BuildDeterministicChain(10);
    ASSERT_EQ(chain.size(), 10u);

    chain[4].vtx[0].vout[0].value =
        AmountUna::Una(chain[4].vtx[0].vout[0].value.GetUna() - 1);

    chain[4].header.merkle_root = consensus::ComputeMerkleRoot(chain[4].vtx);
    assumeutxo::AssumeUtxoReplayEngine engine;
    std::string err;
    ASSERT_TRUE(engine.SeedGenesis(SelectedGenesis(), err)) << err;
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t h = i + 1;
        ASSERT_TRUE(engine.ConnectAndAdvance(chain[i], h, chain[i].GetHash(), err))
            << "height " << h << ": " << err;
    }
    EXPECT_FALSE(engine.ConnectAndAdvance(chain[4], 5, chain[4].GetHash(), err));
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("bad-utreexo-root"), std::string::npos) << err;
    EXPECT_EQ(engine.Height(), 4u);

    // The engine also refuses non-ascending heights (double-connect guard).
    std::string err2;
    EXPECT_FALSE(engine.ConnectAndAdvance(chain[3], 4, chain[3].GetHash(), err2));
    EXPECT_FALSE(err2.empty());
}

// Genesis coins are part of the committed set (Task 8 e2e finding): seeding
// must add the records to the digest WITHOUT adding utreexo leaves.
// Neuter check (per plan): swapping EXPECT_NE → EXPECT_EQ on RecordsDigestHex
// causes the test to fail (digests differ because b has genesis records, a
// does not), confirming the assertion is live.
TEST(AssumeUtxoReplay, SeedGenesisAddsRecordsNotLeaves) {
    const Block genesis = SelectedGenesis();

    assumeutxo::AssumeUtxoReplayEngine a;  // unseeded
    assumeutxo::AssumeUtxoReplayEngine b;  // seeded
    std::string err;
    ASSERT_TRUE(b.SeedGenesis(genesis, err)) << err;

    // Record was added: digests differ between unseeded and seeded engines.
    EXPECT_NE(a.RecordsDigestHex(), b.RecordsDigestHex());
    // Exactly the genesis coinbase outputs were recorded.
    EXPECT_EQ(b.UtxoCount(), genesis.vtx[0].vout.size());
    // No utreexo leaves were added: forest roots are identical (both empty).
    EXPECT_EQ(a.UtreexoRootHex(), b.UtreexoRootHex());
}

// Promotion inputs: the engine captures undo for the requested tail window only.
TEST(AssumeUtxoReplay, CapturesUndoTailWindow) {
    const auto chain = BuildDeterministicChain(10);
    ASSERT_EQ(chain.size(), 10u);
    assumeutxo::AssumeUtxoReplayEngine engine;
    engine.SetUndoTailWindow(3);   // capture undo for the last 3 connected heights
    std::string err;
    ASSERT_TRUE(engine.SeedGenesis(SelectedGenesis(), err)) << err;
    // genesis pre-applied per engine contract; replay 1..9
    // chain[h-1] is the block built for height h (builder: chain[i] -> height i+1)
    for (uint32_t h = 1; h < chain.size(); ++h) {
        ASSERT_TRUE(engine.ConnectAndAdvance(chain[h-1], h, chain[h-1].GetHash(), err)) << err;
    }
    const auto& tail = engine.UndoTail();          // ordered ascending by height
    ASSERT_EQ(tail.size(), 3u);
    EXPECT_EQ(tail.front().height, 7u);
    EXPECT_EQ(tail.back().height, 9u);
    // CRITICAL memory guard: ConnectBlock attaches a full pre-block UTXO-set
    // snapshot to undo when the backend supports snapshot/restore (~30MB at
    // mainnet scale). The ring must strip it — 1024 retained snapshots would
    // OOM the final pass (~30GB). The flatfile undo format never serializes
    // it; disconnect durability relies on spent_coins + frontier + UD sidecar.
    for (const auto& cu : tail) {
        EXPECT_FALSE(cu.undo.pre_block_snapshot.has_value());
    }
    // Each captured undo must be non-trivial for blocks that spend (our builder's
    // coinbase-only blocks have empty spent sets — assert the struct is present
    // and heights are right; spend-bearing undo content is e2e territory).
}

// Promotion inputs: proven-set access for the bulk coin reconcile.
TEST(AssumeUtxoReplay, ExposesProvenSetAndStateRefs) {
    const auto chain = BuildDeterministicChain(5);
    ASSERT_EQ(chain.size(), 5u);
    assumeutxo::AssumeUtxoReplayEngine engine;
    std::string err;
    ASSERT_TRUE(engine.SeedGenesis(SelectedGenesis(), err)) << err;
    // chain[h-1] is the block built for height h
    for (uint32_t h = 1; h < chain.size(); ++h) {
        ASSERT_TRUE(engine.ConnectAndAdvance(chain[h-1], h, chain[h-1].GetHash(), err)) << err;
    }
    EXPECT_EQ(engine.ProvenUtxos().size(), engine.UtxoCount());
    EXPECT_NE(engine.Forest(), nullptr);
    EXPECT_NE(engine.ShieldedTree(), nullptr);
    EXPECT_NE(engine.ShieldedNullifiers(), nullptr);
    EXPECT_NE(engine.ShieldedAnchors(), nullptr);
}


TEST(AssumeUtxoReplay, RequiresSelectedGenesisAndContiguousIdentity) {
    const auto chain = BuildDeterministicChain(2);
    ASSERT_EQ(chain.size(), 2u);
    assumeutxo::AssumeUtxoReplayEngine engine;
    std::string error;
    const auto empty = engine.RecordsDigestHex();
    EXPECT_FALSE(engine.ConnectAndAdvance(chain[0], 1, chain[0].GetHash(), error));
    EXPECT_EQ(error, "replay requires seeded selected genesis");
    auto wrong = SelectedGenesis();
    wrong.vtx.front().vout.front().value = AmountUna::Una(1);
    EXPECT_FALSE(engine.SeedGenesis(wrong, error));
    EXPECT_EQ(engine.RecordsDigestHex(), empty);
    wrong = SelectedGenesis();
    ++wrong.header.nonce;
    EXPECT_FALSE(engine.SeedGenesis(wrong, error));
    ASSERT_TRUE(engine.SeedGenesis(SelectedGenesis(), error)) << error;
    const auto baseline = engine.RecordsDigestHex();
    EXPECT_FALSE(engine.SeedGenesis(SelectedGenesis(), error));
    EXPECT_FALSE(engine.ConnectAndAdvance(chain[1], 2, chain[1].GetHash(), error));
    EXPECT_FALSE(engine.ConnectAndAdvance(chain[0], 1, uint256{}, error));
    wrong = chain[0];
    wrong.header.prev_block_hash = uint256{};
    EXPECT_FALSE(engine.ConnectAndAdvance(wrong, 1, wrong.GetHash(), error));
    wrong = chain[0];
    wrong.vtx.front().vout.front().value = AmountUna::Una(1);
    EXPECT_FALSE(engine.ConnectAndAdvance(wrong, 1, wrong.GetHash(), error));
    EXPECT_EQ(error, "replay block identity, parent or Merkle mismatch");
    EXPECT_EQ(engine.Height(), 0u);
    EXPECT_EQ(engine.RecordsDigestHex(), baseline);
    ASSERT_TRUE(engine.ConnectAndAdvance(chain[0], 1, chain[0].GetHash(), error)) << error;
}

TEST(AssumeUtxoReplay, OwnsHeaderValidationBeforeCoinEffects) {
    const auto chain = BuildDeterministicChain(2);
    ASSERT_EQ(chain.size(), 2u);
    assumeutxo::AssumeUtxoReplayEngine engine;
    std::string error;
    ASSERT_TRUE(engine.SeedGenesis(SelectedGenesis(), error)) << error;
    const auto baseline = engine.RecordsDigestHex();
    for (int field = 0; field < 3; ++field) {
        auto wrong = chain[0];
        if (field == 0) wrong.header.version = 0;
        if (field == 1) wrong.header.timestamp = SelectedGenesis().header.timestamp;
        if (field == 2) wrong.header.difficulty = 0;
        EXPECT_FALSE(engine.ConnectAndAdvance(wrong, 1, wrong.GetHash(), error));
        EXPECT_EQ(error, "replay header validation failed");
        EXPECT_EQ(engine.RecordsDigestHex(), baseline);
        EXPECT_EQ(engine.Height(), 0u);
    }
    ASSERT_TRUE(engine.ConnectAndAdvance(chain[0], 1, chain[0].GetHash(), error)) << error;
    ASSERT_TRUE(engine.ConnectAndAdvance(chain[1], 2, chain[1].GetHash(), error)) << error;
}

TEST(AssumeUtxoReplay, TimeLocksUseOwnedAncestryWithoutGlobalIndex) {
    const uint32_t saved = MutableParams().contextual_locks_activation_height;
    struct Restore { uint32_t value; ~Restore() { MutableParams().contextual_locks_activation_height = value; } } restore{saved};
    MutableParams().contextual_locks_activation_height = 1;
    const auto chain = BuildDeterministicChain(102);
    ASSERT_EQ(chain.size(), 102u);
    assumeutxo::AssumeUtxoReplayEngine engine;
    std::string error;
    ASSERT_TRUE(engine.SeedGenesis(SelectedGenesis(), error)) << error;
    for (uint32_t h = 1; h <= 101; ++h)
        ASSERT_TRUE(engine.ConnectAndAdvance(chain[h-1], h, chain[h-1].GetHash(), error)) << error;
    const auto before = engine.RecordsDigestHex();
    auto block = chain[101];
    Transaction spend;
    spend.version = 2;
    TxInput input;
    input.prevout.txid = chain.front().vtx.front().GetTxid();
    input.prevout.vout = 0;
    input.sequence = (1U << 22) | 65535U;
    spend.vin.push_back(input);
    spend.vout.push_back(chain.front().vtx.front().vout.front());
    block.vtx.push_back(spend);
    block.header.merkle_root = consensus::ComputeMerkleRoot(block.vtx);
    EXPECT_FALSE(engine.ConnectAndAdvance(block, 102, block.GetHash(), error));
    EXPECT_NE(error.find("non-final-relative-time-lock"), std::string::npos) << error;
    EXPECT_EQ(engine.RecordsDigestHex(), before);
    EXPECT_EQ(engine.Height(), 101u);
    block.vtx.back().vin.front().sequence = UINT32_MAX - 1;
    block.vtx.back().lockTime = static_cast<uint32_t>(chain[95].header.timestamp);
    block.header.merkle_root = consensus::ComputeMerkleRoot(block.vtx);
    EXPECT_FALSE(engine.ConnectAndAdvance(block, 102, block.GetHash(), error));
    EXPECT_NE(error.find("non-final-absolute-lock"), std::string::npos) << error;
    EXPECT_EQ(engine.RecordsDigestHex(), before);
    block.vtx.back().vin.front().sequence = 1U << 22;
    block.vtx.back().lockTime = 0;
    block.header.merkle_root = consensus::ComputeMerkleRoot(block.vtx);
    EXPECT_FALSE(engine.ConnectAndAdvance(block, 102, block.GetHash(), error));
    // A mature time lock reaches mandatory script validation; the unsigned
    // fixture must still refuse and leave the actual replay state unchanged.
    EXPECT_NE(error.find("Script validation failed"), std::string::npos) << error;
    EXPECT_EQ(engine.RecordsDigestHex(), before);
    ASSERT_TRUE(engine.ConnectAndAdvance(chain[101], 102, chain[101].GetHash(), error)) << error;


}

}  // namespace dinero

int main(int argc, char** argv) {
    dinero::SelectParams(dinero::Chain::REGTEST);  // utreexo active from genesis on all nets
    // state_commitment_v1: dormant for this suite — its blocks are hand-built
    // without coinbase DNRS commitments and its subject is replay digest
    // equivalence, not commitment enforcement (which the state-commitment
    // suites and the forged-snapshot e2e exercise on the active default).
    dinero::MutableParams().state_commitment_activation_height = UINT32_MAX;
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
