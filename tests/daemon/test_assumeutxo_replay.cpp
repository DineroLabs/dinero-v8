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
// those belong to header acceptance, outside the replay engine's contract.
//
// Genesis handling (mirrors fuzzer AND production ConnectTip): genesis
// (height 0) is UTXO-neutral (OP_RETURN-only outputs, zero subsidy) and is
// pre-applied by the daemon during early init, never connected through
// ConnectBlock (chainstate_service.cpp "Early init block" path). The
// deterministic chain therefore starts at height 1 over an empty fresh set;
// chain[i] is the block at height i+1.
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
#include "primitives/block.h"
#include "primitives/transaction.h"

namespace dinero {

namespace {

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

    uint256 prev_hash;  // zero: stand-in for the pre-applied genesis hash
    for (uint32_t h = 1; h <= n; ++h) {
        Block b;
        b.header.version = 1;
        b.header.prev_block_hash = prev_hash;
        b.header.timestamp = 1772841600ULL + h * 120;  // fixed past base
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

    assumeutxo::AssumeUtxoReplayEngine engine;
    std::string err;
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

}  // namespace dinero

int main(int argc, char** argv) {
    dinero::SelectParams(dinero::Chain::REGTEST);  // utreexo active from genesis on all nets
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
