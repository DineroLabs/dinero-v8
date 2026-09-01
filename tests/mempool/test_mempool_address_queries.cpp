/**
 * Mempool address-query regressions.
 *
 * Verifies that address-indexed mempool scans:
 * 1. include direct receives to the address
 * 2. include children that spend address-owned outputs created by mempool parents
 * 3. ignore unrelated transactions
 */

#include "daemon/mempool.h"
#include "mining/address_validator.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>

using namespace dinero;

static int tests_passed = 0;
static int tests_total = 0;

#define TEST_ASSERT(cond, msg) do { \
    tests_total++; \
    if (!(cond)) { \
        std::cerr << "FAIL: " << msg << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
        assert(false); \
    } else { \
        tests_passed++; \
    } \
} while(0)

static TxId makeTxId(uint32_t seed) {
    TxId id;
    std::memset(id.v.data, 0, 32);
    std::memcpy(id.v.data, &seed, 4);
    return id;
}

static std::vector<uint8_t> scriptForAddress(const std::string& address) {
    mining::AddressInfo info;
    TEST_ASSERT(mining::DecodeAddress(address, info), "test address must decode");
    auto script = mining::BuildScriptPubKey(info);
    TEST_ASSERT(!script.empty(), "test address must produce scriptPubKey");
    return script;
}

static Transaction makeTx(const std::vector<std::pair<TxId, uint32_t>>& inputs,
                          const std::vector<std::vector<uint8_t>>& output_scripts,
                          uint64_t value = 1000) {
    Transaction tx;
    tx.version = 2;
    tx.witness_version = 0xFF;

    for (const auto& [txid, vout] : inputs) {
        TxInput inp;
        inp.prevout.txid = txid;
        inp.prevout.vout = vout;
        tx.vin.push_back(inp);
    }

    for (const auto& script : output_scripts) {
        TxOutput out;
        out.value = AmountUna::Una(value);
        out.scriptPubKey = script;
        tx.vout.push_back(out);
    }

    return tx;
}

static bool containsTx(const std::vector<Transaction>& txs, const TxId& txid) {
    return std::any_of(txs.begin(), txs.end(), [&](const Transaction& tx) {
        return tx.GetTxid() == txid;
    });
}

static void test_parent_and_child_spend_are_visible() {
    std::cout << "Test 1: parent + child spend visible in address mempool query..." << std::endl;

    const std::string target_address =
        "rdin1pl300xwevn94hhr56l629fzkzxgdkp6u5msynwq3045yqsm0c5y5qyan2ec";
    const std::string other_address =
        "rdin1pt8j9vmvfmtpcxgew3hantwvdv7v3azawn25nn2qjskxmkvd4ettqs33zl9";

    const auto target_script = scriptForAddress(target_address);
    const auto other_script = scriptForAddress(other_address);

    Mempool pool(nullptr);

    Transaction parent = makeTx({{makeTxId(1), 0}}, {target_script}, 5000);
    Transaction child = makeTx({{parent.GetTxid(), 0}}, {other_script}, 4000);
    Transaction unrelated = makeTx({{makeTxId(2), 0}}, {other_script}, 3000);

    pool.addUnchecked(parent);
    pool.addUnchecked(child);
    pool.addUnchecked(unrelated);

    const auto target_txs = pool.getTransactionsForAddress(target_address);
    TEST_ASSERT(target_txs.size() == 2, "target address should see parent receive and child spend");
    TEST_ASSERT(containsTx(target_txs, parent.GetTxid()), "target address should include parent receive");
    TEST_ASSERT(containsTx(target_txs, child.GetTxid()), "target address should include child spend");
    TEST_ASSERT(!containsTx(target_txs, unrelated.GetTxid()), "target address should exclude unrelated tx");

    std::cout << "  PASSED" << std::endl;
}

static void test_unrelated_address_is_ignored() {
    std::cout << "Test 2: unrelated address query stays empty..." << std::endl;

    const std::string target_address =
        "rdin1pl300xwevn94hhr56l629fzkzxgdkp6u5msynwq3045yqsm0c5y5qyan2ec";
    const std::string unrelated_address =
        "rdin1pmx7y4k2mau9u0zjrrlwp9vjlwm9ympk8whn8v54m0c45pu6w9j8sjh2cc0";

    const auto target_script = scriptForAddress(target_address);

    Mempool pool(nullptr);
    pool.addUnchecked(makeTx({{makeTxId(3), 0}}, {target_script}, 5000));

    const auto unrelated_txs = pool.getTransactionsForAddress(unrelated_address);
    TEST_ASSERT(unrelated_txs.empty(), "unrelated address should not match other scripts");

    std::cout << "  PASSED" << std::endl;
}

static void test_p2mr_buildscriptpubkey() {
    std::cout << "Test 3: P2MR AddressInfo produces witness v3 scriptPubKey..." << std::endl;

    // Construct AddressInfo directly. No real rdin1r... bech32m test vector
    // exists in the tree, so bypass DecodeAddress; the gap fixed here is in
    // BuildScriptPubKey alone, which had no case for AddressType::P2MR.
    mining::AddressInfo info;
    info.type = mining::AddressType::P2MR;
    info.program.assign(32, 0xAB);

    auto script = mining::BuildScriptPubKey(info);

    TEST_ASSERT(script.size() == 34, "P2MR scriptPubKey must be 34 bytes (OP_3 + len + 32-byte program)");
    TEST_ASSERT(script[0] == 0x53, "P2MR scriptPubKey must start with OP_3 (0x53)");
    TEST_ASSERT(script[1] == 0x20, "P2MR scriptPubKey must push 32 bytes (0x20)");
    TEST_ASSERT(std::equal(script.begin() + 2, script.end(), info.program.begin()),
                "P2MR scriptPubKey body must equal the 32-byte program");

    std::cout << "  PASSED" << std::endl;
}

static void test_prebase_input_fee_resolution() {
    std::cout << "Test 4: frozen pre-base input contributes to mempool fee..." << std::endl;

    const auto prev_txid = makeTxId(4);
    const OutPoint expected{prev_txid, 0};
    const auto output_script = scriptForAddress(
        "rdin1pl300xwevn94hhr56l629fzkzxgdkp6u5msynwq3045yqsm0c5y5qyan2ec");

    Mempool pool(nullptr);
    pool.setPreBaseCoinPredicate([expected](const OutPoint& outpoint) {
        return outpoint == expected;
    });
    pool.setPreBaseCoinResolver([expected, output_script](const OutPoint& outpoint)
            -> std::optional<consensus::UTXOEntry> {
        if (!(outpoint == expected)) return std::nullopt;
        consensus::UTXOEntry coin;
        coin.value = AmountUna::Una(10'000);
        coin.scriptPubKey = output_script;
        coin.height = 1;
        coin.isCoinbase = false;
        return coin;
    });

    Transaction spend = makeTx({{prev_txid, 0}}, {output_script}, 9'000);
    TEST_ASSERT(pool.calculateFee(spend) == 1'000,
                "live frozen pre-base input must contribute its value to the fee");

    std::cout << "  PASSED" << std::endl;
}

int main() {
    test_parent_and_child_spend_are_visible();
    test_unrelated_address_is_ignored();
    test_p2mr_buildscriptpubkey();
    test_prebase_input_fee_resolution();

    std::cout << "\nAll mempool address-query tests passed (" << tests_passed
              << "/" << tests_total << ")" << std::endl;
    return 0;
}
