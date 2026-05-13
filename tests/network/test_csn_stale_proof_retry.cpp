/**
 * Focused regression: stale proof rejection must immediately re-request.
 *
 * This exercises the stateless sync path directly:
 *   1. Seed the CSN with the confirmed pre-state for block N
 *   2. Deliver a stale proof for block N+1 (wrong root_before)
 *   3. Ensure the CSN bans the sender and immediately re-requests the block
 *   4. Deliver a fresh proof for the same block and confirm the CSN advances
 */

#include "consensus/interfaces/iutxo_provider.h"
#include "consensus/utreexo_accumulator.h"
#include "network/bridge_node.h"
#include "network/stateless_node.h"
#include "network/utreexo_messages.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

using namespace dinero;
using namespace dinero::consensus;
using namespace dinero::network;

namespace {

int tests_passed = 0;
int tests_total = 0;

#define TEST_ASSERT(cond, msg) do { \
    tests_total++; \
    if (!(cond)) { \
        std::cerr << "FAIL: " << msg << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
        assert(false); \
    } else { \
        tests_passed++; \
    } \
} while (0)

class MockUTXOProvider : public IUTXOProvider {
public:
    void AddTestUTXO(const TxId& txid, uint32_t vout,
                     uint64_t value, const std::vector<uint8_t>& script,
                     uint32_t height = 1) {
        OutPoint op(txid, vout);
        UTXOEntry entry(AmountUna::Una(value), script, height, false);
        utxos_[op] = entry;
    }

    std::optional<UTXOEntry> GetUTXO(const OutPoint& outpoint) const override {
        auto it = utxos_.find(outpoint);
        if (it == utxos_.end()) return std::nullopt;
        return it->second;
    }

    bool AddUTXO(const OutPoint& outpoint, const UTXOEntry& entry) override {
        utxos_[outpoint] = entry;
        return true;
    }

    bool SpendUTXO(const OutPoint& outpoint, uint32_t) override {
        return utxos_.erase(outpoint) > 0;
    }

    bool DeleteUTXO(const OutPoint& outpoint) override {
        utxos_.erase(outpoint);
        return true;
    }

    bool HasUTXO(const OutPoint& outpoint) const override {
        return utxos_.count(outpoint) > 0;
    }

private:
    std::unordered_map<OutPoint, UTXOEntry> utxos_;
};

std::vector<uint8_t> makeScript() {
    std::vector<uint8_t> script = {0x51, 0x20};
    script.resize(34, 0x00);
    return script;
}

Block makeCoinbaseBlock(uint32_t height,
                        const std::vector<std::pair<uint64_t, std::vector<uint8_t>>>& outputs,
                        const uint256& prev_hash = uint256()) {
    Block block;
    std::memset(&block.header, 0, sizeof(BlockHeader));
    block.header.version = 1;
    block.header.prev_block_hash = prev_hash;
    block.header.timestamp = 1700000000 + height * 600;
    block.header.difficulty = 0x1d00ffff;
    block.header.nonce = height;

    Transaction coinbase;
    coinbase.version = 2;
    coinbase.witness_version = 0xFF;
    TxInput cb_in;
    cb_in.prevout.vout = 0xFFFFFFFF;
    cb_in.scriptSig.resize(4);
    std::memcpy(cb_in.scriptSig.data(), &height, 4);
    coinbase.vin.push_back(cb_in);

    for (const auto& [value, script] : outputs) {
        TxOutput out;
        out.value = AmountUna::Una(value);
        out.scriptPubKey = script;
        coinbase.vout.push_back(out);
    }

    block.vtx.push_back(coinbase);
    return block;
}

struct TrackedUTXO {
    TxId txid;
    uint32_t vout;
    uint64_t value;
    std::vector<uint8_t> scriptPubKey;
    UtreexoHash leafHash;
};

std::vector<TrackedUTXO> extractUTXOsFromBlock(const Block& block) {
    std::vector<TrackedUTXO> result;
    for (const auto& tx : block.vtx) {
        TxId txid = tx.GetTxid();
        for (size_t n = 0; n < tx.vout.size(); n++) {
            TrackedUTXO u;
            u.txid = txid;
            u.vout = static_cast<uint32_t>(n);
            u.value = tx.vout[n].value.GetUna();
            u.scriptPubKey = tx.vout[n].scriptPubKey;
            u.leafHash = HashUTXO(txid.AsUint256(), u.vout, u.value, u.scriptPubKey);
            result.push_back(u);
        }
    }
    return result;
}

void test_stale_proof_retries_immediately() {
    std::cout << "CSN stale proof retry regression..." << std::endl;

    UtreexoForest bridge_forest;
    auto mock_utxo = std::make_shared<MockUTXOProvider>();
    BridgeNode bridge(mock_utxo, &bridge_forest);
    auto script = makeScript();

    Block block1 = makeCoinbaseBlock(1, {
        {1000, script}, {2000, script}, {3000, script}, {4000, script}
    });
    auto utxos_1 = extractUTXOsFromBlock(block1);
    for (const auto& u : utxos_1) {
        bridge_forest.add(u.leafHash);
        mock_utxo->AddTestUTXO(u.txid, u.vout, u.value, u.scriptPubKey, 1);
    }

    UtreexoForest csn_forest = bridge_forest;
    StatelessNode csn(&csn_forest);
    csn.SyncToForestState(1);

    Block block2;
    std::memset(&block2.header, 0, sizeof(BlockHeader));
    block2.header.version = 1;
    block2.header.prev_block_hash = block1.GetHash();
    block2.header.timestamp = 1700001200;
    block2.header.difficulty = 0x1d00ffff;
    block2.header.nonce = 2;

    Transaction coinbase2;
    coinbase2.version = 2;
    coinbase2.witness_version = 0xFF;
    TxInput cb_in2;
    cb_in2.prevout.vout = 0xFFFFFFFF;
    uint32_t h2 = 2;
    cb_in2.scriptSig.resize(4);
    std::memcpy(cb_in2.scriptSig.data(), &h2, 4);
    coinbase2.vin.push_back(cb_in2);
    coinbase2.vout.push_back(TxOutput(AmountUna::Una(5000), script));
    block2.vtx.push_back(coinbase2);

    Transaction spend_tx;
    spend_tx.version = 2;
    spend_tx.witness_version = 0xFF;
    TxInput in0;
    in0.prevout.txid = utxos_1[0].txid;
    in0.prevout.vout = utxos_1[0].vout;
    spend_tx.vin.push_back(in0);
    TxInput in2;
    in2.prevout.txid = utxos_1[2].txid;
    in2.prevout.vout = utxos_1[2].vout;
    spend_tx.vin.push_back(in2);
    spend_tx.vout.push_back(TxOutput(AmountUna::Una(7000), script));
    block2.vtx.push_back(spend_tx);

    UtreexoHash root_before_2 = bridge_forest.getCommitment();
    BlockUtreexoData proof_data_2 = bridge.GenerateProofForBlock(block2, 2);

    for (const auto& target : proof_data_2.spend_proof.targets) {
        auto pos = bridge_forest.findLeafPosition(target);
        TEST_ASSERT(pos.has_value(), "Bridge must locate deletion target");
        auto proof = bridge_forest.prove(*pos);
        TEST_ASSERT(proof.has_value(), "Bridge must prove deletion target");
        TEST_ASSERT(bridge_forest.remove(target, *proof), "Bridge must remove deletion target");
    }
    mock_utxo->SpendUTXO(OutPoint(utxos_1[0].txid, utxos_1[0].vout), 2);
    mock_utxo->SpendUTXO(OutPoint(utxos_1[2].txid, utxos_1[2].vout), 2);

    auto utxos_2 = extractUTXOsFromBlock(block2);
    for (const auto& u : utxos_2) {
        bridge_forest.add(u.leafHash);
        mock_utxo->AddTestUTXO(u.txid, u.vout, u.value, u.scriptPubKey, 2);
    }

    UtreexoProofMessage fresh_pm2;
    fresh_pm2.block_hash = block2.GetHash();
    fresh_pm2.block_height = 2;
    fresh_pm2.accumulator_root_before = root_before_2;
    fresh_pm2.accumulator_root_after = bridge_forest.getCommitment();
    fresh_pm2.proof_data = proof_data_2;

    UtreexoProofMessage stale_pm2 = fresh_pm2;
    stale_pm2.accumulator_root_before[0] ^= 0x5a;
    TEST_ASSERT(stale_pm2.accumulator_root_before != fresh_pm2.accumulator_root_before,
                "Stale proof must have a different root_before");

    std::vector<std::vector<uint256>> retries;
    std::vector<std::string> bans;
    csn.setBlockProvider([&](const uint256& hash) -> std::optional<Block> {
        if (hash == block2.GetHash()) {
            return block2;
        }
        return std::nullopt;
    });
    csn.setProofRequester([&](const std::vector<uint256>& block_hashes) {
        retries.push_back(block_hashes);
    });
    csn.setPeerBanCallback([&](const std::string& peer_id, const std::string& reason) {
        bans.push_back(peer_id + ":" + reason);
    });

    csn.TrackExternalProofRequest(block2.GetHash());

    TEST_ASSERT(!csn.onProofResponse("stale-peer", stale_pm2),
                "CSN must reject stale proof response");
    TEST_ASSERT(bans.size() == 1, "CSN must ban the stale proof peer");
    TEST_ASSERT(retries.size() == 1, "CSN must immediately re-request after stale proof rejection");
    TEST_ASSERT(retries[0].size() == 1 && retries[0][0] == block2.GetHash(),
                "Immediate retry must target the rejected block");

    TEST_ASSERT(csn.onProofResponse("fresh-peer", fresh_pm2),
                "CSN must accept the refreshed proof response");
    TEST_ASSERT(csn.GetSyncHeight() == 2, "CSN sync height must advance after accepting fresh proof");
    TEST_ASSERT(csn.GetCurrentAccumulatorRoot() == bridge_forest.getCommitment(),
                "CSN root must match bridge after retry recovery");

    std::cout << "  PASSED" << std::endl;
}

}  // namespace

int main() {
    std::cout << std::endl;
    std::cout << "═══════════════════════════════════════════════════" << std::endl;
    std::cout << "  CSN Stale Proof Retry Regression" << std::endl;
    std::cout << "═══════════════════════════════════════════════════" << std::endl;

    test_stale_proof_retries_immediately();

    std::cout << std::endl;
    std::cout << "═══════════════════════════════════════════════════" << std::endl;
    std::cout << "  All " << tests_passed << "/" << tests_total
              << " assertions passed!" << std::endl;
    std::cout << "═══════════════════════════════════════════════════" << std::endl;

    return tests_passed == tests_total ? 0 : 1;
}
