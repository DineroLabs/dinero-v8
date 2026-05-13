#include "blockchain_sync.h"
#include "wallet.h"
#include "utxo_manager.h"
#include "database.h"
#include <iostream>
#include <ctime>
#include <stdexcept>

namespace dinero {
namespace wallet {
namespace reference {

BlockchainSync::BlockchainSync(
    ReferenceWallet* wallet,
    const BlockchainSyncConfig& config
)
    : wallet_(wallet)
    , config_(config)
    , stop_requested_(false) {

    if (!wallet_) {
        throw std::invalid_argument("Wallet pointer cannot be null");
    }

    // Initialize stats
    stats_.start_height = 0;
    stats_.current_height = 0;
    stats_.scanned_height = 0;
    stats_.blocks_scanned = 0;
    stats_.transactions_found = 0;
    stats_.utxos_added = 0;
    stats_.utxos_spent = 0;
    stats_.sync_start_time = 0;
    stats_.sync_end_time = 0;
    stats_.is_syncing = false;
}

BlockchainSync::~BlockchainSync() {
    Stop();
}

bool BlockchainSync::Initialize() {
    try {
        // Create RPC client
        Dinero::Common::RPCConfig rpc_config;
        rpc_config.url = config_.rpc_url;
        rpc_config.user = config_.rpc_user;
        rpc_config.pass = config_.rpc_password;
        rpc_config.timeout = config_.rpc_timeout;
        rpc_config.verbose = config_.verbose;

        rpc_client_ = std::make_unique<Dinero::Common::RPCClient>(rpc_config);

        // Test connection
        if (!IsConnected()) {
            last_error_ = "Failed to connect to blockchain node at " + config_.rpc_url;
            return false;
        }

        if (config_.verbose) {
            std::cout << "Connected to blockchain node at " << config_.rpc_url << std::endl;
        }

        return true;

    } catch (const std::exception& e) {
        last_error_ = std::string("Initialization failed: ") + e.what();
        return false;
    }
}

bool BlockchainSync::Sync(uint32_t start_height, uint32_t max_blocks) {
    if (!rpc_client_) {
        last_error_ = "Not initialized. Call Initialize() first.";
        return false;
    }

    try {
        // Get current blockchain height
        uint32_t current_height = GetCurrentHeight();
        if (current_height == 0) {
            last_error_ = "Failed to get blockchain height";
            return false;
        }

        // Initialize stats
        stats_.start_height = start_height;
        stats_.current_height = current_height;
        stats_.scanned_height = start_height > 0 ? start_height - 1 : 0;
        stats_.blocks_scanned = 0;
        stats_.transactions_found = 0;
        stats_.utxos_added = 0;
        stats_.utxos_spent = 0;
        stats_.sync_start_time = std::time(nullptr);
        stats_.sync_end_time = 0;
        stats_.is_syncing = true;
        stats_.last_error = "";
        stop_requested_ = false;

        // Calculate end height
        uint32_t end_height = current_height;
        if (max_blocks > 0) {
            end_height = std::min(start_height + max_blocks - 1, current_height);
        }

        if (config_.verbose) {
            std::cout << "Starting sync from height " << start_height
                      << " to " << end_height << std::endl;
        }

        // Sync blocks in batches
        for (uint32_t height = start_height; height <= end_height && !stop_requested_; ++height) {
            if (!ProcessBlock(height)) {
                stats_.is_syncing = false;
                stats_.sync_end_time = std::time(nullptr);
                return false;
            }

            stats_.scanned_height = height;
            stats_.blocks_scanned++;

            // Update wallet's last block height
            wallet_->SetCurrentHeight(height);

            // Progress reporting
            if (config_.verbose && height % 100 == 0) {
                std::cout << "Scanned block " << height << "/" << end_height
                          << " (" << stats_.transactions_found << " txs, "
                          << stats_.utxos_added << " UTXOs)" << std::endl;
            }
        }

        // Sync completed
        stats_.is_syncing = false;
        stats_.sync_end_time = std::time(nullptr);

        if (config_.verbose) {
            std::cout << "Sync completed!" << std::endl;
            std::cout << "  Blocks scanned: " << stats_.blocks_scanned << std::endl;
            std::cout << "  Transactions found: " << stats_.transactions_found << std::endl;
            std::cout << "  UTXOs added: " << stats_.utxos_added << std::endl;
            std::cout << "  UTXOs spent: " << stats_.utxos_spent << std::endl;
        }

        return !stop_requested_;

    } catch (const std::exception& e) {
        last_error_ = std::string("Sync failed: ") + e.what();
        stats_.is_syncing = false;
        stats_.sync_end_time = std::time(nullptr);
        stats_.last_error = last_error_;
        return false;
    }
}

bool BlockchainSync::SyncBlock(uint32_t height) {
    if (!rpc_client_) {
        last_error_ = "Not initialized. Call Initialize() first.";
        return false;
    }

    return ProcessBlock(height);
}

bool BlockchainSync::SyncBlock(const std::string& block_hash) {
    if (!rpc_client_) {
        last_error_ = "Not initialized. Call Initialize() first.";
        return false;
    }

    // We need to get the height for this block
    // For now, we'll process it with height 0 (unknown)
    // In a production system, you'd query the block header first
    return ProcessBlockByHash(block_hash, 0);
}

uint32_t BlockchainSync::GetCurrentHeight() const {
    if (!rpc_client_) {
        return 0;
    }

    try {
        auto result = rpc_client_->getblockchaininfo();

        if (result.isMember("result") && result["result"].isMember("blocks")) {
            return result["result"]["blocks"].asUInt();
        }

        // If no result member, try direct access (some RPCs return result directly)
        if (result.isMember("blocks")) {
            return result["blocks"].asUInt();
        }

        return 0;

    } catch (const std::exception& e) {
        last_error_ = std::string("Failed to get blockchain height: ") + e.what();
        return 0;
    }
}

BlockchainSyncStats BlockchainSync::GetStats() const {
    return stats_;
}

bool BlockchainSync::IsConnected() const {
    if (!rpc_client_) {
        return false;
    }

    try {
        auto result = rpc_client_->getblockchaininfo();
        return result.isMember("result") || result.isMember("blocks");
    } catch (...) {
        return false;
    }
}

void BlockchainSync::Stop() {
    stop_requested_ = true;
}

std::string BlockchainSync::GetLastError() const {
    return last_error_;
}

// Private methods

bool BlockchainSync::ProcessBlock(uint32_t height) {
    try {
        // Get block hash for this height
        Json::Value params;
        params.append(height);
        auto hash_result = rpc_client_->getblockhash(height);

        std::string block_hash;
        if (hash_result.isMember("result")) {
            block_hash = hash_result["result"].asString();
        } else if (hash_result.isString()) {
            block_hash = hash_result.asString();
        } else {
            last_error_ = "Failed to get block hash for height " + std::to_string(height);
            return false;
        }

        return ProcessBlockByHash(block_hash, height);

    } catch (const std::exception& e) {
        last_error_ = "Failed to process block at height " + std::to_string(height) + ": " + e.what();
        return false;
    }
}

bool BlockchainSync::ProcessBlockByHash(const std::string& block_hash, uint32_t height) {
    try {
        // Get full block data
        auto block_result = rpc_client_->getblock(block_hash);

        Json::Value block;
        if (block_result.isMember("result")) {
            block = block_result["result"];
        } else {
            block = block_result;
        }

        // Extract height from block if not provided
        if (height == 0 && block.isMember("height")) {
            height = block["height"].asUInt();
        }

        // Parse block
        ParseBlock(block, height);

        return true;

    } catch (const std::exception& e) {
        last_error_ = "Failed to process block " + block_hash + ": " + e.what();
        return false;
    }
}

void BlockchainSync::ParseBlock(
    const Json::Value& block,
    uint32_t height
) {
    // Extract transactions
    if (!block.isMember("tx")) {
        return;
    }

    const auto& transactions = block["tx"];
    if (!transactions.isArray()) {
        return;
    }

    // Process each transaction
    for (unsigned int i = 0; i < transactions.size(); ++i) {
        const auto& tx = transactions[i];

        // First transaction is always coinbase
        bool is_coinbase = (i == 0);

        ParseTransaction(tx, height, is_coinbase);
    }
}

void BlockchainSync::ParseTransaction(
    const Json::Value& tx,
    uint32_t height,
    bool is_coinbase
) {
    // Get transaction ID
    if (!tx.isMember("txid")) {
        return;
    }
    std::string txid = tx["txid"].asString();

    // Check outputs (vout) for relevant UTXOs
    if (tx.isMember("vout") && tx["vout"].isArray()) {
        const auto& vouts = tx["vout"];

        for (unsigned int vout_idx = 0; vout_idx < vouts.size(); ++vout_idx) {
            const auto& vout = vouts[vout_idx];

            if (IsRelevantOutput(vout)) {
                // Extract amount
                uint64_t amount = 0;
                if (vout.isMember("value")) {
                    // Value is in DIN, convert to una
                    double value_din = vout["value"].asDouble();
                    amount = static_cast<uint64_t>(value_din * 100000000);
                } else if (vout.isMember("amount")) {
                    double value_din = vout["amount"].asDouble();
                    amount = static_cast<uint64_t>(value_din * 100000000);
                }

                // Extract script pubkey
                std::string script_pubkey;
                if (vout.isMember("scriptPubKey") && vout["scriptPubKey"].isMember("hex")) {
                    script_pubkey = vout["scriptPubKey"]["hex"].asString();
                }

                // Create UTXO
                UTXO utxo;
                utxo.txid = txid;
                utxo.vout = vout_idx;
                utxo.amount = amount;
                utxo.script_pubkey = script_pubkey;
                utxo.height = height;
                utxo.is_coinbase = is_coinbase;

                // Add to wallet
                wallet_->AddUTXO(utxo);
                stats_.utxos_added++;
                stats_.transactions_found++;

                if (config_.verbose) {
                    std::cout << "  Found UTXO: " << txid << ":" << vout_idx
                              << " (" << (amount / 100000000.0) << " DIN)" << std::endl;
                }
            }
        }
    }

    // Check inputs (vin) for spent UTXOs
    if (tx.isMember("vin") && tx["vin"].isArray()) {
        const auto& vins = tx["vin"];

        for (unsigned int vin_idx = 0; vin_idx < vins.size(); ++vin_idx) {
            const auto& vin = vins[vin_idx];

            // Skip coinbase inputs
            if (vin.isMember("coinbase")) {
                continue;
            }

            // Get spent UTXO reference
            if (vin.isMember("txid") && vin.isMember("vout")) {
                std::string spent_txid = vin["txid"].asString();
                uint32_t spent_vout = vin["vout"].asUInt();

                // Check if this UTXO belongs to our wallet
                // We'll try to remove it - if it doesn't exist, nothing happens
                try {
                    wallet_->RemoveUTXO(spent_txid, spent_vout, txid, height);
                    stats_.utxos_spent++;

                    if (config_.verbose) {
                        std::cout << "  Spent UTXO: " << spent_txid << ":" << spent_vout << std::endl;
                    }
                } catch (...) {
                    // UTXO not in wallet, ignore
                }
            }
        }
    }
}

bool BlockchainSync::IsRelevantOutput(const Json::Value& vout) const {
    std::string address = ExtractAddress(vout);
    if (address.empty()) {
        return false;
    }

    // Check if this address matches our wallet's address
    return (address == wallet_->GetAddress());
}

std::string BlockchainSync::ExtractAddress(const Json::Value& vout) const {
    // Try to extract address from scriptPubKey
    if (vout.isMember("scriptPubKey")) {
        const auto& script_pubkey = vout["scriptPubKey"];

        // Check for addresses array
        if (script_pubkey.isMember("addresses") && script_pubkey["addresses"].isArray()) {
            const auto& addresses = script_pubkey["addresses"];
            if (addresses.size() > 0) {
                return addresses[0].asString();
            }
        }

        // Check for single address field
        if (script_pubkey.isMember("address")) {
            return script_pubkey["address"].asString();
        }
    }

    return "";
}

void BlockchainSync::UpdateStats() {
    // Stats are updated inline during sync
    // This method is reserved for future use
}

} // namespace reference
} // namespace wallet
} // namespace dinero
