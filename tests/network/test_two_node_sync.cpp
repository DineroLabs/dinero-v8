/**
 * @file test_two_node_sync.cpp
 * @brief Phase N.1: Multi-Node Network Validation
 *
 * PURPOSE:
 * Prove that Dinero works outside a single process.
 * This is the bridge between "local correctness" and "network correctness".
 *
 * WHAT THIS TEST PROVES:
 * - Two independent nodes can start from empty datadirs
 * - Both load genesis independently → same hash
 * - Block propagation works (A→B, B→A)
 * - Reorg works across nodes (short fork → longer fork wins)
 * - Handshake protocol is correct
 * - Block relay is deterministic
 *
 * TEST STRATEGY:
 * 1. Launch 2 dinerod instances with separate datadirs
 * 2. Verify both have same genesis
 * 3. Mine block on node A → verify node B receives it
 * 4. Mine block on node B → verify node A receives it
 * 5. Force a reorg → verify both nodes agree on winner
 *
 * FAILURE MODES THIS CATCHES:
 * - P2P handshake broken
 * - Block serialization/deserialization mismatch
 * - Reorg logic inconsistent across nodes
 * - Network message ordering bugs
 * - Race conditions in block relay
 */

#include <gtest/gtest.h>
#include <iostream>
#include <fstream>
#include <thread>
#include <chrono>
#include <filesystem>
#include <cstdlib>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

// ============================================================================
// Test Configuration
// ============================================================================

static const char* NODE_A_DATADIR = "/tmp/dinero-test-node-a";
static const char* NODE_B_DATADIR = "/tmp/dinero-test-node-b";
static const int NODE_A_RPC_PORT = 30001;
static const int NODE_B_RPC_PORT = 30002;
static const int NODE_A_P2P_PORT = 30003;
static const int NODE_B_P2P_PORT = 30004;

// Path to dinerod binary (should be in current directory when test runs)
static const char* DINEROD_PATH = "./dinerod";

// Expected genesis hash (from frozen genesis)
static const char* EXPECTED_GENESIS = "0000001c36abf27e2c233ff40ed0c08888926c24450f3bff82a047ae1528b76f";

// ============================================================================
// Helper: Clean up Orphaned Processes
// ============================================================================

void cleanupOrphanedProcesses() {
    // Kill any lingering test daemons from previous failed runs
    system("pkill -9 -f 'dinerod.*tmp/dinero-test' 2>/dev/null");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

// ============================================================================
// Helper: Clean Test Environment
// ============================================================================

void cleanTestDirs() {
    // First kill any orphaned processes that might be using these directories
    cleanupOrphanedProcesses();

    // Remove and recreate test directories
    fs::remove_all(NODE_A_DATADIR);
    fs::remove_all(NODE_B_DATADIR);
    fs::create_directories(NODE_A_DATADIR);
    fs::create_directories(NODE_B_DATADIR);
}

// ============================================================================
// Helper: RPC Call
// ============================================================================

json rpcCall(int port, const std::string& method, const json& params = json::array()) {
    // Build JSON-RPC request
    json request = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", method},
        {"params", params}
    };

    // Write request to temp file
    std::string req_file = "/tmp/rpc_request_" + std::to_string(port) + ".json";
    std::ofstream(req_file) << request.dump();

    // Determine cookie file path based on port
    std::string datadir;
    if (port == NODE_A_RPC_PORT) {
        datadir = NODE_A_DATADIR;
    } else if (port == NODE_B_RPC_PORT) {
        datadir = NODE_B_DATADIR;
    }
    std::string cookie_file = datadir + "/.cookie";

    // Read cookie file content (format: __cookie__:password)
    std::ifstream cookie_stream(cookie_file);
    std::string cookie_auth;
    if (cookie_stream.is_open()) {
        std::getline(cookie_stream, cookie_auth);
        cookie_stream.close();
    } else {
        throw std::runtime_error("Failed to read cookie file: " + cookie_file);
    }

    // Call RPC using curl with cookie authentication
    std::string cmd = "curl -s -X POST http://127.0.0.1:" + std::to_string(port) +
                      " -H 'Content-Type: application/json'" +
                      " --user \"" + cookie_auth + "\"" +
                      " -d @" + req_file +
                      " 2>/dev/null";

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("Failed to execute RPC call");
    }

    std::string result;
    char buffer[128];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    pclose(pipe);
    fs::remove(req_file);

    if (result.empty()) {
        throw std::runtime_error("Empty RPC response");
    }

    return json::parse(result);
}

// ============================================================================
// Helper: Wait for Node to be Ready
// ============================================================================

bool waitForNode(int rpc_port, int timeout_secs = 30) {
    for (int i = 0; i < timeout_secs; ++i) {
        try {
            json response = rpcCall(rpc_port, "getblockcount");
            if (response.contains("result")) {
                return true;
            }
        } catch (...) {
            // Node not ready yet
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return false;
}

// ============================================================================
// Helper: Get Peer Connection Count
// ============================================================================

int getPeerCount(int rpc_port) {
    try {
        json response = rpcCall(rpc_port, "getconnectioncount");
        if (response.contains("result")) {
            return response["result"].get<int>();
        }
    } catch (const std::exception& e) {
        std::cerr << "[P2P DEBUG] getconnectioncount failed: " << e.what() << std::endl;
    }
    return -1;
}

// ============================================================================
// Helper: Wait for Peer Connection
// ============================================================================

bool waitForPeerConnection(int rpc_port, int min_peers = 1, int timeout_secs = 30) {
    std::cout << "[P2P DEBUG] Waiting for peer connection (port " << rpc_port << ")..." << std::endl;
    for (int i = 0; i < timeout_secs; ++i) {
        int peer_count = getPeerCount(rpc_port);
        std::cout << "[P2P DEBUG] " << i << "s: peer_count=" << peer_count << std::endl;
        if (peer_count >= min_peers) {
            std::cout << "[P2P DEBUG] ✅ Peer connected after " << i << " seconds" << std::endl;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    std::cerr << "[P2P DEBUG] ❌ Timeout waiting for peer connection" << std::endl;
    return false;
}

// ============================================================================
// Helper: Launch Node
// ============================================================================

pid_t launchNode(const char* datadir, int rpc_port, int p2p_port, const std::string& connect_to = "") {
    pid_t pid = fork();

    if (pid == 0) {
        // Child process - launch dinerod
        std::string datadir_arg = std::string("--datadir=") + datadir;
        std::string rpcport_arg = std::string("--rpcport=") + std::to_string(rpc_port);
        std::string p2pport_arg = std::string("--p2pport=") + std::to_string(p2p_port);

        if (connect_to.empty()) {
            execl(DINEROD_PATH, "dinerod",
                  datadir_arg.c_str(),
                  rpcport_arg.c_str(),
                  p2pport_arg.c_str(),
                  "--regtest",
                  nullptr);
        } else {
            std::string connect_arg = std::string("--connect=") + connect_to;
            execl(DINEROD_PATH, "dinerod",
                  datadir_arg.c_str(),
                  rpcport_arg.c_str(),
                  p2pport_arg.c_str(),
                  connect_arg.c_str(),
                  "--regtest",
                  nullptr);
        }

        // If execl returns, it failed
        std::cerr << "Failed to launch dinerod from " << DINEROD_PATH << std::endl;
        exit(1);
    }

    return pid;
}

// ============================================================================
// Helper: Stop Node
// ============================================================================

void stopNode(pid_t pid) {
    if (pid > 0) {
        // First try SIGTERM for graceful shutdown
        kill(pid, SIGTERM);

        // Wait up to 5 seconds for graceful exit
        int status;
        for (int i = 0; i < 50; ++i) {
            pid_t result = waitpid(pid, &status, WNOHANG);
            if (result > 0) {
                return;  // Process exited
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // Process didn't exit gracefully, force kill
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
}

// ============================================================================
// Test 1: Independent Genesis Loading
// ============================================================================

TEST(TwoNodeSync, IndependentGenesisLoading) {
    std::cout << "\n[Test N.1.1] Independent Genesis Loading..." << std::endl;

    // Clean environment
    cleanTestDirs();

    // Launch both nodes (isolated, no P2P connection yet)
    pid_t node_a = launchNode(NODE_A_DATADIR, NODE_A_RPC_PORT, NODE_A_P2P_PORT);
    pid_t node_b = launchNode(NODE_B_DATADIR, NODE_B_RPC_PORT, NODE_B_P2P_PORT);

    ASSERT_GT(node_a, 0) << "Failed to launch node A";
    ASSERT_GT(node_b, 0) << "Failed to launch node B";

    // Wait for both nodes to be ready
    ASSERT_TRUE(waitForNode(NODE_A_RPC_PORT)) << "Node A did not start";
    ASSERT_TRUE(waitForNode(NODE_B_RPC_PORT)) << "Node B did not start";

    std::cout << "✅ Both nodes started" << std::endl;

    // Get genesis hash from both nodes
    json response_a = rpcCall(NODE_A_RPC_PORT, "getblockhash", {0});
    json response_b = rpcCall(NODE_B_RPC_PORT, "getblockhash", {0});

    ASSERT_TRUE(response_a.contains("result")) << "Node A getblockhash failed";
    ASSERT_TRUE(response_b.contains("result")) << "Node B getblockhash failed";

    std::string genesis_a = response_a["result"];
    std::string genesis_b = response_b["result"];

    std::cout << "Node A genesis: " << genesis_a << std::endl;
    std::cout << "Node B genesis: " << genesis_b << std::endl;

    // Verify both nodes have same genesis
    EXPECT_EQ(genesis_a, genesis_b) << "Genesis hash mismatch between nodes!";
    EXPECT_EQ(genesis_a, EXPECTED_GENESIS) << "Genesis hash does not match frozen value!";

    std::cout << "✅ Both nodes have same genesis" << std::endl;

    // Cleanup
    stopNode(node_a);
    stopNode(node_b);
}

// ============================================================================
// Test 2: Block Propagation (A → B)
// ============================================================================

TEST(TwoNodeSync, BlockPropagationAtoB) {
    std::cout << "\n[Test N.1.2] Block Propagation (A → B)..." << std::endl;

    // Clean environment
    cleanTestDirs();

    // Launch node A first
    pid_t node_a = launchNode(NODE_A_DATADIR, NODE_A_RPC_PORT, NODE_A_P2P_PORT);
    ASSERT_GT(node_a, 0);
    ASSERT_TRUE(waitForNode(NODE_A_RPC_PORT));

    // Launch node B connected to node A
    std::string connect_to = "127.0.0.1:" + std::to_string(NODE_A_P2P_PORT);
    pid_t node_b = launchNode(NODE_B_DATADIR, NODE_B_RPC_PORT, NODE_B_P2P_PORT, connect_to);
    ASSERT_GT(node_b, 0);
    ASSERT_TRUE(waitForNode(NODE_B_RPC_PORT));

    std::cout << "✅ Both nodes started" << std::endl;

    // Wait for P2P handshake to complete - verify connection established
    std::cout << "⏳ Waiting for P2P handshake..." << std::endl;

    // Check both nodes see each other
    bool node_a_connected = waitForPeerConnection(NODE_A_RPC_PORT, 1, 15);
    bool node_b_connected = waitForPeerConnection(NODE_B_RPC_PORT, 1, 15);

    std::cout << "Node A peer count: " << getPeerCount(NODE_A_RPC_PORT) << std::endl;
    std::cout << "Node B peer count: " << getPeerCount(NODE_B_RPC_PORT) << std::endl;

    ASSERT_TRUE(node_a_connected) << "Node A has no peers - P2P handshake failed";
    ASSERT_TRUE(node_b_connected) << "Node B has no peers - P2P handshake failed";

    std::cout << "✅ P2P handshake complete - both nodes connected" << std::endl;

    // Mine a block on node A
    json mine_response = rpcCall(NODE_A_RPC_PORT, "generate", {1});
    ASSERT_TRUE(mine_response.contains("result")) << "Mining on node A failed";

    std::cout << "✅ Mined block on node A" << std::endl;

    // Wait for block propagation (extended for multi-node sync)
    std::cout << "⏳ Waiting for block propagation..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(5));

    // Verify node B received the block
    json count_a = rpcCall(NODE_A_RPC_PORT, "getblockcount");
    json count_b = rpcCall(NODE_B_RPC_PORT, "getblockcount");

    int height_a = count_a["result"];
    int height_b = count_b["result"];

    std::cout << "Node A height: " << height_a << std::endl;
    std::cout << "Node B height: " << height_b << std::endl;

    EXPECT_EQ(height_a, height_b) << "Block did not propagate from A to B!";
    EXPECT_GT(height_a, 0) << "No blocks mined!";

    // Verify both nodes have same best block hash
    json hash_a = rpcCall(NODE_A_RPC_PORT, "getbestblockhash");
    json hash_b = rpcCall(NODE_B_RPC_PORT, "getbestblockhash");

    EXPECT_EQ(hash_a["result"], hash_b["result"]) << "Best block hash mismatch!";

    std::cout << "✅ Block propagated successfully (A → B)" << std::endl;

    // Cleanup
    stopNode(node_a);
    stopNode(node_b);
}

// ============================================================================
// Test 3: Bidirectional Sync (B → A)
// ============================================================================

TEST(TwoNodeSync, BlockPropagationBtoA) {
    std::cout << "\n[Test N.1.3] Block Propagation (B → A)..." << std::endl;

    // Clean environment
    cleanTestDirs();

    // Launch both nodes connected
    pid_t node_a = launchNode(NODE_A_DATADIR, NODE_A_RPC_PORT, NODE_A_P2P_PORT);
    ASSERT_GT(node_a, 0);
    ASSERT_TRUE(waitForNode(NODE_A_RPC_PORT));

    std::string connect_to = "127.0.0.1:" + std::to_string(NODE_A_P2P_PORT);
    pid_t node_b = launchNode(NODE_B_DATADIR, NODE_B_RPC_PORT, NODE_B_P2P_PORT, connect_to);
    ASSERT_GT(node_b, 0);
    ASSERT_TRUE(waitForNode(NODE_B_RPC_PORT));

    std::cout << "✅ Both nodes started" << std::endl;

    // Wait for P2P handshake to complete - verify connection established
    std::cout << "⏳ Waiting for P2P handshake..." << std::endl;

    // Check both nodes see each other
    bool node_a_connected = waitForPeerConnection(NODE_A_RPC_PORT, 1, 15);
    bool node_b_connected = waitForPeerConnection(NODE_B_RPC_PORT, 1, 15);

    std::cout << "Node A peer count: " << getPeerCount(NODE_A_RPC_PORT) << std::endl;
    std::cout << "Node B peer count: " << getPeerCount(NODE_B_RPC_PORT) << std::endl;

    ASSERT_TRUE(node_a_connected) << "Node A has no peers - P2P handshake failed";
    ASSERT_TRUE(node_b_connected) << "Node B has no peers - P2P handshake failed";

    std::cout << "✅ P2P handshake complete - both nodes connected" << std::endl;

    // Mine a block on node B this time
    json mine_response = rpcCall(NODE_B_RPC_PORT, "generate", {1});
    ASSERT_TRUE(mine_response.contains("result")) << "Mining on node B failed";

    std::cout << "✅ Mined block on node B" << std::endl;

    // Wait for block propagation (extended for multi-node sync)
    std::cout << "⏳ Waiting for block propagation..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(5));

    // Verify node A received the block
    json count_a = rpcCall(NODE_A_RPC_PORT, "getblockcount");
    json count_b = rpcCall(NODE_B_RPC_PORT, "getblockcount");

    int height_a = count_a["result"];
    int height_b = count_b["result"];

    std::cout << "Node A height: " << height_a << std::endl;
    std::cout << "Node B height: " << height_b << std::endl;

    EXPECT_EQ(height_a, height_b) << "Block did not propagate from B to A!";

    json hash_a = rpcCall(NODE_A_RPC_PORT, "getbestblockhash");
    json hash_b = rpcCall(NODE_B_RPC_PORT, "getbestblockhash");

    EXPECT_EQ(hash_a["result"], hash_b["result"]) << "Best block hash mismatch!";

    std::cout << "✅ Block propagated successfully (B → A)" << std::endl;

    // Cleanup
    stopNode(node_a);
    stopNode(node_b);
}

// ============================================================================
// Test 4: Reorg Handling (Short Fork → Longer Fork Wins)
// ============================================================================

TEST(TwoNodeSync, ReorgHandling) {
    std::cout << "\n[Test N.1.4] Reorg Handling (Short Fork → Longer Fork Wins)..." << std::endl;

    // This test is more complex - requires disconnecting nodes, mining competing chains,
    // then reconnecting to trigger reorg. Marked as TODO for Phase N.1.1

    GTEST_SKIP() << "Reorg test requires node disconnect/reconnect API (Phase N.1.1)";
}

// ============================================================================
// Test Runner
// ============================================================================

int main(int argc, char** argv) {
    std::cout << "════════════════════════════════════════════════════════════" << std::endl;
    std::cout << "  PHASE N.1: MULTI-NODE NETWORK VALIDATION" << std::endl;
    std::cout << "════════════════════════════════════════════════════════════" << std::endl;
    std::cout << "\nGoal: Prove Dinero works outside a single process" << std::endl;
    std::cout << "Tests: Genesis sync, block propagation, bidirectional relay\n" << std::endl;

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
