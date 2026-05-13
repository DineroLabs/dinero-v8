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

int main() {
    test_parent_and_child_spend_are_visible();
    test_unrelated_address_is_ignored();

    std::cout << "\nAll mempool address-query tests passed (" << tests_passed
              << "/" << tests_total << ")" << std::endl;
    return 0;
}
