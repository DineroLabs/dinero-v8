/**
 * Phase G.3: Mempool Relay - Integration Tests
 *
 * Test Coverage:
 * - G.3.1: Single transaction propagation (Alice → Bob)
 * - G.3.2: Multi-peer relay (Alice → Bob → Charlie)
 * - G.3.3: Invalid transaction rejection
 * - G.3.4: Duplicate prevention
 */

#include "daemon/tx_relay_manager.h"
#include "primitives/transaction.h"
#include "primitives/uint256.h"
#include "primitives/hash_domains.h"  // Phase M.4.3-B: TxId type
#include "common/ilogger.h"
#include <iostream>
#include <cassert>
#include <unordered_map>
#include <vector>

using namespace dinero;

// ============================================================================
// Mock Logger
// ============================================================================

class MockLogger : public ILogger {
public:
    void setLogLevel(LogLevel level) override {}
    void setLogFile(const std::string& filename) override {}
    void shutdown() override {}

    void log(LogLevel level, const std::string& message) override {
        std::cout << "[LOG] " << message << std::endl;
    }
    void debug(const std::string& message) override {
        std::cout << "[DEBUG] " << message << std::endl;
    }
    void info(const std::string& message) override {
        std::cout << "[INFO] " << message << std::endl;
    }
    void warning(const std::string& message) override {
        std::cout << "[WARN] " << message << std::endl;
    }
    void error(const std::string& message) override {
        std::cout << "[ERROR] " << message << std::endl;
    }
};

// ============================================================================
// Mock P2P Message Sink
// ============================================================================

class MockP2PMessageSink {
public:
    struct Message {
        std::string peer_address;
        std::string command;
        std::vector<uint8_t> payload;
    };

    std::vector<Message> messages;

    void SendMessage(const std::string& peer_addr, const std::string& cmd, const std::vector<uint8_t>& payload) {
        messages.push_back({peer_addr, cmd, payload});
        std::cout << "[P2P] Sent " << cmd << " to " << (peer_addr.empty() ? "ALL" : peer_addr)
                  << " (" << payload.size() << " bytes)" << std::endl;
    }

    size_t count_command(const std::string& cmd) {
        size_t count = 0;
        for (const auto& msg : messages) {
            if (msg.command == cmd) ++count;
        }
        return count;
    }

    bool has_command(const std::string& cmd) {
        return count_command(cmd) > 0;
    }
};

// ============================================================================
// Mock Mempool
// ============================================================================

class MockMempool {
public:
    std::unordered_map<uint256, Transaction> transactions;

    bool AddTransaction(const Transaction& tx) {
        uint256 txid = tx.GetTxid().AsUint256();

        // Simple validation: reject if version is 0 (invalid)
        if (tx.version == 0) {
            std::cout << "[Mempool] Transaction rejected (invalid version): "
                      << txid.GetHex().substr(0, 16) << "..." << std::endl;
            return false;
        }

        transactions[txid] = tx;
        std::cout << "[Mempool] Transaction accepted: "
                  << txid.GetHex().substr(0, 16) << "..." << std::endl;
        return true;
    }

    bool GetTransaction(const uint256& txid, Transaction& out_tx) const {
        auto it = transactions.find(txid);
        if (it == transactions.end()) {
            return false;
        }
        out_tx = it->second;
        return true;
    }
};

// ============================================================================
// Test G.3.1: Single Transaction Propagation (Alice → Bob)
// ============================================================================

void test_g3_1_single_tx_propagation() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test G.3.1: Single Transaction Propagation" << std::endl;
    std::cout << "========================================\n" << std::endl;

    MockLogger logger_alice, logger_bob;
    MockP2PMessageSink p2p_alice, p2p_bob;
    MockMempool mempool_alice, mempool_bob;

    // Setup Alice (has the transaction)
    TxRelayManager relay_alice(&logger_alice);

    // Setup Bob (receives the transaction)
    TxRelayManager relay_bob(&logger_bob);

    // Wire Alice's send callback
    relay_alice.SetSendMessageCallback([&p2p_alice](
        const std::string& peer_addr,
        const std::string& cmd,
        const std::vector<uint8_t>& payload
    ) {
        p2p_alice.SendMessage(peer_addr, cmd, payload);
    });

    // Wire Alice's retrieve callback
    relay_alice.SetRetrieveTxCallback([&mempool_alice](
        const uint256& txid,
        Transaction& out_tx
    ) -> bool {
        return mempool_alice.GetTransaction(txid, out_tx);
    });

    // Wire Bob's send callback
    relay_bob.SetSendMessageCallback([&p2p_bob](
        const std::string& peer_addr,
        const std::string& cmd,
        const std::vector<uint8_t>& payload
    ) {
        p2p_bob.SendMessage(peer_addr, cmd, payload);
    });

    // Wire Bob's validate callback
    relay_bob.SetValidateTxCallback([&mempool_bob](
        const Transaction& tx,
        const std::string& peer_addr
    ) -> bool {
        return mempool_bob.AddTransaction(tx);
    });

    // Create a simple transaction
    Transaction tx;
    tx.version = 2;
    tx.lockTime = 0;
    tx.witness_version = 0;

    // Add a simple input
    TxInput in;
    in.prevout.txid = TxId(uint256::FromHexUnsafe("1111111111111111111111111111111111111111111111111111111111111111"));
    in.prevout.vout = 0;
    in.scriptSig = {0x01, 0x02, 0x03};
    tx.vin.push_back(in);

    // Add a simple output
    TxOutput out;
    // Phase M.6.3: Use AmountUna wrapper for type safety
    out.value = AmountUna::Una(50 * 100000000ULL);  // 50 coins
    out.scriptPubKey = {0x04, 0x05, 0x06};
    tx.vout.push_back(out);

    uint256 txid = tx.GetTxid().AsUint256();

    std::cout << "Transaction ID: " << txid.GetHex().substr(0, 16) << "...\n" << std::endl;

    // Step 1: Alice adds transaction to mempool and announces it
    assert(mempool_alice.AddTransaction(tx) == true);
    relay_alice.AnnounceTx(txid);

    // Verify: Alice broadcast inv
    assert(p2p_alice.has_command("inv") == true);
    std::cout << "✅ Alice announced transaction via inv\n" << std::endl;

    // Step 2: Bob receives inv and requests transaction
    relay_bob.HandleInv("alice:8333", txid);

    // Verify: Bob sent getdata
    assert(p2p_bob.has_command("getdata") == true);
    std::cout << "✅ Bob requested transaction via getdata\n" << std::endl;

    // Step 3: Alice receives getdata and sends transaction
    relay_alice.HandleGetData("bob:8333", txid);

    // Verify: Alice sent tx message
    assert(p2p_alice.has_command("tx") == true);
    std::cout << "✅ Alice sent transaction\n" << std::endl;

    // Step 4: Bob receives transaction
    relay_bob.HandleTx("alice:8333", tx);

    // Verify: Bob accepted transaction
    assert(mempool_bob.GetTransaction(txid, tx) == true);
    std::cout << "✅ Bob accepted transaction into mempool\n" << std::endl;

    std::cout << "✅ Test G.3.1 PASSED: Single transaction propagation successful\n" << std::endl;
}

// ============================================================================
// Test G.3.2: Multi-Peer Relay (Alice → Bob → Charlie)
// ============================================================================

void test_g3_2_multi_peer_relay() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test G.3.2: Multi-Peer Relay" << std::endl;
    std::cout << "========================================\n" << std::endl;

    MockLogger logger_alice, logger_bob, logger_charlie;
    MockP2PMessageSink p2p_alice, p2p_bob, p2p_charlie;
    MockMempool mempool_alice, mempool_bob, mempool_charlie;

    TxRelayManager relay_alice(&logger_alice);
    TxRelayManager relay_bob(&logger_bob);
    TxRelayManager relay_charlie(&logger_charlie);

    // Wire all send callbacks
    relay_alice.SetSendMessageCallback([&p2p_alice](auto addr, auto cmd, auto payload) {
        p2p_alice.SendMessage(addr, cmd, payload);
    });
    relay_bob.SetSendMessageCallback([&p2p_bob](auto addr, auto cmd, auto payload) {
        p2p_bob.SendMessage(addr, cmd, payload);
    });
    relay_charlie.SetSendMessageCallback([&p2p_charlie](auto addr, auto cmd, auto payload) {
        p2p_charlie.SendMessage(addr, cmd, payload);
    });

    // Wire all retrieve callbacks
    relay_alice.SetRetrieveTxCallback([&mempool_alice](auto txid, auto& out_tx) {
        return mempool_alice.GetTransaction(txid, out_tx);
    });
    relay_bob.SetRetrieveTxCallback([&mempool_bob](auto txid, auto& out_tx) {
        return mempool_bob.GetTransaction(txid, out_tx);
    });

    // Wire all validate callbacks
    relay_bob.SetValidateTxCallback([&mempool_bob, &relay_bob](auto tx, auto peer) -> bool {
        bool accepted = mempool_bob.AddTransaction(tx);
        if (accepted) {
            relay_bob.AnnounceTx(tx.GetTxid().AsUint256());  // Bob relays to peers
        }
        return accepted;
    });
    relay_charlie.SetValidateTxCallback([&mempool_charlie](auto tx, auto peer) {
        return mempool_charlie.AddTransaction(tx);
    });

    // Create transaction
    Transaction tx;
    tx.version = 2;
    tx.lockTime = 0;
    tx.witness_version = 0;

    TxInput in;
    in.prevout.txid = TxId(uint256::FromHexUnsafe("2222222222222222222222222222222222222222222222222222222222222222"));
    in.prevout.vout = 0;
    in.scriptSig = {0x07, 0x08, 0x09};
    tx.vin.push_back(in);

    TxOutput out;
    // Phase M.6.3: Use AmountUna wrapper for type safety
    out.value = AmountUna::Una(25 * 100000000ULL);
    out.scriptPubKey = {0x0a, 0x0b, 0x0c};
    tx.vout.push_back(out);

    uint256 txid = tx.GetTxid().AsUint256();

    std::cout << "Transaction ID: " << txid.GetHex().substr(0, 16) << "...\n" << std::endl;

    // Step 1: Alice announces
    mempool_alice.AddTransaction(tx);
    relay_alice.AnnounceTx(txid);

    // Step 2: Bob receives inv, requests, and receives tx
    relay_bob.HandleInv("alice:8333", txid);
    relay_alice.HandleGetData("bob:8333", txid);
    relay_bob.HandleTx("alice:8333", tx);

    // Verify: Bob accepted and relayed
    assert(mempool_bob.GetTransaction(txid, tx) == true);
    assert(p2p_bob.has_command("inv") == true);  // Bob relayed
    std::cout << "✅ Bob accepted and relayed transaction\n" << std::endl;

    // Step 3: Charlie receives from Bob
    relay_charlie.HandleInv("bob:8333", txid);
    relay_bob.HandleGetData("charlie:8333", txid);
    relay_charlie.HandleTx("bob:8333", tx);

    // Verify: Charlie accepted
    assert(mempool_charlie.GetTransaction(txid, tx) == true);
    std::cout << "✅ Charlie accepted transaction\n" << std::endl;

    std::cout << "✅ Test G.3.2 PASSED: Multi-peer relay successful\n" << std::endl;
}

// ============================================================================
// Test G.3.3: Invalid Transaction Rejection
// ============================================================================

void test_g3_3_invalid_tx_rejection() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test G.3.3: Invalid Transaction Rejection" << std::endl;
    std::cout << "========================================\n" << std::endl;

    MockLogger logger_alice, logger_bob;
    MockP2PMessageSink p2p_alice, p2p_bob;
    MockMempool mempool_alice, mempool_bob;

    TxRelayManager relay_alice(&logger_alice);
    TxRelayManager relay_bob(&logger_bob);

    relay_alice.SetSendMessageCallback([&p2p_alice](auto addr, auto cmd, auto payload) {
        p2p_alice.SendMessage(addr, cmd, payload);
    });
    relay_bob.SetSendMessageCallback([&p2p_bob](auto addr, auto cmd, auto payload) {
        p2p_bob.SendMessage(addr, cmd, payload);
    });
    relay_alice.SetRetrieveTxCallback([&mempool_alice](auto txid, auto& out_tx) {
        return mempool_alice.GetTransaction(txid, out_tx);
    });
    relay_bob.SetValidateTxCallback([&mempool_bob](auto tx, auto peer) {
        return mempool_bob.AddTransaction(tx);
    });

    // Create INVALID transaction (version = 0)
    Transaction invalid_tx;
    invalid_tx.version = 0;  // Invalid!
    invalid_tx.lockTime = 0;
    invalid_tx.witness_version = 0;

    TxInput in;
    in.prevout.txid = TxId(uint256::FromHexUnsafe("3333333333333333333333333333333333333333333333333333333333333333"));
    in.prevout.vout = 0;
    in.scriptSig = {0x0d, 0x0e, 0x0f};
    invalid_tx.vin.push_back(in);

    TxOutput out;
    // Phase M.6.3: Use AmountUna wrapper for type safety
    out.value = AmountUna::Una(10 * 100000000ULL);
    out.scriptPubKey = {0x10, 0x11, 0x12};
    invalid_tx.vout.push_back(out);

    uint256 txid = invalid_tx.GetTxid().AsUint256();

    std::cout << "Invalid transaction ID: " << txid.GetHex().substr(0, 16) << "...\n" << std::endl;

    // Step 1: Alice adds invalid transaction (for testing purposes)
    mempool_alice.transactions[txid] = invalid_tx;  // Bypass validation
    relay_alice.AnnounceTx(txid);

    // Step 2: Bob requests transaction
    relay_bob.HandleInv("alice:8333", txid);
    relay_alice.HandleGetData("bob:8333", txid);

    // Step 3: Bob receives and REJECTS transaction
    relay_bob.HandleTx("alice:8333", invalid_tx);

    // Verify: Bob did NOT accept transaction
    Transaction temp_tx;
    assert(mempool_bob.GetTransaction(txid, temp_tx) == false);
    std::cout << "✅ Bob rejected invalid transaction\n" << std::endl;

    // Verify: Bob did NOT relay transaction
    assert(p2p_bob.has_command("inv") == false);
    std::cout << "✅ Bob did not relay invalid transaction\n" << std::endl;

    std::cout << "✅ Test G.3.3 PASSED: Invalid transaction rejection successful\n" << std::endl;
}

// ============================================================================
// Test G.3.4: Duplicate Prevention
// ============================================================================

void test_g3_4_duplicate_prevention() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test G.3.4: Duplicate Prevention" << std::endl;
    std::cout << "========================================\n" << std::endl;

    MockLogger logger_alice;
    MockP2PMessageSink p2p_alice;
    MockMempool mempool_alice;

    TxRelayManager relay_alice(&logger_alice);

    relay_alice.SetSendMessageCallback([&p2p_alice](auto addr, auto cmd, auto payload) {
        p2p_alice.SendMessage(addr, cmd, payload);
    });
    relay_alice.SetValidateTxCallback([&mempool_alice](auto tx, auto peer) {
        return mempool_alice.AddTransaction(tx);
    });

    // Create transaction
    Transaction tx;
    tx.version = 2;
    tx.lockTime = 0;
    tx.witness_version = 0;

    TxInput in;
    in.prevout.txid = TxId(uint256::FromHexUnsafe("4444444444444444444444444444444444444444444444444444444444444444"));
    in.prevout.vout = 0;
    in.scriptSig = {0x13, 0x14, 0x15};
    tx.vin.push_back(in);

    TxOutput out;
    // Phase M.6.3: Use AmountUna wrapper for type safety
    out.value = AmountUna::Una(15 * 100000000ULL);
    out.scriptPubKey = {0x16, 0x17, 0x18};
    tx.vout.push_back(out);

    uint256 txid = tx.GetTxid().AsUint256();

    std::cout << "Transaction ID: " << txid.GetHex().substr(0, 16) << "...\n" << std::endl;

    // Step 1: Alice processes transaction first time
    relay_alice.HandleTx("peer1:8333", tx);
    assert(mempool_alice.GetTransaction(txid, tx) == true);
    std::cout << "✅ First transaction accepted\n" << std::endl;

    // Step 2: Alice receives same transaction again
    size_t mempool_size_before = mempool_alice.transactions.size();
    relay_alice.HandleTx("peer2:8333", tx);
    size_t mempool_size_after = mempool_alice.transactions.size();

    // Verify: Duplicate was ignored
    assert(mempool_size_before == mempool_size_after);
    std::cout << "✅ Duplicate transaction ignored\n" << std::endl;

    // Verify: seen_txs set tracked it
    assert(relay_alice.IsTxSeen(txid) == true);
    std::cout << "✅ Transaction marked as seen\n" << std::endl;

    std::cout << "✅ Test G.3.4 PASSED: Duplicate prevention successful\n" << std::endl;
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "\n╔════════════════════════════════════════╗" << std::endl;
    std::cout << "║  Phase G.3: Mempool Relay Tests      ║" << std::endl;
    std::cout << "╚════════════════════════════════════════╝\n" << std::endl;

    try {
        test_g3_1_single_tx_propagation();
        test_g3_2_multi_peer_relay();
        test_g3_3_invalid_tx_rejection();
        test_g3_4_duplicate_prevention();

        std::cout << "\n╔════════════════════════════════════════╗" << std::endl;
        std::cout << "║  ✅ ALL TESTS PASSED                  ║" << std::endl;
        std::cout << "╚════════════════════════════════════════╝\n" << std::endl;

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ TEST FAILED: " << e.what() << std::endl;
        return 1;
    }
}
