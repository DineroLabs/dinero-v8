// src/daemon/db_init_example.cpp
// Example integration of database initialization into daemon startup
// This shows how to call ensureGenesis() safely during dinerod initialization

#include <iostream>
#include <sqlite3.h>
#include <filesystem>
#include <stdexcept>

#include "daemon/db_init.hpp"

namespace fs = std::filesystem;
using namespace dinero::db;

// Example network parameters (replace with your actual consensus values)
NetworkParams getNetworkParams(const std::string& network) {
  NetworkParams p;
  p.network = network;
  
  if (network == "regtest") {
    p.version = 1;
    p.prevhash = {}; // all zeros for genesis
    p.merkle = hex32("4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b");
    p.time = 1296688602;
    p.bits = 0x207fffff;
    p.nonce = 2;
    p.genesis_hash = hex32("b10c574aa46f4e663dcae73321c91c67da738c308bc7440836b5de2f61b393cf");
    p.chainwork = hex32("0000000000000000000000000000000000000000000000000000000000000002");
  } else if (network == "testnet") {
    p.version = 1;
    p.prevhash = {};
    p.merkle = hex32("4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b");
    p.time = 1296688602;
    p.bits = 0x1d00ffff;
    p.nonce = 414098458;
    p.genesis_hash = hex32("000000000933ea01ad0ee984209779baaec3ced90fa3f408719526f8d77f4943");
    p.chainwork = hex32("0000000000000000000000000000000000000000000000000000000100010001");
  } else if (network == "mainnet") {
    p.version = 1;
    p.prevhash = {};
    p.merkle = hex32("4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b");
    p.time = 1231006505;
    p.bits = 0x1d00ffff;
    p.nonce = 2083236893;
    p.genesis_hash = hex32("000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f");
    p.chainwork = hex32("0000000000000000000000000000000000000000000000000000000100010001");
  } else {
    throw std::invalid_argument("Unknown network: " + network);
  }
  
  return p;
}

// Example daemon initialization function
bool initializeChainstate(const std::string& datadir, const std::string& network) {
  try {
    // 1) Ensure data directory exists
    fs::path chainstate_path = fs::path(datadir) / "blockchain.db";
    fs::create_directories(chainstate_path.parent_path());
    
    std::cout << "🔧 Initializing chainstate database...\n";
    std::cout << "   Network: " << network << "\n";
    std::cout << "   Database: " << chainstate_path << "\n";
    
    // 2) Open SQLite database
    sqlite3* db = nullptr;
    int rc = sqlite3_open(chainstate_path.c_str(), &db);
    if (rc != SQLITE_OK) {
      std::cerr << "❌ Failed to open chainstate database: " << sqlite3_errmsg(db) << "\n";
      return false;
    }
    
    // 3) Ensure genesis block and meta keys are initialized
    NetworkParams params = getNetworkParams(network);
    auto result = ensureGenesis(db, params);
    
    if (!result.ok) {
      std::cerr << "❌ Database initialization failed: " << result.error << "\n";
      sqlite3_close(db);
      return false;
    }
    
    if (result.wrote_genesis || result.wrote_meta) {
      std::cout << "✅ Database initialized (first run or missing data)\n";
      std::cout << "   Genesis header: " << (result.wrote_genesis ? "CREATED" : "EXISTS") << "\n";
      std::cout << "   Meta keys: " << (result.wrote_meta ? "CREATED" : "EXISTS") << "\n";
    } else {
      std::cout << "✅ Database already initialized (idempotent check passed)\n";
    }
    
    sqlite3_close(db);
    return true;
    
  } catch (const std::exception& e) {
    std::cerr << "❌ Exception during chainstate initialization: " << e.what() << "\n";
    return false;
  }
}

// Example main function showing integration
int main(int argc, char* argv[]) {
  std::string datadir = "/tmp/dinero_example";
  std::string network = "regtest";
  
  // Parse command line arguments (simplified)
  if (argc > 1) datadir = argv[1];
  if (argc > 2) network = argv[2];
  
  std::cout << "🚀 **DINERO DAEMON DATABASE INITIALIZATION EXAMPLE**\n";
  std::cout << "==================================================\n\n";
  
  // This would be called early in your daemon startup, after parsing config
  // but before starting P2P networking or RPC server
  if (!initializeChainstate(datadir, network)) {
    std::cerr << "💥 Daemon startup failed - chainstate initialization error\n";
    return 1;
  }
  
  std::cout << "\n🎊 Database initialization successful!\n";
  std::cout << "💡 Your daemon can now safely:\n";
  std::cout << "   • Start P2P networking\n";
  std::cout << "   • Begin block validation\n";
  std::cout << "   • Accept RPC connections\n";
  std::cout << "   • Process transactions\n\n";
  
  std::cout << "🔄 On subsequent startups, ensureGenesis() will:\n";
  std::cout << "   • Validate network consistency\n";
  std::cout << "   • Verify genesis hash matches\n";
  std::cout << "   • Skip initialization (idempotent)\n";
  std::cout << "   • Return instantly\n\n";
  
  return 0;
}

/*
 * INTEGRATION CHECKLIST:
 * 
 * 1. ✅ Include db_init.hpp in your daemon
 * 2. ✅ Call ensureGenesis() early in startup (after config, before networking)
 * 3. ✅ Handle initialization errors gracefully
 * 4. ✅ Use real consensus parameters (not example values)
 * 5. ✅ Run migrations before calling ensureGenesis()
 * 6. ✅ Test with idempotence unit test
 * 
 * PRODUCTION NOTES:
 * 
 * • ensureGenesis() is crash-safe (atomic transactions)
 * • Safe to call multiple times (idempotent)
 * • Validates network and genesis consistency
 * • Works with existing databases (upgrades gracefully)
 * • Minimal overhead on subsequent startups
 * • Thread-safe (but call during single-threaded startup)
 */
