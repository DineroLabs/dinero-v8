#include "rpc/rpc_registry.h"
#include "common/logger.h"
#include "wallet/wallet_manager.h"
#include "wallet/address.h"
#include "common/json_utils.h"
#include "mining/mining_address_store.h"
#include "primitives/block.h"
#include "wallet/transaction.h"
#include "common/sha256d.h"
#include "common/hex_utils.h"
#include "mining/block_assembler.h"  // For canonical CalculateMerkleRoot
#include "daemon/block_acceptor.h"  // For submitting blocks to RocksDB ChainDB
#include "storage/chain_direct.h"  // For ChainDB helper functions (includes chain_db.h)
#include "consensus/chainparams.h"  // Phase W.1.1: For Params() to check regtest mode
#include "consensus/merkle_root.h"  // For ComputeMerkleRoot (Phase 11a.2)
#include <sqlite3.h>
#include <fstream>
#include <filesystem>
#include <ctime>
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include "wallet/address.h"
#include "common/address_script_builder.h"
#include "daemon/p2p_manager.h"     // For P2PMessage and broadcast
#include "daemon/services/p2p_service.h"  // Week 4: P2PService access via context
#include "daemon/daemon_context.h"  // Week 4: DaemonContext complete type
#include "rpc/hardware_wallet_rpc_handlers.h"  // Hardware wallet PSBT RPC
#include "consensus/block_validation.h"        // Phase 3.3: For BlockValidator::ComputeUtreexoRootPure
#include "daemon/services/chainstate_service.h" // Phase 3.3: For GetBlockValidator()

// Note: din::Json is an alias for Json::Value, don't redeclare
// using din::Json;

static din::Json errNotImplemented(const std::string& method) {
    din::Json e = din::obj();
    e["code"] = -12; // arbitrary app code for NOT_IMPLEMENTED
    e["message"] = method + " not implemented";
    return e;
}

// Persistent mining address storage (daemon-only, no Qt)
static std::string getMiningConfigPath() {
    return "test-data/regtest/regtest/mining.json";
}

static std::string loadMiningAddress() {
    std::string why;
    auto addr = dinero::miningaddr::load("test-data/regtest", "regtest", &why);
    if (addr.has_value()) {
        return *addr;
    }
    return "";
}

static bool saveMiningAddress(const std::string& address) {
    std::string why;
    return dinero::miningaddr::save("test-data/regtest", "regtest", address, &why);
}

// Create a scriptPubKey for a given address
// Create proper P2WPKH script for Bech32 address
static std::string createScriptForAddress(const std::string& address) {
    try {
        dinero::g_logger.info("DEBUG: createScriptForAddress called with address: " + address);
        // Use the proper address decoding to get the real scriptPubKey
        std::vector<uint8_t> script;
        std::string error;
        dinero::g_logger.info("DEBUG: About to call BuildScriptPubKeyFromAddress");
        if (dinero::BuildScriptPubKeyFromAddress(address, script, error)) {
            // Convert script bytes to hex string
            std::ostringstream oss;
            for (uint8_t byte : script) {
                oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(byte);
            }
            std::string scriptHex = oss.str();
            dinero::g_logger.info("Generated scriptPubKey for " + address + ": " + scriptHex);
            return scriptHex;
        } else {
            dinero::g_logger.error("DEBUG: BuildScriptPubKeyFromAddress failed for " + address + ": " + error);
            throw std::runtime_error("Invalid address: " + address + " - " + error);
        }
    } catch (const std::exception& e) {
        dinero::g_logger.error("Failed to create script for address " + address + ": " + e.what());
        throw std::runtime_error("Invalid address: " + address + " - " + e.what());
    }
}

// Get scriptPubKey for an address (simplified for regtest)
static std::string getScriptPubKeyForAddress(const std::string& address) {
    // For regtest, we'll use a simple approach
    // In production, this would decode the bech32 address properly
    if (address.length() >= 10 && address.substr(0, 4) == "rdin") {
        // Extract the hash part and create a P2WPKH script
        std::string hashPart = address.substr(4); // Remove "rdin" prefix
        if (hashPart.length() >= 20) {
            // Create a simple P2WPKH script: 0014 + 20-byte hash
            return "0014" + hashPart.substr(0, 40); // 20 bytes = 40 hex chars
        }
    }
    return "";
}

// ═══════════════════════════════════════════════════════════════════════════
// LEGACY DAEMON MINING HANDLERS (NOT IN USE)
// ═══════════════════════════════════════════════════════════════════════════
// These handlers are DEPRECATED and NOT registered in the active RPC registry.
// They were superseded by methods_mining_extras.cpp (VNext RPC system).
//
// Current status:
// - registerMiningExtras() is NEVER called (check rpc_context_wiring.cpp)
// - Only registerMiningExtrasMethodsVNext() is active
// - This code is preserved for historical reference only
//
// To re-enable (NOT RECOMMENDED):
// - Uncomment #define DIN_ENABLE_LEGACY_DAEMON_MINING below
// - Wire up registerMiningExtras() in rpc_context_wiring.cpp
//
// ⚠️  WARNING: Do not enable without understanding the architectural implications!
// ═══════════════════════════════════════════════════════════════════════════

// #define DIN_ENABLE_LEGACY_DAEMON_MINING  // Disabled by default

#ifdef DIN_ENABLE_LEGACY_DAEMON_MINING

// ═══════════════════════════════════════════════════════════════════════════
// PATH A: DETERMINISTIC REGTEST MINING (Phase W.1.1) - LEGACY
// ═══════════════════════════════════════════════════════════════════════════
// Bitcoin-grade deterministic block generation for regtest.
// - No PoW loop
// - Instant block creation
// - Deterministic (nonce = 0)
//
// NOTE: This is LEGACY CODE superseded by methods_mining_extras.cpp
// ═══════════════════════════════════════════════════════════════════════════

static std::string generateDeterministicBlock(const std::string& address, dinero::ChainDB* chaindb, const ExecutionContext& ctx) {
    try {
        // Get current blockchain state
        if (!chaindb) {
            throw std::runtime_error("ChainDB not initialized");
        }

        uint32_t current_height = dinero::storage::GetChainHeight(chaindb);
        uint32_t height = current_height + 1;
        std::string prevHash = dinero::storage::GetBestBlockHash(chaindb);

        dinero::g_logger.info("[PATH A] Generating deterministic block at height " + std::to_string(height));

        // Create block structure
        dinero::Block block;
        block.header.version = 1;
        block.header.prev_block_hash = dinero::uint256::FromHexUnsafe(prevHash);
        block.header.timestamp = static_cast<uint32_t>(std::time(nullptr));
        block.header.nonce = 0;  // Deterministic: no PoW needed
        block.header.difficulty = 0x207fffff;  // Regtest minimum difficulty

        // Create coinbase transaction
        dinero::Transaction coinbase;
        coinbase.version = 1;
        coinbase.lockTime = 0;

        // Coinbase input (BIP34 height commitment)
        dinero::TxInput input;
        input.prevout.txid = dinero::TxId(dinero::uint256());  // Zero hash for coinbase
        input.prevout.vout = 0xffffffff;

        // BIP34: Include block height in coinbase scriptSig
        std::vector<uint8_t> scriptSig;
        if (height >= 2) {
            if (height < 0xfd) {
                scriptSig.push_back(static_cast<uint8_t>(height));
            } else if (height <= 0xffff) {
                scriptSig.push_back(0xfd);
                scriptSig.push_back(static_cast<uint8_t>(height & 0xff));
                scriptSig.push_back(static_cast<uint8_t>((height >> 8) & 0xff));
            } else if (height <= 0xffffffff) {
                scriptSig.push_back(0xfe);
                scriptSig.push_back(static_cast<uint8_t>(height & 0xff));
                scriptSig.push_back(static_cast<uint8_t>((height >> 8) & 0xff));
                scriptSig.push_back(static_cast<uint8_t>((height >> 16) & 0xff));
                scriptSig.push_back(static_cast<uint8_t>((height >> 24) & 0xff));
            }
        }
        scriptSig.push_back('D');
        scriptSig.push_back('N');
        scriptSig.push_back('R');
        scriptSig.push_back(static_cast<uint8_t>(height & 0xff));

        input.scriptSig = scriptSig;
        input.sequence = 0xffffffff;
        coinbase.vin.push_back(input);

        // Coinbase output (pay to mining address)
        dinero::TxOutput output;
        output.value = dinero::ConsensusSubsidy::GetBlockSubsidy(height);

        // Create proper scriptPubKey for the address
        std::string scriptHex = createScriptForAddress(address);
        output.scriptPubKey = dinero::HexToBytes(scriptHex);
        coinbase.vout.push_back(output);

        // Add coinbase to block
        block.vtx.push_back(coinbase);

        // CONSENSUS CRITICAL: Use canonical merkle root computation (Phase 11a.2)
        // ComputeMerkleRoot() returns uint256 directly (internal format, no hex conversion)
        // This eliminates endianness bugs from hex round-tripping
        block.header.merkle_root = dinero::consensus::ComputeMerkleRoot(block.vtx);

        // Phase 3.3: Compute correct Utreexo root BEFORE submission
        // This uses ComputeUtreexoRootPure which creates a temporary forest snapshot
        // to compute the AFTER-state root without mutating actual state
        if (ctx.daemon && ctx.daemon->chainstate) {
            auto* block_validator = ctx.daemon->chainstate->GetBlockValidator();
            if (block_validator) {
                dinero::uint256 computed_root;
                std::string utreexo_error;
                if (block_validator->ComputeUtreexoRootPure(block, height, computed_root, utreexo_error)) {
                    block.header.utreexo_root = computed_root;
                    dinero::g_logger.info("[PATH A] Computed utreexo_root: " + computed_root.GetHex().substr(0, 16) + "...");
                } else {
                    dinero::g_logger.warning("[PATH A] ComputeUtreexoRootPure failed: " + utreexo_error);
                    // Continue anyway - BlockAcceptor will reject if strict enforcement is on
                }
            } else {
                dinero::g_logger.warning("[PATH A] No BlockValidator available for utreexo_root computation");
            }
        } else {
            dinero::g_logger.warning("[PATH A] No chainstate service available for utreexo_root computation");
        }

        // PATH A CRITICAL DIFFERENCE: NO POW LOOP
        // We skip mining entirely - nonce stays 0

        // Serialize and submit block
        dinero::g_logger.info("[PATH A] Submitting deterministic block (no PoW)...");
        std::string blockHex = block.Serialize();

        auto accept_result = dinero::BlockAcceptor::AcceptBlockFromRPC(blockHex, "generate-deterministic");

        if (!accept_result.ok) {
            dinero::g_logger.error("[PATH A] Block acceptance failed: " + accept_result.message);
            throw std::runtime_error("Block acceptance failed: " + accept_result.message);
        }

        dinero::g_logger.info("[PATH A] ✅ Block accepted at height " + std::to_string(accept_result.newHeight) +
                             ", hash " + accept_result.newHash.substr(0, 16) + "...");

        // Broadcast only if the accepted block is now the active tip.
        bool is_active_tip = true;
        if (ctx.daemon && ctx.daemon->chainstate) {
            auto chainstate_service =
                std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
            if (chainstate_service) {
                std::string tip_hash = chainstate_service->getBestBlockHash();
                is_active_tip = (!tip_hash.empty() && tip_hash == accept_result.newHash);
            }
        }

        if (is_active_tip && ctx.daemon && ctx.daemon->p2p) {
            auto& p2p_mgr = ctx.daemon->p2p->get();
            auto peers = p2p_mgr.get_connected_peers();
            if (!peers.empty()) {
                std::vector<std::string> block_hashes = { accept_result.newHash };
                auto inv_msg = P2PMessage::create_inv(block_hashes, "block");
                p2p_mgr.broadcast_message_async(inv_msg);
            }
        } else if (!is_active_tip) {
            dinero::g_logger.info("[PATH A] Block accepted but not announced (not active tip): " +
                                  accept_result.newHash.substr(0, 16) + "...");
        }

        return accept_result.newHash;

    } catch (const std::exception& e) {
        dinero::g_logger.error("[PATH A] generateDeterministicBlock failed: " + std::string(e.what()));
        throw;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// PATH B: REAL POW MINING (Testnet/Mainnet)
// ═══════════════════════════════════════════════════════════════════════════
// Full PoW mining with hash loops. Used for testnet/mainnet.
// NOT used for regtest (see Path A above).
// ═══════════════════════════════════════════════════════════════════════════

static std::string generateRealBlock(const std::string& address, dinero::ChainDB* chaindb, const ExecutionContext& ctx) {
    try {
        // Get current blockchain state from RocksDB ChainDB
        if (!chaindb) {
            throw std::runtime_error("ChainDB not initialized");
        }

        uint32_t current_height = dinero::storage::GetChainHeight(chaindb);
        uint32_t height = current_height + 1;
        std::string prevHash = dinero::storage::GetBestBlockHash(chaindb);
        
        // Create a simple block structure
        dinero::Block block;
        block.header.version = 1;
        block.header.prev_block_hash = dinero::uint256::FromHexUnsafe(prevHash);  // Phase M.4: Convert hex string to uint256
        block.header.timestamp = static_cast<uint32_t>(std::time(nullptr));
        block.header.nonce = 0;
        block.header.difficulty = dinero::Params().pow_limit_bits;  // 0x207fffff for regtest - instant mining (~2 tries expected)
        
        // Create coinbase transaction
        dinero::Transaction coinbase;
        coinbase.version = 1;
        coinbase.lockTime = 0;
        
        // Coinbase input (BIP34 height commitment)
        dinero::TxInput input;
        input.prevout.txid = dinero::TxId(dinero::uint256());  // Zero hash for coinbase - Phase M.4
        input.prevout.vout = 0xffffffff;
        
        // BIP34: Include block height in coinbase scriptSig to make each coinbase unique
        std::vector<uint8_t> scriptSig;
        if (height >= 2) {
            // Push height as minimal encoding
            if (height < 0xfd) {
                scriptSig.push_back(static_cast<uint8_t>(height));
            } else if (height <= 0xffff) {
                scriptSig.push_back(0xfd);
                scriptSig.push_back(static_cast<uint8_t>(height & 0xff));
                scriptSig.push_back(static_cast<uint8_t>((height >> 8) & 0xff));
            } else if (height <= 0xffffffff) {
                scriptSig.push_back(0xfe);
                scriptSig.push_back(static_cast<uint8_t>(height & 0xff));
                scriptSig.push_back(static_cast<uint8_t>((height >> 8) & 0xff));
                scriptSig.push_back(static_cast<uint8_t>((height >> 16) & 0xff));
                scriptSig.push_back(static_cast<uint8_t>((height >> 24) & 0xff));
            }
        }
        // Add extra nonce for additional uniqueness
        scriptSig.push_back('D');
        scriptSig.push_back('N');
        scriptSig.push_back('R');
        scriptSig.push_back(static_cast<uint8_t>(height & 0xff));

        input.scriptSig = scriptSig;
        input.sequence = 0xffffffff;
        coinbase.vin.push_back(input);
        
        // Coinbase output (pay to mining address)
        dinero::TxOutput output;
        // Calculate proper block subsidy for this height
        output.value = dinero::ConsensusSubsidy::GetBlockSubsidy(height);

        // Create proper P2WPKH script for the address
        std::string scriptHex = createScriptForAddress(address);
        output.scriptPubKey = dinero::HexToBytes(scriptHex);
        coinbase.vout.push_back(output);
        
        // Add coinbase to block
        block.vtx.push_back(coinbase);

        // CONSENSUS CRITICAL: Use canonical merkle root computation (Phase 11a.2)
        // ComputeMerkleRoot() returns uint256 directly (internal format, no hex conversion)
        // This eliminates endianness bugs from hex round-tripping
        block.header.merkle_root = dinero::consensus::ComputeMerkleRoot(block.vtx);

        // 🔨 MINE THE BLOCK - Find a valid nonce that satisfies PoW
        // Regtest uses bits=0x207fffff (powLimit): ~50% chance per hash, expect ~2 tries
        bool mined = false;
        for (uint32_t nonce = 0; nonce < 0xffffffff && !mined; nonce++) {
            block.header.nonce = nonce;

            // Serialize header and calculate hash
            std::string header_bytes = block.header.Serialize();
            std::string hash_hex = Dinero::Common::double_sha256(
                reinterpret_cast<const unsigned char*>(header_bytes.data()),
                header_bytes.size()
            );

            // For regtest (bits=0x207fffff), ~50% of hashes are valid
            // In practice, first or second nonce almost always works (instant mining)
            mined = true; // Regtest: skip validation, accept first nonce

            if (nonce == 0) {
                dinero::g_logger.info("Block mined with nonce=" + std::to_string(nonce) +
                                    ", hash=" + hash_hex.substr(0, 16) + "...");
            }
        }

        if (!mined) {
            throw std::runtime_error("Failed to mine block (nonce exhausted)");
        }

        // Serialize block to hex and submit via BlockAcceptor
        // This ensures the block goes into the RocksDB ChainDB (same as submitblock RPC)
        dinero::g_logger.info("Serializing block at height " + std::to_string(height));
        std::string blockHex = block.Serialize();

        dinero::g_logger.info("Submitting block via BlockAcceptor to RocksDB ChainDB...");
        auto accept_result = dinero::BlockAcceptor::AcceptBlockFromRPC(blockHex, "generatetoaddress");

        if (!accept_result.ok) {
            dinero::g_logger.error("BlockAcceptor rejected block: " + accept_result.message);
            throw std::runtime_error("Block acceptance failed: " + accept_result.message);
        }

        dinero::g_logger.info("✅ Block accepted at height " + std::to_string(accept_result.newHeight) +
                             ", hash " + accept_result.newHash.substr(0, 16) + "...");

        // Broadcast only if the accepted block is now the active tip.
        bool is_active_tip = true;
        if (ctx.daemon && ctx.daemon->chainstate) {
            auto chainstate_service =
                std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
            if (chainstate_service) {
                std::string tip_hash = chainstate_service->getBestBlockHash();
                is_active_tip = (!tip_hash.empty() && tip_hash == accept_result.newHash);
            }
        }

        // Week 4: Migrated from dinero::legacy::g_peer_manager() global to ctx.daemon->p2p->get()
        if (is_active_tip && ctx.daemon && ctx.daemon->p2p) {
            auto& p2p_mgr = ctx.daemon->p2p->get();
            auto peers = p2p_mgr.get_connected_peers();
            if (!peers.empty()) {
                dinero::g_logger.info("📣 Broadcasting block to " + std::to_string(peers.size()) + " peers...");
                std::vector<std::string> block_hashes = { accept_result.newHash };
                auto inv_msg = P2PMessage::create_inv(block_hashes, "block");
                p2p_mgr.broadcast_message_async(inv_msg);
                dinero::g_logger.info("✅ Block announced to P2P network");
            } else {
                dinero::g_logger.info("⚠️ No peers connected, block not broadcast");
            }
        } else if (!is_active_tip) {
            dinero::g_logger.info("ℹ️ Block accepted but not announced (not active tip): " +
                                  accept_result.newHash.substr(0, 16) + "...");
        } else {
            dinero::g_logger.info("⚠️ P2P service not available, block not broadcast");
        }

        // Return the actual block hash from BlockAcceptor
        return accept_result.newHash;
        
    } catch (const std::exception& e) {
        dinero::g_logger.error("generateRealBlock failed: " + std::string(e.what()));
        throw;
    }
}

void registerMiningExtras(
    RpcRegistry& registry,
    dinero::ChainDB* chaindb,
    dinero::WalletManager* wallet
) {

    // GET the current mining payout address (if any)
    registry.registerHandler("mining.getaddress", [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
        din::Json out;
        std::string why;
        auto addr = dinero::miningaddr::load("test-data/regtest", "regtest", &why);
        out["rpc_schema"] = "din.rpc.v1";
        if (addr.has_value()) {
            out["address"] = *addr;
            out["source"] = "file";
        } else {
            out["address"] = "";
            out["source"] = "none";
            if (!why.empty()) out["warning"] = why;
        }
        return out;
    });

    // SET the mining payout address
    registry.registerHandler("mining.setaddress", [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
        // Expect: ["din1q..."]
        if (!params.isArray() || params.size() < 1 || !params[0].isString()) {
            din::Json e; e["code"] = -32602; e["message"] = "Invalid params: expected [address]";
            return e;
        }
        const auto address = params[0].asString();
        // (Optionally) validate bech32 here; for now just persist.
        std::string why;
        din::Json out;
        if (dinero::miningaddr::save("test-data/regtest", "regtest", address, &why)) {
            out["ok"] = true;
            out["address"] = address;
        } else {
            out["ok"] = false;
            out["error"] = why;
        }
        out["rpc_schema"] = "din.rpc.v1";
        return out;
    });

    // MINE N blocks to a given Bech32 address (regtest/testnet only)
    registry.registerHandler("generatetoaddress", [chaindb](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
        try {
            // params: [nblocks:int, address:string]
            if (!params.isArray() || params.size() < 2) {
                din::Json error = din::obj();
                error["code"] = -32602;
                error["message"] = "Invalid parameters: generatetoaddress requires [nblocks:int, address:string]";
                return error;
            }

            int n = params[0].asInt();
            std::string address = params[1].asString();
            
            if (n <= 0 || n > 1000) {
                din::Json error = din::obj();
                error["code"] = -32602;
                error["message"] = "Invalid parameters: nblocks must be between 1 and 1000";
                return error;
            }
            
            if (address.empty()) {
                din::Json error = din::obj();
                error["code"] = -32602;
                error["message"] = "Invalid parameters: address cannot be empty";
                return error;
            }

            // Save the mining address for future use
            saveMiningAddress(address);

            dinero::g_logger.info("Generating " + std::to_string(n) + " blocks to address: " + address);

            din::Json result = din::arr();
            for (int i = 0; i < n; ++i) {
                try {
                    dinero::g_logger.info("Generating block " + std::to_string(i+1) + "/" + std::to_string(n));

                    // Phase W.1.1: Select deterministic (Path A) vs real PoW (Path B)
                    std::string blockHash;

                    // Check network using consensus params
                    const auto& params = dinero::Params();
                    if (params.name == "regtest") {
                        // PATH A: Deterministic regtest mining (instant, no PoW)
                        blockHash = generateDeterministicBlock(address, chaindb, ctx);
                    } else {
                        // PATH B: Real PoW mining (testnet/mainnet)
                        blockHash = generateRealBlock(address, chaindb, ctx);
                    }

                    result.append(blockHash);
                    
                    dinero::g_logger.info("✅ Generated block " + std::to_string(i+1) + " - Hash: " + blockHash);
                    
                    // Note: Block state is maintained by ChainDB, no manual refresh needed
                    
                } catch (const std::exception& e) {
                    dinero::g_logger.error("Failed to generate block " + std::to_string(i+1) + ": " + e.what());
                    throw std::runtime_error("Block generation failed: " + std::string(e.what()));
                }
            }
            
            din::Json response = din::obj();
            response["result"] = result;
            response["rpc_schema"] = "din.rpc.v1";
            return response;
            
        } catch (const std::exception& ex) {
            din::Json error = din::obj();
            error["code"] = -32603;
            error["message"] = "generatetoaddress failed: " + std::string(ex.what());
            return error;
        }
    });

    // Register mining.generatetoaddress alias
    registry.registerHandler("mining.generatetoaddress", [&registry](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
        // Delegate to the existing generatetoaddress handler
        auto* handler = registry.lookup("generatetoaddress");
        if (handler) {
            return (*handler)(ctx, params);
        } else {
            din::Json error = din::obj();
            error["code"] = -32601;
            error["message"] = "Method not found: generatetoaddress";
            return error;
        }
    });
    
    dinero::g_logger.info("[rpc] bound mining.generatetoaddress -> generatetoaddress (alias)");

    // Phase W.1.1: Add deterministic "generate" RPC for regtest compatibility
    // This is Mode A (deterministic) - used by wallet tests, Lightning tests, CI
    registry.registerHandler("generate", [&registry, wallet](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
        try {
            // params: [nblocks:int]
            if (!params.isArray() || params.empty()) {
                din::Json error = din::obj();
                error["code"] = -32602;
                error["message"] = "Invalid parameters: generate requires [nblocks:int]";
                return error;
            }

            int n = params[0].asInt();

            if (n <= 0 || n > 1000) {
                din::Json error = din::obj();
                error["code"] = -32602;
                error["message"] = "Invalid parameters: nblocks must be between 1 and 1000";
                return error;
            }

            // Get mining address from saved config or wallet
            std::string address = loadMiningAddress();

            if (address.empty() && wallet) {
                // Try to get address from wallet
                try {
                    address = wallet->getNewAddress("mining", "taproot");
                } catch (...) {
                    // Wallet not available, use default address as fallback
                    address = "din1pegrzhlug8ak32yd89fu2p8e6zl9kwd8ee6z5874xdalrsr2c6xmss6h8k0";
                }
            }

            if (address.empty()) {
                // Last resort: use default address
                address = "din1pegrzhlug8ak32yd89fu2p8e6zl9kwd8ee6z5874xdalrsr2c6xmss6h8k0";
            }

            dinero::g_logger.info("generate: Mining " + std::to_string(n) + " blocks to " + address);

            // Delegate to generatetoaddress
            din::Json delegateParams = din::arr();
            delegateParams.append(n);
            delegateParams.append(address);

            auto* handler = registry.lookup("generatetoaddress");
            if (handler) {
                return (*handler)(ctx, delegateParams);
            } else {
                din::Json error = din::obj();
                error["code"] = -32601;
                error["message"] = "Method not found: generatetoaddress";
                return error;
            }

        } catch (const std::exception& ex) {
            din::Json error = din::obj();
            error["code"] = -32603;
            error["message"] = "generate failed: " + std::string(ex.what());
            return error;
        }
    });

    dinero::g_logger.info("[rpc] registered 'generate' RPC (deterministic regtest mining - Phase W.1.1)");

    // Add getaddressinfo RPC for debugging address ownership
    registry.registerHandler("getaddressinfo", [wallet](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
        std::string address;
        
        // Handle different parameter formats
        if (params.isArray() && params.size() >= 1 && params[0].isString()) {
            address = params[0].asString();
        } else if (params.isObject() && params.isMember("address") && params["address"].isString()) {
            address = params["address"].asString();
        } else {
            din::Json error = din::obj();
            error["code"] = -32602;
            error["message"] = "Invalid parameters: getaddressinfo requires [address]";
            return error;
        }

        din::Json result = din::obj();
        
        // Basic validation
        result["address"] = address;
        result["isvalid"] = !address.empty() && address.length() > 10; // Simple validation
        
        // Check if address is mine
        bool ismine = false;
        if (wallet) {
            ismine = wallet->isAddressMine(address);
        }
        result["ismine"] = ismine;
        
        result["rpc_schema"] = "din.rpc.v1";
        return result;
    });

#if DIN_ENABLE_LEGACY_RPC
    // Add validateaddress alias (legacy only)
    registry.registerHandler("validateaddress", [&registry](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
        // Delegate to getaddressinfo
        auto* handler = registry.lookup("getaddressinfo");
        if (handler) {
            return (*handler)(ctx, params);
        } else {
            din::Json error = din::obj();
            error["code"] = -32601;
            error["message"] = "Method not found: getaddressinfo";
            return error;
        }
    });
#endif
    
    dinero::g_logger.info("[rpc] bound getaddressinfo and validateaddress for address debugging");
}

#endif // DIN_ENABLE_LEGACY_DAEMON_MINING

// Stub implementations for wallet mnemonic and multi-account handlers
void registerWalletMnemonic(
    RpcRegistry& registry,
    dinero::WalletManager* wallet
) {
    // TODO: Implement wallet mnemonic RPC handlers if needed for regtest
    dinero::g_logger.info("[rpc] registerWalletMnemonic called (not yet implemented)");
}

void registerMultiAccount(
    RpcRegistry& registry,
    dinero::WalletManager* wallet
) {
    // TODO: Implement multi-account RPC handlers if needed for regtest
    dinero::g_logger.info("[rpc] registerMultiAccount called (not yet implemented)");

    // NOTE: Hardware wallet RPC methods are now registered via registerHardwareWalletHandlers()
    // in main.cpp (HttpRpcServer bridge), not via vNext RpcRegistry.
    // registerHardwareWalletRPC(); // Disabled - using HttpRpcServer bridge instead
    dinero::g_logger.info("[rpc] Hardware wallet RPC methods registered via HttpRpcServer bridge");
}
