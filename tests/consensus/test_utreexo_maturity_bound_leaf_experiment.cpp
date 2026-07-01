#include "consensus/stateless_verification.h"
#include "consensus/utreexo_accumulator.h"
#include "consensus/chainparams.h"
#include "consensus/utreexo_maturity_leaf_activation.h"
#include "primitives/amount.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "primitives/uint256.h"

#include <array>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

using namespace dinero;
using namespace dinero::consensus;

namespace {

uint256 MakeHash(uint8_t seed) {
    uint256 h;
    for (size_t i = 0; i < 32; ++i) {
        h.data[i] = static_cast<uint8_t>(seed + i);
    }
    return h;
}

std::vector<uint8_t> Script(uint8_t seed) {
    return {0x51, seed, 0xac};
}

std::string Hex(const UtreexoHash& hash) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint8_t byte : hash) {
        oss << std::setw(2) << static_cast<unsigned>(byte);
    }
    return oss.str();
}

Transaction MakeCoinbase(uint32_t height) {
    Transaction tx;
    tx.version = 2;
    tx.lockTime = 0;

    TxInput in;
    in.prevout.txid = TxId();
    in.prevout.vout = 0xffffffff;
    in.sequence = 0xffffffff;
    in.scriptSig = {
        static_cast<uint8_t>(height & 0xff),
        static_cast<uint8_t>((height >> 8) & 0xff),
        static_cast<uint8_t>((height >> 16) & 0xff),
        static_cast<uint8_t>((height >> 24) & 0xff),
    };
    tx.vin.push_back(in);

    TxOutput out;
    out.value = AmountUna::Una(GetBlockSubsidy(height));
    out.scriptPubKey = Script(0x01);
    tx.vout.push_back(out);
    return tx;
}

Transaction MakeSpend(const uint256& prev_txid, uint32_t prev_vout, uint64_t output_value) {
    Transaction tx;
    tx.version = 2;
    tx.lockTime = 0;

    TxInput in;
    in.prevout.txid = TxId(prev_txid);
    in.prevout.vout = prev_vout;
    in.sequence = 0xffffffff;
    tx.vin.push_back(in);

    TxOutput out;
    out.value = AmountUna::Una(output_value);
    out.scriptPubKey = Script(0x02);
    tx.vout.push_back(out);
    return tx;
}

Block MakeBlock(uint32_t spend_height, const uint256& prev_txid, uint64_t spend_value) {
    Block block;
    block.header.version = 1;
    block.header.prev_block_hash = MakeHash(0xa0);
    block.header.timestamp = 123456789;
    block.header.difficulty = 0x1d00ffff;
    block.header.nonce = 42;
    block.header.ZeroReserved();
    block.vtx.push_back(MakeCoinbase(spend_height));
    block.vtx.push_back(MakeSpend(prev_txid, 0, spend_value - 100));
    return block;
}

VerifyResult VerifyWithForest(const Block& block,
                              const UtreexoForest& forest,
                              const BlockUtreexoProof& proof,
                              const SpentOutputData& spent,
                              uint32_t spend_height) {
    std::vector<UtreexoHash> roots = forest.getRoots();
    StatelessContext ctx;
    ctx.spent_outputs = &spent;
    ctx.spent_count = 1;
    ctx.roots = roots.data();
    ctx.num_roots = static_cast<uint8_t>(roots.size());
    ctx.num_leaves = forest.getNumLeaves();
    ctx.height = spend_height;
    return VerifyBlockStateless(block, ctx, proof);
}

bool LegacyLeafAllowsCoinbaseMetadataLie() {
    const uint32_t coinbase_height = 200;
    const uint32_t spend_height = 250;
    const uint64_t value = 5000;
    const uint256 prev_txid = MakeHash(0x10);
    const auto script = Script(0x44);

    UtreexoForest forest;
    const UtreexoHash leaf = HashUTXOLegacy(prev_txid, 0, value, script);
    forest.add(leaf);
    BlockUtreexoProof proof = forest.generateBlockProof({leaf});
    proof.format_version = 5;

    // The witness lies: this is actually an immature coinbase, but v1/v5 leaves
    // do not authenticate either the coinbase flag or creation height.
    SpentOutputData lied(value, script, coinbase_height, false);
    Block block = MakeBlock(spend_height, prev_txid, value);
    VerifyResult result = VerifyWithForest(block, forest, proof, lied, spend_height);
    if (!result.valid()) {
        std::cout << "legacy lie unexpectedly failed with error "
                  << static_cast<int>(result.error) << "\n";
        return false;
    }
    return true;
}

bool V2LeafRejectsCoinbaseMetadataLie() {
    const uint32_t coinbase_height = UTREEXO_MATURITY_LEAF_HEIGHT_MAINNET;
    const uint32_t spend_height = coinbase_height + 50;
    const uint64_t value = 5000;
    const uint256 prev_txid = MakeHash(0x20);
    const auto script = Script(0x55);

    UtreexoForest forest;
    const UtreexoHash truthful_leaf =
        HashUTXOV2(prev_txid, 0, value, script, coinbase_height, true);
    forest.add(truthful_leaf);
    BlockUtreexoProof proof = forest.generateBlockProof({truthful_leaf});
    proof.format_version = 6;

    SpentOutputData lied(value, script, coinbase_height, false);
    Block block = MakeBlock(spend_height, prev_txid, value);
    VerifyResult result = VerifyWithForest(block, forest, proof, lied, spend_height);
    if (result.error != VerifyError::INVALID_PROOF) {
        std::cout << "v2 metadata lie failed with wrong error "
                  << static_cast<int>(result.error) << "\n";
        return false;
    }
    return true;
}

bool V2LeafRejectsTruthfulImmatureCoinbase() {
    const uint32_t coinbase_height = UTREEXO_MATURITY_LEAF_HEIGHT_MAINNET;
    const uint32_t spend_height = coinbase_height + 50;
    const uint64_t value = 5000;
    const uint256 prev_txid = MakeHash(0x30);
    const auto script = Script(0x66);

    UtreexoForest forest;
    const UtreexoHash leaf = HashUTXOV2(prev_txid, 0, value, script, coinbase_height, true);
    forest.add(leaf);
    BlockUtreexoProof proof = forest.generateBlockProof({leaf});
    proof.format_version = 6;

    SpentOutputData truthful(value, script, coinbase_height, true);
    Block block = MakeBlock(spend_height, prev_txid, value);
    VerifyResult result = VerifyWithForest(block, forest, proof, truthful, spend_height);
    if (result.error != VerifyError::COINBASE_IMMATURE) {
        std::cout << "truthful immature coinbase failed with wrong error "
                  << static_cast<int>(result.error) << "\n";
        return false;
    }
    return true;
}

bool V2LeafAllowsTruthfulMatureCoinbase() {
    const uint32_t coinbase_height = UTREEXO_MATURITY_LEAF_HEIGHT_MAINNET;
    const uint32_t spend_height = coinbase_height + 101;
    const uint64_t value = 5000;
    const uint256 prev_txid = MakeHash(0x40);
    const auto script = Script(0x77);

    UtreexoForest forest;
    const UtreexoHash leaf = HashUTXOV2(prev_txid, 0, value, script, coinbase_height, true);
    forest.add(leaf);
    BlockUtreexoProof proof = forest.generateBlockProof({leaf});
    proof.format_version = 6;

    SpentOutputData truthful(value, script, coinbase_height, true);
    Block block = MakeBlock(spend_height, prev_txid, value);
    VerifyResult result = VerifyWithForest(block, forest, proof, truthful, spend_height);
    if (!result.valid()) {
        std::cout << "truthful mature coinbase failed with error "
                  << static_cast<int>(result.error) << "\n";
        return false;
    }
    return true;
}

bool ForcedV5DowngradeOfPostForkLegacyCoinbaseRejects() {
    const uint32_t coinbase_height = UTREEXO_MATURITY_LEAF_HEIGHT_MAINNET - 10;
    const uint32_t spend_height = UTREEXO_MATURITY_LEAF_HEIGHT_MAINNET + 50;
    const uint64_t value = 5000;
    const uint256 prev_txid = MakeHash(0x50);
    const auto script = Script(0x88);

    UtreexoForest forest;
    const UtreexoHash leaf = HashUTXOLegacy(prev_txid, 0, value, script);
    forest.add(leaf);
    BlockUtreexoProof proof = forest.generateBlockProof({leaf});
    proof.format_version = 5;

    SpentOutputData spent(value, script, coinbase_height, true);
    Block block = MakeBlock(spend_height, prev_txid, value);
    VerifyResult result = VerifyWithForest(block, forest, proof, spent, spend_height);
    if (result.error != VerifyError::INVALID_PROOF) {
        std::cout << "forced v5 downgrade failed with wrong error "
                  << static_cast<int>(result.error) << "\n";
        return false;
    }
    return true;
}

bool HonestV6GraceWindowLegacyCoinbaseDefers() {
    const uint32_t coinbase_height = UTREEXO_MATURITY_LEAF_HEIGHT_MAINNET - 10;
    const uint32_t spend_height = UTREEXO_MATURITY_LEAF_HEIGHT_MAINNET + 50;
    const uint64_t value = 5000;
    const uint256 prev_txid = MakeHash(0x60);
    const auto script = Script(0x99);

    UtreexoForest forest;
    const UtreexoHash leaf = HashUTXOLegacy(prev_txid, 0, value, script);
    forest.add(leaf);
    BlockUtreexoProof proof = forest.generateBlockProof({leaf});
    proof.format_version = 6;

    SpentOutputData spent(value, script, coinbase_height, true);
    Block block = MakeBlock(spend_height, prev_txid, value);
    VerifyResult result = VerifyWithForest(block, forest, proof, spent, spend_height);
    if (!result.valid()) {
        std::cout << "honest v6 grace-window legacy spend failed with error "
                  << static_cast<int>(result.error) << "\n";
        return false;
    }
    if (!result.maturity_deferred) {
        std::cout << "honest v6 grace-window legacy spend did not defer maturity\n";
        return false;
    }
    return true;
}

bool SharedCoreAppBoundaryLeafVectorsMatchCore() {
    const std::string display_txid_hex =
        "1f1e1d1c1b1a191817161514131211100f0e0d0c0b0a09080706050403020100";
    uint256 txid;
    if (!uint256::FromHex(display_txid_hex, txid)) {
        std::cout << "failed to parse shared vector txid\n";
        return false;
    }

    const std::vector<uint8_t> script{
        0x00, 0x14, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
        0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee,
        0xff, 0x00, 0x12, 0x34, 0x56, 0x78,
    };
    const uint32_t vout = 3;
    const uint64_t amount = 123456789;

    const std::string legacy59999 =
        Hex(HashUTXOForCreationHeight(txid, vout, amount, script, 59999, true));
    const std::string v260000_coinbase =
        Hex(HashUTXOForCreationHeight(txid, vout, amount, script, 60000, true));
    const std::string v260000_noncoinbase =
        Hex(HashUTXOForCreationHeight(txid, vout, amount, script, 60000, false));

    if (legacy59999 != "4a4db8ed9bdf514b27143bf4ad017e5fc4c74f832f6e52ca073f568c2331fe55") {
        std::cout << "legacy 59999 shared vector mismatch: " << legacy59999 << "\n";
        return false;
    }
    if (v260000_coinbase != "5d7fd31a327d81bf75119ee97007da14925740a2f32d0592a652127c29e508db") {
        std::cout << "v2 60000 coinbase shared vector mismatch: "
                  << v260000_coinbase << "\n";
        return false;
    }
    if (v260000_noncoinbase != "98582354ccdfa9d5725f984ac85b4b454b7707826855c8d76072a59114422214") {
        std::cout << "v2 60000 non-coinbase shared vector mismatch: "
                  << v260000_noncoinbase << "\n";
        return false;
    }
    return true;
}

}  // namespace

int main() {
    SelectParams(Chain::MAINNET);

    struct TestCase {
        const char* name;
        bool (*fn)();
    };

    const std::array<TestCase, 7> tests{{
        {"legacy leaf allows coinbase metadata lie", LegacyLeafAllowsCoinbaseMetadataLie},
        {"v2 leaf rejects coinbase metadata lie", V2LeafRejectsCoinbaseMetadataLie},
        {"v2 leaf rejects truthful immature coinbase", V2LeafRejectsTruthfulImmatureCoinbase},
        {"v2 leaf allows truthful mature coinbase", V2LeafAllowsTruthfulMatureCoinbase},
        {"forced v5 downgrade of post-fork legacy coinbase rejects", ForcedV5DowngradeOfPostForkLegacyCoinbaseRejects},
        {"honest v6 grace-window legacy coinbase defers", HonestV6GraceWindowLegacyCoinbaseDefers},
        {"shared core/app boundary leaf vectors match core", SharedCoreAppBoundaryLeafVectorsMatchCore},
    }};

    for (const auto& test : tests) {
        std::cout << "[ RUN      ] " << test.name << "\n";
        if (!test.fn()) {
            std::cout << "[  FAILED  ] " << test.name << "\n";
            return 1;
        }
        std::cout << "[       OK ] " << test.name << "\n";
    }

    std::cout << "maturity-bound Utreexo leaf experiment passed\n";
    return 0;
}
