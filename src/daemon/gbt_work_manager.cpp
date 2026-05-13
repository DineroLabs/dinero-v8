#include "daemon/gbt_work_manager.h"
#include "daemon/mining_engine.h"
#include "daemon/mempool.h"
#include "daemon/mempool_globals.h"
#include "daemon/mining_payout_resolver.h"
#include "daemon/block_acceptor.h"
#include "daemon/daemon_context.h"  // Week 3: Context injection
#include "daemon/services/chainstate_service.h"  // Week 3: Service access
#include "common/sha256d.h"
#include "common/logger.h"
#include "common/address_script_builder.h"
#include "consensus/pow.hpp"
#include "consensus/subsidy.h"  // Phase M.5.3: Consensus subsidy query
#include "consensus/block_validation.h"  // For BlockValidator::ComputeUtreexoRootPure
#include "consensus/utreexo_activation.h"  // For FullRulesActive()
#include "storage/chain_direct.h"

#include <sqlite3.h>
#include <algorithm>
#include <random>
#include <sstream>
#include <iomanip>
#include <cstring>

GBTWorkManager::GBTWorkManager(DaemonContext* ctx) : m_context(ctx) {}

GBTWorkManager::~GBTWorkManager() {
    Stop();
}

bool GBTWorkManager::Start(const GBTConfig& config) {
    if (m_isRunning.load()) {
        return false; // Already running
    }

    m_config = config;
    m_shouldStop.store(false);
    m_isRunning.store(true);

    // Start background threads
    m_refreshThread = std::thread(&GBTWorkManager::WorkRefreshLoop, this);
    m_monitorThread = std::thread(&GBTWorkManager::TipMonitorLoop, this);

    // Generate initial work template
    RefreshWork();

    return true;
}

void GBTWorkManager::Stop() {
    if (!m_isRunning.load()) {
        return; // Already stopped
    }

    m_shouldStop.store(true);
    m_workCondition.notify_all();

    // Join background threads
    if (m_refreshThread.joinable()) {
        m_refreshThread.join();
    }
    if (m_monitorThread.joinable()) {
        m_monitorThread.join();
    }

    m_isRunning.store(false);
}

std::shared_ptr<WorkTemplate> GBTWorkManager::GetCurrentWork() {
    std::lock_guard<std::mutex> lock(m_workMutex);
    return m_currentWork;
}

bool GBTWorkManager::RefreshWork() {
    try {
        // Build new block candidate
        BlockCandidate candidate = BuildBlockCandidate();

        // ✅ Phase 3: Convert to mining work template
        // Copy BlockHeader directly (authoritative source)
        auto workTemplate = std::make_shared<WorkTemplate>();
        workTemplate->header = candidate.header;  // Copy complete BlockHeader

        // Copy metadata (not part of header)
        workTemplate->height = candidate.height;
        workTemplate->coinbaseValue = std::to_string(candidate.coinbaseValue);
        workTemplate->transactions = candidate.transactions;
        workTemplate->templateId = candidate.templateId;
        workTemplate->createdAt = std::chrono::steady_clock::now();
        workTemplate->stale.store(false);

        // Update current work
        {
            std::lock_guard<std::mutex> lock(m_workMutex);
            
            // Mark old work as stale
            if (m_currentWork) {
                m_currentWork->stale.store(true);
            }
            
            m_currentWork = workTemplate;
        }

        // Notify callback if set
        if (m_workUpdateCallback) {
            m_workUpdateCallback(*workTemplate);
        }

        m_stats.templatesGenerated.fetch_add(1);
        m_stats.lastRefresh = std::chrono::steady_clock::now();

        return true;
    }
    catch (const std::exception& e) {
        // Log error but don't crash
        return false;
    }
}

void GBTWorkManager::InvalidateCurrentWork() {
    std::lock_guard<std::mutex> lock(m_workMutex);
    if (m_currentWork) {
        m_currentWork->stale.store(true);
    }
}

bool GBTWorkManager::SubmitBlock(const std::string& blockHex) {
    m_stats.blocksSubmitted.fetch_add(1);
    m_stats.lastSubmission = std::chrono::steady_clock::now();

    // Use callback if available
    if (m_blockSubmitCallback) {
        bool accepted = m_blockSubmitCallback(blockHex);
        if (accepted) {
            m_stats.blocksAccepted.fetch_add(1);
            
            // Invalidate current work since we found a block
            InvalidateCurrentWork();
            
            // Trigger immediate work refresh
            RefreshWork();
        }
        return accepted;
    }

    try {
        if (blockHex.empty()) {
            dinero::g_logger.error("Block data is empty");
            return false;
        }

        auto accept_result = dinero::BlockAcceptor::AcceptBlockFromRPC(blockHex, "gbt_submit");
        if (!accept_result.accepted()) {
            dinero::g_logger.error(
                "GBT block submission rejected: " +
                std::string(dinero::BlockRejectCodeToString(accept_result.code)) +
                " (" + accept_result.reason + ")");
            return false;
        }

        m_stats.blocksAccepted.fetch_add(1);

        // Invalidate current work since we found a block
        InvalidateCurrentWork();

        // Trigger immediate work refresh
        RefreshWork();

        return true;

    } catch (const std::exception& e) {
        dinero::g_logger.error("Block validation failed: " + std::string(e.what()));
        return false;
    }
}

int64_t GBTWorkManager::GetCurrentHeight() {
    int64_t height = -1;
    std::string hash;
    uint32_t bits;
    
    if (ReadBlockchainTip(height, hash, bits)) {
        return height;
    }
    
    return -1;
}

std::string GBTWorkManager::GetBestBlockHash() {
    int64_t height;
    std::string hash;
    uint32_t bits;
    
    if (ReadBlockchainTip(height, hash, bits)) {
        return hash;
    }
    
    return "";
}

uint32_t GBTWorkManager::GetNetworkDifficulty() {
    int64_t height;
    std::string hash;
    uint32_t bits;
    
    if (ReadBlockchainTip(height, hash, bits)) {
        return bits;
    }
    
    return 0x207fffff; // Default difficulty
}

void GBTWorkManager::UpdateConfig(const GBTConfig& config) {
    std::lock_guard<std::mutex> lock(m_workMutex);
    m_config = config;
}

void GBTWorkManager::WorkRefreshLoop() {
    while (!m_shouldStop.load()) {
        std::unique_lock<std::mutex> lock(m_workMutex);
        
        // Wait for refresh interval or stop signal
        if (m_workCondition.wait_for(lock, m_config.refreshInterval, 
                                     [this] { return m_shouldStop.load(); })) {
            break; // Stop requested
        }
        
        lock.unlock();
        
        // Refresh work template
        RefreshWork();
    }
}

void GBTWorkManager::TipMonitorLoop() {
    while (!m_shouldStop.load()) {
        try {
            int64_t currentHeight;
            std::string currentHash;
            uint32_t currentBits;
            
            if (ReadBlockchainTip(currentHeight, currentHash, currentBits)) {
                // Check if tip has changed
                if (currentHeight != m_lastKnownHeight || 
                    currentHash != m_lastKnownHash ||
                    currentBits != m_lastKnownBits) {
                    
                    // Tip changed - invalidate current work and refresh
                    InvalidateCurrentWork();
                    RefreshWork();
                    
                    // Update tracking
                    m_lastKnownHeight = currentHeight;
                    m_lastKnownHash = currentHash;
                    m_lastKnownBits = currentBits;
                }
            }
        }
        catch (const std::exception& e) {
            // Log error but continue monitoring
        }
        
        // Sleep before next check
        std::this_thread::sleep_for(TIP_MONITOR_INTERVAL);
    }
}

GBTWorkManager::BlockCandidate GBTWorkManager::BuildBlockCandidate() {
    BlockCandidate candidate;

    // ═══════════════════════════════════════════════════════════════════
    // Phase 3: Build BlockHeader directly (authoritative source)
    // ═══════════════════════════════════════════════════════════════════

    // Read current blockchain tip
    int64_t tipHeight;
    std::string prevHashHex;
    uint32_t tipBits;
    if (!ReadBlockchainTip(tipHeight, prevHashHex, tipBits)) {
        throw std::runtime_error("Failed to read blockchain tip");
    }
    (void)tipBits;

    if (!m_context || !m_context->chainstate) {
        throw std::runtime_error("GBTWorkManager: chainstate unavailable");
    }

    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(m_context->chainstate);
    if (!chainstate) {
        throw std::runtime_error("GBTWorkManager: failed to cast chainstate service");
    }

    auto* chain_db = chainstate->chainDB();
    if (!chain_db) {
        throw std::runtime_error("GBTWorkManager: ChainDB unavailable");
    }

    // Set metadata (not in header)
    candidate.height = tipHeight + 1;  // New block height
    candidate.totalFees = 0;
    candidate.totalSize = 0;

    // Select transactions from mempool
    candidate.transactions = SelectTransactions(candidate.totalFees, candidate.totalSize);

    // Calculate coinbase value (base reward + fees)
    // Phase M.5.3: Query consensus for block subsidy (100 DIN at height 2+, halves every 1,314,000 blocks)
    uint64_t blockSubsidy = dinero::ConsensusSubsidy::GetBlockSubsidy(candidate.height).GetUna();
    auto coinbase_value = dinero::CheckedAddUna(blockSubsidy, candidate.totalFees);
    if (!coinbase_value) {
        throw std::runtime_error("Coinbase value overflow while building GBT candidate");
    }
    candidate.coinbaseValue = *coinbase_value;

    // Build coinbase transaction
    std::string coinbaseTx = BuildCoinbaseTransaction(candidate.height,
                                                     candidate.coinbaseValue,
                                                     m_config.miningAddress);
    candidate.transactionData.push_back(coinbaseTx);

    // Calculate merkle root
    std::vector<std::string> allTxHashes;
    allTxHashes.push_back(Dinero::Common::double_sha256(coinbaseTx)); // Coinbase hash
    allTxHashes.insert(allTxHashes.end(), candidate.transactions.begin(), candidate.transactions.end());
    std::string merkleRootHex = CalculateMerkleRoot(allTxHashes);

    // ✅ BUILD BLOCKHEADER (authoritative - Phase 3)
    candidate.header.version = 1;  // BlockHeader v1
    candidate.header.prev_block_hash = uint256::FromHexUnsafe(prevHashHex);
    candidate.header.merkle_root = uint256::FromHexUnsafe(merkleRootHex);
    candidate.header.timestamp = GetNextBlockTimestamp();
    candidate.header.difficulty = GetNextWorkRequiredWithChainDB(
        static_cast<int32_t>(candidate.height),
        static_cast<int64_t>(candidate.header.timestamp),
        GetConsensusForCurrentNetwork(),
        chain_db,
        m_context ? m_context->block_storage.get() : nullptr);
    if (candidate.header.difficulty == 0) {
        throw std::runtime_error("GBTWorkManager: failed to compute next difficulty bits");
    }
    candidate.header.nonce = 0;  // Miner will increment
    candidate.header.ZeroReserved();  // MUST be zero (consensus rule)

    // ═══════════════════════════════════════════════════════════════════════════
    // CRITICAL: Compute Utreexo root using the SAME oracle as validation
    // ═══════════════════════════════════════════════════════════════════════════
    // Utreexo is active from genesis (height 0). Any block built with zero root
    // will fail validation because computed root != header root.
    // Use ComputeUtreexoRootPure() - the single source of truth for mining & validation.
    // ═══════════════════════════════════════════════════════════════════════════

    // Try to get BlockValidator: first from explicit set, then from context
    dinero::consensus::BlockValidator* block_validator = m_blockValidator;
    if (!block_validator && m_context && m_context->chainstate) {
        auto chainstate_svc = std::dynamic_pointer_cast<dinero::ChainstateService>(m_context->chainstate);
        if (chainstate_svc) {
            block_validator = chainstate_svc->GetBlockValidator();
        }
    }

    if (dinero::consensus::FullRulesActive(static_cast<uint32_t>(candidate.height)) && block_validator) {
        // Build temporary block for oracle call
        Block temp_block;
        temp_block.header = candidate.header;
        // Note: For coinbase-only blocks, vtx is empty except coinbase
        // ComputeUtreexoRootPure will handle computing the AFTER-state root
        // by applying the block's UTXOs to a temporary forest snapshot

        // Call the SAME pure function used by validation (single source of truth)
        uint256 computed_root;
        std::string utreexo_error;
        if (block_validator->ComputeUtreexoRootPure(temp_block, static_cast<uint32_t>(candidate.height),
                                                    computed_root, utreexo_error)) {
            candidate.header.utreexo_root = computed_root;
            dinero::g_logger.debug("GBTWorkManager: Utreexo root computed via oracle: " +
                computed_root.GetHex().substr(0, 16) + "...");
        } else {
            dinero::g_logger.error("GBTWorkManager: ComputeUtreexoRootPure failed: " + utreexo_error);
            // Fall back to null root (will fail validation, but logs show why)
            candidate.header.utreexo_root.SetNull();
        }
    } else if (!block_validator) {
        dinero::g_logger.warning("GBTWorkManager: BlockValidator not available, using zero Utreexo root (blocks will fail validation)");
        candidate.header.utreexo_root = uint256();
    } else {
        // Pre-activation path (should not happen since Utreexo active from height 0)
        candidate.header.utreexo_root = uint256();
    }

    // Generate unique template ID
    candidate.templateId = GenerateTemplateId();

    return candidate;
}

std::vector<std::string> GBTWorkManager::SelectTransactions(uint64_t& totalFees, size_t& totalSize) {
    std::vector<std::string> selected;
    totalFees = 0;
    totalSize = 128; // Block header size (BlockHeader v1 - Phase 3)

    // Total timeout for transaction selection: if the entire selection takes
    // longer than 2 seconds, stop adding transactions and return what we have.
    // This prevents the block template RPC from hanging when the mempool
    // contains slow-to-validate transactions (e.g., consolidation txs with 30+ inputs).
    static constexpr auto SELECTION_TIMEOUT = std::chrono::milliseconds(2000);
    const auto selectionStart = std::chrono::steady_clock::now();

    // Get available transactions from mempool
    std::vector<std::string> mempoolTxs = GetMempoolTransactions(m_config.maxTransactions, totalFees);

    // Simple selection: take transactions until size limit
    for (const auto& txHash : mempoolTxs) {
        // Check total timeout before processing each transaction
        auto elapsed = std::chrono::steady_clock::now() - selectionStart;
        if (elapsed > SELECTION_TIMEOUT) {
            dinero::g_logger.warning("GBT SelectTransactions: total timeout (" +
                std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()) +
                "ms) reached after " + std::to_string(selected.size()) +
                " txs, skipping remaining " +
                std::to_string(mempoolTxs.size() - selected.size()) + " mempool txs");
            break;
        }

        size_t txSize = GetTransactionSize(txHash);

        if (totalSize + txSize > m_config.maxBlockSize) {
            break; // Would exceed block size limit
        }

        if (ValidateTransaction(txHash)) {
            selected.push_back(txHash);
            totalSize += txSize;

            if (m_config.includeFees) {
                totalFees += GetTransactionFee(txHash);
            }
        }

        if (selected.size() >= m_config.maxTransactions) {
            break; // Reached transaction limit
        }
    }

    return selected;
}

std::string GBTWorkManager::BuildCoinbaseTransaction(int64_t height, uint64_t reward, const std::string& address) {
    // Implement proper coinbase construction with BIP34 height commitment
    std::ostringstream coinbase;
    
    // Version (4 bytes, little-endian)
    coinbase << "\x01\x00\x00\x00";
    
    // Input count (1 byte)
    coinbase << "\x01";
    
    // Input: Previous output hash (32 bytes of zeros)
    coinbase << std::string(32, '\x00');
    
    // Input: Previous output index (4 bytes, 0xffffffff)
    coinbase << "\xff\xff\xff\xff";
    
    // Input: Script length (BIP34 height commitment)
    uint8_t script_length = 1 + 4; // height bytes + height commitment
    coinbase << static_cast<char>(script_length);
    
    // BIP34 height commitment (height in little-endian)
    coinbase << static_cast<char>(height & 0xff);
    if (height >= 0x100) coinbase << static_cast<char>((height >> 8) & 0xff);
    if (height >= 0x10000) coinbase << static_cast<char>((height >> 16) & 0xff);
    if (height >= 0x1000000) coinbase << static_cast<char>((height >> 24) & 0xff);
    
    // Input: Coinbase script (height + extra data)
    coinbase << static_cast<char>(height & 0xff);
    coinbase << static_cast<char>((height >> 8) & 0xff);
    coinbase << static_cast<char>((height >> 16) & 0xff);
    coinbase << static_cast<char>((height >> 24) & 0xff);
    coinbase << "DIN\x01"; // Extra nonce space
    
    // Input: Sequence (4 bytes, 0xffffffff)
    coinbase << "\xff\xff\xff\xff";
    
    // Output count (1 byte)
    coinbase << "\x01";
    
    // Output: Value (8 bytes, little-endian)
    for (int i = 0; i < 8; i++) {
        coinbase << static_cast<char>((reward >> (i * 8)) & 0xff);
    }
    
    // Output: Script length (25 bytes for P2WPKH)
    coinbase << "\x19"; // 25 bytes for P2WPKH
    
    // Output: Script (P2WPKH: OP_0 + 20-byte pubkey hash)
    coinbase << "\x00\x14"; // OP_0 + 20-byte push
    
    // Resolve mining payout address from the daemon payout resolver.
    std::string mining_address = dinero::GetMiningAddress();
    if (!mining_address.empty()) {
        // Decode the mining address to get the witness program
        std::vector<uint8_t> script;
        std::string error;
        if (dinero::BuildScriptPubKeyFromAddress(mining_address, script, error)) {
            // Extract the 20-byte witness program from P2WPKH script
            if (script.size() >= 22 && script[0] == 0x00 && script[1] == 0x14) {
                // Valid P2WPKH script, extract the 20-byte hash
                coinbase.write(reinterpret_cast<const char*>(script.data() + 2), 20);
            } else {
                // Fallback to zero hash if script format is unexpected
                coinbase << std::string(20, '\x00');
                dinero::g_logger.warning("Unexpected script format for mining address, using zero hash");
            }
        } else {
            // Fallback to zero hash if address decoding fails
            coinbase << std::string(20, '\x00');
            dinero::g_logger.warning("Failed to decode mining address: " + error + ", using zero hash");
        }
    } else {
        // No mining address set, use zero hash
        coinbase << std::string(20, '\x00');
        dinero::g_logger.warning("No mining address set, using zero hash");
    }
    
    // Lock time (4 bytes)
    coinbase << "\x00\x00\x00\x00";
    
    return coinbase.str();
}

std::string GBTWorkManager::CalculateMerkleRoot(const std::vector<std::string>& transactions) {
    if (transactions.empty()) {
        return std::string(32, '\x00'); // Empty merkle root
    }
    
    if (transactions.size() == 1) {
        return transactions[0]; // Single transaction is the root
    }
    
    std::vector<std::string> level = transactions;
    
    // Build merkle tree bottom-up
    while (level.size() > 1) {
        std::vector<std::string> nextLevel;
        
        for (size_t i = 0; i < level.size(); i += 2) {
            std::string left = level[i];
            std::string right = (i + 1 < level.size()) ? level[i + 1] : left; // Duplicate if odd
            
            // Combine and hash
            std::string combined = left + right;
            std::string hash = Dinero::Common::double_sha256(combined);
            nextLevel.push_back(hash);
        }
        
        level = nextLevel;
    }
    
    return level[0];
}

uint64_t GBTWorkManager::GetNextBlockTimestamp() {
    // Return a consensus-safe candidate timestamp:
    //   nTime = max(wall_clock, parent_mtp + 1), capped at wall_clock + 2h.
    auto now = std::chrono::system_clock::now();
    const int64_t current_time = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());
    const int64_t max_future_time = current_time + 7200;

    if (!m_context || !m_context->chainstate) {
        return static_cast<uint64_t>(current_time);
    }

    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(m_context->chainstate);
    if (!chainstate) {
        return static_cast<uint64_t>(current_time);
    }

    auto* chain_db = chainstate->chainDB();
    if (!chain_db) {
        return static_cast<uint64_t>(current_time);
    }

    const int64_t height = chainstate->getBlockHeight();
    const int64_t parent_mtp = (height <= 0)
        ? static_cast<int64_t>(dinero::Params().genesis.nTime)
        : dinero::GetMedianTimePastAtHeight(
              chain_db,
              m_context ? m_context->block_storage.get() : nullptr,
              static_cast<uint32_t>(height));

    int64_t candidate_time = current_time;
    if (parent_mtp > 0) {
        candidate_time = std::max<int64_t>(current_time, parent_mtp + 1);
    }
    if (candidate_time > max_future_time) {
        candidate_time = max_future_time;
    }

    return static_cast<uint64_t>(candidate_time);
}

bool GBTWorkManager::ReadBlockchainTip(int64_t& height, std::string& hash, uint32_t& bits) {
    try {
        // Week 3: Use context instead of global
        if (!m_context || !m_context->chainstate) {
            dinero::g_logger.error("GBTWorkManager: Context or chainstate not available");
            return false;
        }

        // Get chainstate service
        auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(m_context->chainstate);
        if (!chainstate) {
            dinero::g_logger.error("GBTWorkManager: Failed to cast chainstate service");
            return false;
        }

        auto* chain_db = chainstate->chainDB();
        if (!chain_db) {
            dinero::g_logger.error("GBTWorkManager: ChainDB unavailable");
            return false;
        }

        // Get current blockchain state via service
        height = chainstate->getBlockHeight();
        hash = chainstate->getBestBlockHash();

        if (height <= 0) {
            bits = dinero::Params().genesis.nBits;
        } else {
            auto hash_result = chain_db->getBlockHashByHeight(static_cast<uint32_t>(height));
            if (!hash_result.ok()) {
                dinero::g_logger.error("GBTWorkManager: Failed to get tip hash by height");
                return false;
            }

            auto header_result = chain_db->getHeader(hash_result.value());
            if (header_result.ok()) {
                bits = header_result.value().difficulty;
            } else {
                auto block_result = chainstate->getBlockByHash(hash_result.value());
                if (!block_result.ok()) {
                    dinero::g_logger.error("GBTWorkManager: Failed to load tip header/block");
                    return false;
                }
                bits = block_result.value().header.difficulty;
            }
        }

        dinero::g_logger.info("GBTWorkManager: Read blockchain tip - height: " + std::to_string(height) + ", hash: " + hash);

        return true;
    }
    catch (const std::exception& e) {
        dinero::g_logger.error("GBTWorkManager: Failed to read blockchain tip: " + std::string(e.what()));
        return false;
    }
}

std::vector<std::string> GBTWorkManager::GetMempoolTransactions(size_t maxCount, uint64_t& totalFees) {
    std::vector<std::string> transactions;
    totalFees = 0;

    try {
        if (!dinero::g_mempool) {
            dinero::g_logger.debug("Mempool query: mempool not initialized");
            return transactions;
        }

        auto txids = dinero::g_mempool->getTransactionIds();
        if (txids.empty()) {
            return transactions;
        }

        const size_t limit = std::min(maxCount, txids.size());
        transactions.reserve(limit);
        for (size_t i = 0; i < limit; ++i) {
            transactions.push_back(txids[i].GetHex());
        }
        dinero::g_logger.debug("Mempool query: " + std::to_string(transactions.size()) + " transactions");
    } catch (const std::exception& e) {
        dinero::g_logger.error("Mempool query failed: " + std::string(e.what()));
    }

    return transactions;
}

bool GBTWorkManager::ValidateTransaction(const std::string& txHash) {
    try {
        if (txHash.empty()) {
            dinero::g_logger.error("Transaction hash is empty");
            return false;
        }

        if (!dinero::g_mempool) {
            return false;
        }

        uint256 txid;
        if (!uint256::FromHex(txHash, txid)) {
            return false;
        }

        return dinero::g_mempool->hasTransaction(txid);
    } catch (const std::exception& e) {
        dinero::g_logger.error("Transaction validation failed: " + std::string(e.what()));
        return false;
    }
}

uint64_t GBTWorkManager::GetTransactionFee(const std::string& txHash) {
    try {
        if (!dinero::g_mempool) {
            return 0;
        }

        uint256 txid;
        if (!uint256::FromHex(txHash, txid)) {
            return 0;
        }

        auto fee_opt = dinero::g_mempool->getTransactionFee(txid);
        return fee_opt.has_value() ? *fee_opt : 0;
    } catch (const std::exception& e) {
        dinero::g_logger.error("Fee calculation failed: " + std::string(e.what()));
        return 0;
    }
}

size_t GBTWorkManager::GetTransactionSize(const std::string& txHash) {
    try {
        if (!dinero::g_mempool) {
            return 250;
        }

        uint256 txid;
        if (!uint256::FromHex(txHash, txid)) {
            return 250;
        }

        auto tx = dinero::g_mempool->getTransaction(txid);
        if (!tx) {
            return 250;
        }

        const std::string serialized = tx->Serialize();
        if (serialized.empty()) {
            return 250;
        }
        return serialized.size() / 2;
    } catch (const std::exception& e) {
        dinero::g_logger.error("Size calculation failed: " + std::string(e.what()));
        return 250; // Fallback to average size
    }
}

uint64_t GBTWorkManager::GenerateTemplateId() {
    return m_nextTemplateId.fetch_add(1);
}

bool GBTWorkManager::IsWorkStale(uint64_t templateId) {
    std::lock_guard<std::mutex> lock(m_workMutex);
    return !m_currentWork || m_currentWork->templateId != templateId || m_currentWork->stale.load();
}

void GBTWorkManager::CleanupStaleWork() {
    // Cleanup is handled automatically by shared_ptr when work is replaced
}
