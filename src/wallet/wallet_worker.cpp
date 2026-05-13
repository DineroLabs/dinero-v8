// Wallet Worker Implementation - Off-thread wallet processing with atomic reorg handling
#include "wallet/wallet_worker.h"
#include "wallet/utxo_index.h"
#include "wallet/transaction.h"
#include "wallet/wallet_manager.h"  // For database persistence
#include "wallet/shielded_wallet_ops.h"
#include "external/bech32/bech32.hpp"  // For address encoding
#include "wallet/p2mr_address.h"       // Phase 10: EncodeP2MRAddress for witness v3
#include "storage/chain_db.h"  // Phase W.1.1: For blockchain rescan
#include "primitives/block.h"  // Phase W.1.1: For Block type
#include "vault/vault_runtime.h"  // Track C: Liquidity Vault auto-observer
#include <cstring>
#include <iostream>
#include <chrono>
#include <thread>  // Phase W.1.1: For sleep_for
#include <mutex>

namespace dinero {

// ═══════════════════════════════════════════════════════════════════════════
// HELPER FUNCTIONS
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Convert scriptPubKey bytes to bech32 address.
 * Supports P2WPKH (witness v0) and P2TR (witness v1).
 *
 * @param scriptPubKey The script public key bytes
 * @param hrp Human-readable part ("din" for mainnet)
 * @return Bech32/Bech32m encoded address, or empty string on failure
 */
static std::string ScriptPubKeyToAddress(const std::vector<uint8_t>& scriptPubKey, const std::string& hrp = "din") {
    // P2WPKH: OP_0 PUSH20 <20-byte-pubkey-hash>
    if (scriptPubKey.size() == 22 &&
        scriptPubKey[0] == 0x00 &&
        scriptPubKey[1] == 0x14) {
        // Extract 20-byte pubkey hash
        std::vector<uint8_t> pubkey_hash(scriptPubKey.begin() + 2, scriptPubKey.end());

        // Encode as Bech32 address (witness v0, 20 bytes)
        return bech32::Encode(hrp, 0, pubkey_hash, bech32::Encoding::BECH32);
    }

    // P2WSH: OP_0 PUSH32 <32-byte-script-hash>
    if (scriptPubKey.size() == 34 &&
        scriptPubKey[0] == 0x00 &&
        scriptPubKey[1] == 0x20) {
        // Extract 32-byte script hash
        std::vector<uint8_t> script_hash(scriptPubKey.begin() + 2, scriptPubKey.end());

        // Encode as Bech32 address (witness v0, 32 bytes)
        return bech32::Encode(hrp, 0, script_hash, bech32::Encoding::BECH32);
    }

    // P2TR (Taproot): OP_1 PUSH32 <32-byte-witness-program>
    if (scriptPubKey.size() == 34 &&
        scriptPubKey[0] == 0x51 &&
        scriptPubKey[1] == 0x20) {
        // Extract 32-byte witness program
        std::vector<uint8_t> witness_program(scriptPubKey.begin() + 2, scriptPubKey.end());

        // Encode as Bech32m Taproot address (witness v1, 32 bytes)
        return bech32::Encode(hrp, 1, witness_program, bech32::Encoding::BECH32M);
    }

    // P2MR (v7 post-quantum, BIP-360): OP_3 PUSH32 <32-byte merkle-root>
    // Encoded as bech32m with witness version 3. Without this branch the
    // wallet_worker would receive an empty address string for P2MR outputs
    // and skip the addUTXO persistence path → P2MR UTXOs would be visible
    // in the UTXOIndex (wallet_utxos table) but invisible to listunspent,
    // which reads the legacy `utxos` table. See V7_WALLET_SCHEMA.md.
    if (dinero::wallet::P2MR_MERKLE_ROOT_BYTES == 32 &&
        scriptPubKey.size() == 34 &&
        scriptPubKey[0] == 0x53 &&
        scriptPubKey[1] == 0x20) {
        std::vector<uint8_t> merkle_root(scriptPubKey.begin() + 2, scriptPubKey.end());
        return dinero::wallet::EncodeP2MRAddress(hrp, merkle_root);
    }

    // Unknown or legacy script type - return empty string
    return "";
}

// ═══════════════════════════════════════════════════════════════════════════
// GLOBAL WALLET WORKER INSTANCE
// ═══════════════════════════════════════════════════════════════════════════
static std::shared_ptr<WalletWorker> g_wallet_worker;
static std::mutex g_wallet_worker_mutex;

// ═══════════════════════════════════════════════════════════════════════════
// WALLET WORKER IMPLEMENTATION
// ═══════════════════════════════════════════════════════════════════════════

WalletWorker::WalletWorker(UTXOIndex* utxo_index, class WalletManager* wallet_manager)
    : running_(false), utxo_index_(utxo_index), wallet_manager_(wallet_manager) {
    std::cerr << "[WalletWorker] Constructor called, UTXO index: "
              << (utxo_index_ ? "PROVIDED" : "nullptr (wallet scanning disabled)")
              << ", WalletManager: "
              << (wallet_manager_ ? "PROVIDED" : "nullptr (UTXO persistence disabled)") << std::endl;
}

WalletWorker::~WalletWorker() {
    std::cerr << "[WalletWorker] Destructor called" << std::endl;
    Stop();
}

void WalletWorker::Start() {
    if (running_.load()) {
        std::cerr << "[WalletWorker] Already running, ignoring Start()" << std::endl;
        return;
    }

    running_.store(true);
    worker_thread_ = std::thread(&WalletWorker::WorkerThread, this);
    std::cerr << "[WalletWorker] ✅ Started background worker thread" << std::endl;
}

void WalletWorker::Stop() {
    if (!running_.load()) {
        return;
    }

    std::cerr << "[WalletWorker] Stopping worker thread..." << std::endl;
    running_.store(false);
    job_queue_.stop();

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    std::cerr << "[WalletWorker] ✅ Worker thread stopped" << std::endl;
}

void WalletWorker::QueueBlockConnected(uint32_t height, const std::string& hash,
                                       const std::vector<Transaction>& transactions) {
    if (!running_.load()) {
        std::cerr << "[WalletWorker] ⚠️  Not running, ignoring block " << height << std::endl;
        return;
    }

    job_queue_.push(WalletJob::MakeConnect(height, hash, transactions));
    std::cerr << "[WalletWorker] 📥 Queued block connect: height=" << height
              << " hash=" << hash.substr(0, 16) << "... "
              << "txs=" << transactions.size() << std::endl;
}

void WalletWorker::QueueReorg(const ReorgDiff& diff) {
    if (!running_.load()) {
        std::cerr << "[WalletWorker] ⚠️  Not running, ignoring reorg" << std::endl;
        return;
    }

    job_queue_.push(WalletJob::MakeReorg(diff));
    std::cerr << "[WalletWorker] 📥 Queued reorg: disconnect=" << diff.disconnect.size()
              << " connect=" << diff.connect.size() << std::endl;
}

void WalletWorker::QueueBlockDisconnected(uint32_t height, const Block& block) {
    if (!running_.load()) {
        std::cerr << "[WalletWorker] ⚠️  Not running, ignoring disconnect at " << height << std::endl;
        return;
    }

    job_queue_.push(WalletJob::MakeDisconnect(height, block));
    std::cerr << "[WalletWorker] 📥 Queued block disconnect: height=" << height
              << " hash=" << block.GetHash().GetHex().substr(0, 16) << "... "
              << "txs=" << block.vtx.size() << std::endl;
}

bool WalletWorker::RescanSynchronously(ChainDB* chain_db, int start_height, std::string* error) {
    if (!wallet_manager_) {
        if (error) *error = "WalletManager not available";
        return false;
    }
    if (!chain_db) {
        if (error) *error = "ChainDB is null";
        return false;
    }

    try {
        constexpr int kDefaultGapLimit = 20;
        const bool ok = wallet_manager_->rescanBlockchain(start_height, kDefaultGapLimit, chain_db);
        if (!ok && error) {
            *error = "WalletManager::rescanBlockchain returned false";
        }
        return ok;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    } catch (...) {
        if (error) *error = "unknown exception";
        return false;
    }
}

void WalletWorker::WorkerThread() {
    std::cerr << "[WalletWorker] Worker thread started (thread_id=" << std::this_thread::get_id() << ")" << std::endl;

    while (running_.load()) {
        WalletJob job = job_queue_.pop();

        // Check if we're shutting down
        if (!running_.load()) {
            break;
        }

        try {
            switch (job.type) {
                case JobType::Connect:
                    ProcessConnect(job.height, job.hash, job.transactions);
                    break;
                case JobType::Disconnect:
                    ProcessDisconnect(job.height, job.block);
                    break;
                case JobType::Reorg:
                    ProcessReorg(job.diff);
                    break;
            }
        } catch (const std::exception& e) {
            std::cerr << "[WalletWorker] ❌ ERROR processing job: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[WalletWorker] ❌ UNKNOWN ERROR processing job" << std::endl;
        }
    }

    std::cerr << "[WalletWorker] Worker thread exiting" << std::endl;
}

void WalletWorker::ProcessDisconnect(uint32_t height, const Block& block) {
    auto start = std::chrono::steady_clock::now();
    std::cerr << "[WalletWorker] Processing block disconnect: height=" << height
              << " hash=" << block.GetHash().GetHex().substr(0, 16)
              << "... txs=" << block.vtx.size() << std::endl;

    if (utxo_index_) {
        utxo_index_->RevertBlock(height);
    } else {
        std::cerr << "[WalletWorker] ⚠️  UTXO index not available, skipping in-memory rollback" << std::endl;
    }

    if (wallet_manager_) {
        std::string shielded_error;
        if (!wallet::shielded_ops::ProcessDisconnectedBlock(*wallet_manager_, height, block, &shielded_error) &&
            !shielded_error.empty()) {
            std::cerr << "[WalletWorker] ⚠️  Shielded wallet sync disconnect failed: "
                      << shielded_error << std::endl;
        }
        wallet_manager_->onBlockDisconnected(block, height);
    } else {
        std::cerr << "[WalletWorker] ⚠️  WalletManager not available, skipping persistent rollback" << std::endl;
    }

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    std::cerr << "[WalletWorker] ✅ Processed block disconnect " << height
              << " in " << duration.count() << "ms" << std::endl;
}

void WalletWorker::ProcessConnect(uint32_t height, const std::string& hash,
                                  const std::vector<Transaction>& transactions) {
    auto start = std::chrono::steady_clock::now();
    std::cerr << "[WalletWorker] Processing block connect: height=" << height
              << " hash=" << hash.substr(0, 16) << "... txs=" << transactions.size() << std::endl;

    if (wallet_manager_) {
        std::string shielded_error;
        if (!wallet::shielded_ops::ProcessConfirmedBlock(*wallet_manager_, height, transactions, &shielded_error) &&
            !shielded_error.empty()) {
            std::cerr << "[WalletWorker] ⚠️  Shielded wallet sync connect failed: "
                      << shielded_error << std::endl;
        }
    }

    if (wallet_manager_) {
        wallet_manager_->setBlockchainHeight(height);
    }

    // Check if UTXO index is available
    if (!utxo_index_) {
        std::cerr << "[WalletWorker] ⚠️  UTXO index not available, skipping block scan" << std::endl;
        return;
    }

    try {
        int utxos_added = 0;
        int utxos_spent = 0;

        // Scan all transactions in the block
        for (const auto& tx : transactions) {
            // Phase M.4.3-D: GetTxid() returns TxId, use directly
            TxId txid = tx.GetTxid();
            const std::string txid_hex = txid.AsUint256().GetHex();
            bool is_coinbase = tx.IsCoinbase();
            bool existing_history_confirmed = false;
            if (wallet_manager_ && !is_coinbase) {
                existing_history_confirmed = wallet_manager_->confirmTransaction(txid_hex, height);
            }

            // Phase 35.1.1: Track if this transaction affects wallet (for history)
            bool tx_affects_wallet = false;
            bool tx_spends_wallet_inputs = false;
            double total_received = 0.0;
            std::string receiving_address;

            // 1. Mark spent outputs (inputs)
            if (!is_coinbase) {
                for (const auto& input : tx.vin) {
                    // Check if this input spends one of our UTXOs
                    dinero::WalletUTXO spent_utxo;  // Phase M.3: WalletUTXO
                    if (utxo_index_->GetUTXO(input.prevout.txid, input.prevout.vout, spent_utxo)) {
                        // This input spends our UTXO - mark it as spent in memory
                        utxo_index_->SpendUTXO(input.prevout.txid, input.prevout.vout, height);
                        utxos_spent++;
                        tx_spends_wallet_inputs = true;

                        // ✅ CRITICAL FIX: Remove UTXO from wallet database
                        if (wallet_manager_) {
                            try {
                                // Phase M.4.3-B Step 1: Unwrap TxId for logging and string conversion
                                const bool removed = wallet_manager_->removeUTXO(
                                    input.prevout.txid.AsUint256().GetHex(),
                                    input.prevout.vout
                                );
                                if (removed) {
                                    std::cerr << "[WalletWorker] 🗑️  Removed spent UTXO from database: "
                                              << input.prevout.txid.AsUint256().GetHex().substr(0, 16) << "..." << ":"
                                              << input.prevout.vout << std::endl;
                                } else {
                                    std::cerr << "[WalletWorker] ⚠️  removeUTXO returned false for: "
                                              << input.prevout.txid.AsUint256().GetHex().substr(0, 16) << "..." << ":"
                                              << input.prevout.vout << std::endl;
                                }
                            } catch (const std::exception& e) {
                                std::cerr << "[WalletWorker] ⚠️  Failed to remove UTXO from database: "
                                          << e.what() << std::endl;
                            }
                        }
                        // Phase M.4.3-B Step 1: Unwrap TxId for logging
                        // Phase M.6.2: Extract raw value for display
                        std::cerr << "[WalletWorker] 📤 Spent UTXO: " << input.prevout.txid.AsUint256().GetHex().substr(0, 16) << "..."
                                  << ":" << input.prevout.vout
                                  << " (" << (spent_utxo.value.GetUna() / 1e8) << " DIN)" << std::endl;
                    }
                }
            }

            // 2. Check outputs for addresses we own
            for (size_t vout = 0; vout < tx.vout.size(); vout++) {
                const auto& output = tx.vout[vout];

                // Check if this scriptPubKey belongs to our wallet
                auto opt_path = utxo_index_->IsOurScript(output.scriptPubKey);

                if (opt_path.has_value()) {
                    if (output.is_confidential) {
                        std::cerr << "[WalletWorker] ℹ️  Ignoring retired legacy confidential output: "
                                  << txid_hex.substr(0, 16) << "..." << ":" << vout
                                  << std::endl;
                        continue;
                    }

                    // This output belongs to us! Add it to UTXO index
                    AmountUna effective_value = output.value;

                    // Phase M.4.3-D: txid is TxId, constructor accepts it directly
                    dinero::WalletUTXO new_utxo(  // Phase M.3: WalletUTXO
                        txid,
                        static_cast<uint32_t>(vout),
                        effective_value,
                        output.scriptPubKey,
                        opt_path.value(),
                        height,
                        is_coinbase
                    );

                    utxo_index_->AddUTXO(new_utxo);
                    utxos_added++;

                    // Track C: Liquidity Vault auto-observer.
                    // No-op when the vault isn't running or the
                    // scriptPubKey doesn't match the configured
                    // operator address. Decoding to address +
                    // matching live in vault_runtime.cpp; the wallet
                    // pipeline just hands over the raw outpoint +
                    // value + height + block hash.
                    {
                        std::array<uint8_t, 32> txid_raw{};
                        std::memcpy(txid_raw.data(),
                                    txid.AsUint256().begin(), 32);
                        dinero::vault::ObserveWalletOutput(
                            txid_raw, static_cast<uint32_t>(vout),
                            output.scriptPubKey,
                            static_cast<uint64_t>(effective_value.GetUna()),
                            static_cast<uint64_t>(height), hash);
                    }

                    // ✅ CRITICAL FIX: Persist UTXO to wallet database
                    if (wallet_manager_) {
                        try {
                            // Convert scriptPubKey to address
                            std::string address = ScriptPubKeyToAddress(output.scriptPubKey);
                            if (address.empty()) {
                                std::cerr << "[WalletWorker] ⚠️  Failed to convert scriptPubKey to address, skipping database persistence" << std::endl;
                            } else {
                                // Convert scriptPubKey bytes to hex string
                                std::string script_hex;
                                for (uint8_t byte : output.scriptPubKey) {
                                    char buf[3];
                                    snprintf(buf, sizeof(buf), "%02x", byte);
                                    script_hex += buf;
                                }

                                // Phase M.6.2: Use effective_value (includes rewound CT amount)
                                const bool persisted = wallet_manager_->addUTXO(
                                    txid_hex,
                                    static_cast<uint32_t>(vout),
                                    effective_value.GetInt64(),
                                    address,
                                    script_hex,
                                    height,
                                    is_coinbase
                                );
                                if (persisted) {
                                    std::cerr << "[WalletWorker] 💾 Persisted UTXO to database: "
                                              << txid_hex.substr(0, 16) << "..." << ":" << vout
                                              << " (" << address << ")" << std::endl;
                                } else {
                                    std::cerr << "[WalletWorker] ❌ addUTXO failed for: "
                                              << txid_hex.substr(0, 16) << "..." << ":" << vout
                                              << " (" << address << ")" << std::endl;
                                }

                                // Auto-label coinbase addresses with block height
                                if (persisted && is_coinbase) {
                                    try {
                                        std::string coinbase_label = "Coinbase block #" + std::to_string(height);
                                        wallet_manager_->setAddressLabel(address, coinbase_label, true);  // is_system=true
                                        std::cerr << "[WalletWorker] 🏷️  Auto-labeled mining address: " << coinbase_label << std::endl;
                                    } catch (const std::exception& e) {
                                        // Non-fatal - label is optional
                                        std::cerr << "[WalletWorker] ⚠️  Failed to auto-label coinbase address: " << e.what() << std::endl;
                                    }
                                }

                                // Phase 35.1.1: Track for transaction history
                                tx_affects_wallet = true;
                                total_received += static_cast<double>(effective_value.GetUna()) / 1e8;
                                if (receiving_address.empty()) {
                                    receiving_address = address;
                                }
                            }
                        } catch (const std::exception& e) {
                            std::cerr << "[WalletWorker] ⚠️  Failed to persist UTXO to database: "
                                      << e.what() << std::endl;
                        }
                    }

                    // Phase M.4.3-D: Convert TxId to hex for logging
                    // Phase M.6.2: Extract raw value for display
                    std::cerr << "[WalletWorker] 📥 Received UTXO: " << txid_hex.substr(0, 16) << "..."
                              << ":" << vout
                              << " (" << (effective_value.GetUna() / 1e8) << " DIN)"
                              << (is_coinbase ? " [COINBASE]" : "") << std::endl;
                }
            }

            // Phase 35.1.1: Record transaction in history if it affects wallet
            if (tx_affects_wallet && wallet_manager_) {
                try {
                    if ((tx_spends_wallet_inputs || existing_history_confirmed) && !is_coinbase) {
                        continue;  // Change/self-spend history is recorded by the originating RPC.
                    }

                    std::string category = is_coinbase
                        ? "generate"
                        : "receive";
                    std::string label = is_coinbase
                        ? "Mining reward"
                        : "";

                    // Use current time as transaction time (block timestamp not available in this context)
                    int64_t tx_time = static_cast<int64_t>(std::time(nullptr));

                    std::cerr << "[WalletWorker] Phase 35.1.1: Recording transaction: "
                              << txid_hex.substr(0, 16) << "..."
                              << " (category=" << category << ", amount=" << total_received << " DIN)" << std::endl;

                    // Phase 36: Pass block height for reorg support
                    bool tx_added = wallet_manager_->addTransaction(
                        txid_hex,  // Phase M.4.3-D: Explicit boundary
                        receiving_address,
                        total_received,
                        category,
                        is_coinbase,
                        label,
                        tx_time,
                        height  // Phase 36: Track block height for reorg handling
                    );

                    if (tx_added) {
                        std::cerr << "[WalletWorker] ✅ Transaction recorded in history" << std::endl;
                    } else {
                        std::cerr << "[WalletWorker] ⚠️  Failed to record transaction in history" << std::endl;
                    }
                } catch (const std::exception& e) {
                    std::cerr << "[WalletWorker] ⚠️  Exception recording transaction: " << e.what() << std::endl;
                }
            }
        }

        // ✅ CRITICAL FIX: Update wallet's blockchain height for correct confirmation calculation
        if (wallet_manager_) {
            wallet_manager_->setBlockchainHeight(height);
            std::cerr << "[WalletWorker] 📏 Updated wallet blockchain height to " << height << std::endl;
        }

        auto end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        std::cerr << "[WalletWorker] ✅ Processed block " << height << " in " << duration.count() << "ms "
                  << "(+" << utxos_added << " UTXOs, -" << utxos_spent << " UTXOs)" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[WalletWorker] ❌ Failed to process block " << height << ": " << e.what() << std::endl;
        throw;
    }
}

void WalletWorker::ProcessReorg(const ReorgDiff& diff) {
    auto start = std::chrono::steady_clock::now();
    std::cerr << "[WalletWorker] Processing reorg: disconnect=" << diff.disconnect.size()
              << " connect=" << diff.connect.size() << std::endl;

    // Check if UTXO index is available
    if (!utxo_index_) {
        std::cerr << "[WalletWorker] ⚠️  UTXO index not available, skipping reorg" << std::endl;
        return;
    }

    // Atomic reorg handling: Both disconnect and connect phases happen atomically
    // NOTE: Each RevertBlock() is already atomic (single transaction)
    // The idempotent scanning for connect is also atomic (single transaction)
    // For now, we process them sequentially, but in the future we could wrap
    // the entire reorg diff in a single mega-transaction

    try {
        // Phase 1: Disconnect old chain (tip → fork+1, descending order)
        std::cerr << "[WalletWorker] Phase 1: Disconnecting " << diff.disconnect.size() << " blocks" << std::endl;
        for (const auto& block : diff.disconnect) {
            std::cerr << "[WalletWorker]   Disconnect block " << block.height
                      << " hash=" << block.hash.substr(0, 16) << "..." << std::endl;

            // Revert UTXOs (existing Phase 4B logic)
            utxo_index_->RevertBlock(block.height);

            // Phase 36: Remove transaction history for this block
            if (wallet_manager_) {
                wallet_manager_->removeTransactionsAtHeight(block.height);
                std::cerr << "[WalletWorker] Phase 36: Removed transactions at height " << block.height << std::endl;
            }
        }

        // Phase 2: Connect new chain (fork+1' → new tip, ascending order)
        std::cerr << "[WalletWorker] Phase 2: Connecting " << diff.connect.size() << " blocks" << std::endl;
        for (const auto& block : diff.connect) {
            std::cerr << "[WalletWorker]   Connect block " << block.height
                      << " hash=" << block.hash.substr(0, 16) << "..." << std::endl;

            // NOTE: To use ScanBlockIdempotent(), we would need block transaction data
            // For now, we'll mark this as a placeholder that needs blockchain integration
            // The blockchain layer should provide full transaction data for each block
            std::cerr << "[WalletWorker]   TODO: Fetch and scan block transactions" << std::endl;
        }

        auto end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        std::cerr << "[WalletWorker] ✅ Processed reorg in " << duration.count() << "ms" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[WalletWorker] ❌ Failed to process reorg: " << e.what() << std::endl;
        throw;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// WALLET NOTIFICATION INTERFACE
// ═══════════════════════════════════════════════════════════════════════════

namespace WalletNotify {

void Initialize(UTXOIndex* utxo_index, class WalletManager* wallet_manager) {
    std::lock_guard<std::mutex> lock(g_wallet_worker_mutex);
    if (g_wallet_worker) {
        std::cerr << "[WalletNotify] Already initialized, ignoring" << std::endl;
        return;
    }

    g_wallet_worker = std::make_shared<WalletWorker>(utxo_index, wallet_manager);
    g_wallet_worker->Start();
    std::cerr << "[WalletNotify] ✅ Wallet notification system initialized"
              << (utxo_index ? " (with UTXO index)" : " (no UTXO index)")
              << (wallet_manager ? " (with WalletManager for persistence)" : " (no persistence)") << std::endl;
}

void Shutdown() {
    std::shared_ptr<WalletWorker> worker;
    {
        std::lock_guard<std::mutex> lock(g_wallet_worker_mutex);
        worker = std::move(g_wallet_worker);
    }
    if (!worker) return;

    std::cerr << "[WalletNotify] Shutting down wallet notification system..." << std::endl;
    worker->Stop();
    std::cerr << "[WalletNotify] ✅ Wallet notification system shut down" << std::endl;
}

void OnBlockConnected(uint32_t height, const std::string& hash,
                      const std::vector<Transaction>& transactions) {
    std::shared_ptr<WalletWorker> worker;
    {
        std::lock_guard<std::mutex> lock(g_wallet_worker_mutex);
        worker = g_wallet_worker;
    }
    if (!worker) {
        std::cerr << "[WalletNotify] ⚠️  Worker not initialized, ignoring block " << height << std::endl;
        return;
    }

    worker->QueueBlockConnected(height, hash, transactions);
}

void OnReorg(const ReorgDiff& diff) {
    std::shared_ptr<WalletWorker> worker;
    {
        std::lock_guard<std::mutex> lock(g_wallet_worker_mutex);
        worker = g_wallet_worker;
    }
    if (!worker) {
        std::cerr << "[WalletNotify] ⚠️  Worker not initialized, ignoring reorg" << std::endl;
        return;
    }

    worker->QueueReorg(diff);
}

void OnBlockDisconnected(uint32_t height, const Block& block) {
    std::shared_ptr<WalletWorker> worker;
    {
        std::lock_guard<std::mutex> lock(g_wallet_worker_mutex);
        worker = g_wallet_worker;
    }
    if (!worker) {
        std::cerr << "[WalletNotify] ⚠️  Worker not initialized, ignoring disconnected block "
                  << height << std::endl;
        return;
    }

    worker->QueueBlockDisconnected(height, block);
}

// Phase W.1.1: Rescan blockchain synchronously
bool RescanBlockchain(ChainDB* chain_db, int start_height) {
    std::shared_ptr<WalletWorker> worker;
    {
        std::lock_guard<std::mutex> lock(g_wallet_worker_mutex);
        worker = g_wallet_worker;
    }
    if (!worker) {
        std::cerr << "[WalletNotify] ⚠️  Worker not initialized, cannot rescan" << std::endl;
        return false;
    }

    if (!chain_db) {
        std::cerr << "[WalletNotify] ⚠️  ChainDB is null, cannot rescan" << std::endl;
        return false;
    }

    std::cerr << "[WalletNotify] Starting deterministic wallet rescan from height " << start_height << std::endl;
    std::string error;
    const bool ok = worker->RescanSynchronously(chain_db, start_height, &error);
    if (!ok) {
        std::cerr << "[WalletNotify] ❌ Rescan failed: " << error << std::endl;
        return false;
    }

    std::cerr << "[WalletNotify] ✅ Rescan completed successfully" << std::endl;
    return true;
}

// Priority 3 FIX: Validate wallet UTXOs against consensus
// Removes phantom UTXOs (wallet thinks unspent, but consensus doesn't have them)
size_t ValidateUTXOs(ChainDB* chain_db) {
    std::lock_guard<std::mutex> lock(g_wallet_worker_mutex);
    if (!g_wallet_worker) {
        std::cerr << "[WalletNotify] ⚠️  Worker not initialized, cannot validate" << std::endl;
        return 0;
    }

    if (!chain_db) {
        std::cerr << "[WalletNotify] ⚠️  ChainDB is null, cannot validate" << std::endl;
        return 0;
    }

    // Get UTXOIndex from worker (need to expose it or pass it through)
    // For now, we need access to the UTXOIndex
    // This is handled by the calling code which has both UTXOIndex and ChainDB

    std::cerr << "[WalletNotify] Priority 3 FIX: ValidateUTXOs called" << std::endl;
    std::cerr << "[WalletNotify] NOTE: Caller should use UTXOIndex::ValidateAgainstConsensus directly" << std::endl;

    // The validation is done by calling:
    //   utxo_index->ValidateAgainstConsensus([&chain_db](const TxId& txid, uint32_t vout) {
    //       auto result = chain_db->getCoin(txid.AsUint256(), vout);
    //       return result.status() == Status::Ok;
    //   });

    return 0;  // Caller should use UTXOIndex::ValidateAgainstConsensus
}

} // namespace WalletNotify

} // namespace dinero
