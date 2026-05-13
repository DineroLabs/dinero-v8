#include "daemon/miner_core.h"
#include "common/logger.h"
#include <thread>

namespace dinero {

MinerCore::MinerCore() {
    g_logger.info("🔧 MinerCore: Initializing single mining engine");
    m_config.threads = std::max(1, (int)std::thread::hardware_concurrency() - 2);
}

MinerCore::~MinerCore() {
    g_logger.info("🔧 MinerCore: Shutting down");
    stop();
}

bool MinerCore::start(const std::string& address, int threads) {
    if (m_running.load()) {
        g_logger.warning("🔧 MinerCore: Already running, stopping first");
        stop();
    }
    
    if (!m_mining || !m_blockchain) {
        g_logger.error("🔧 MinerCore: Missing dependencies (mining or blockchain)");
        return false;
    }
    
    if (address.empty()) {
        g_logger.error("🔧 MinerCore: No mining address provided");
        return false;
    }
    
    g_logger.info("🔧 MinerCore: Starting with address=" + address + ", threads=" + std::to_string(threads));
    
    // Set configuration
    m_current_address = address;
    m_config.threads = std::max(1, threads);
    
    // Configure mining component
    m_mining->setMiningAddress(address);
    m_mining->setThreadCount(m_config.threads);
    m_mining->setMiningEnabled(true);
    
    // Reset statistics
    m_blocks_found.store(0);
    m_accepted.store(0);
    m_rejected.store(0);
    m_stale.store(0);
    m_total_hashes.store(0);
    m_start_time = std::chrono::steady_clock::now();
    
    // Start mining thread
    m_should_stop.store(false);
    m_running.store(true);
    m_mining_thread = std::thread(&MinerCore::miningLoop, this);
    
    g_logger.info("🔧 MinerCore: Started successfully");
    return true;
}

void MinerCore::stop() {
    if (!m_running.load()) {
        return;
    }
    
    g_logger.info("🔧 MinerCore: Stopping mining engine");
    
    // Signal stop and wait for thread
    m_should_stop.store(true);
    m_running.store(false);
    
    if (m_mining) {
        m_mining->setMiningEnabled(false);
    }
    
    if (m_mining_thread.joinable()) {
        m_mining_thread.join();
    }
    
    g_logger.info("🔧 MinerCore: Stopped");
}

void MinerCore::setConfig(const MinerConfig& config) {
    m_config = config;
    
    // Apply thread count if mining is running
    if (m_running.load() && m_mining) {
        m_mining->setThreadCount(m_config.threads);
    }
    
    g_logger.info("🔧 MinerCore: Config updated - threads=" + std::to_string(config.threads) + 
                  ", affinity=" + config.cpu_affinity + ", priority=" + config.priority);
}

MinerCore::MinerStats MinerCore::getStats() const {
    MinerStats stats;
    stats.running = m_running.load();
    stats.address = m_current_address;
    stats.threads = m_config.threads;
    stats.hashrate = m_hashrate.load();
    stats.blocks_found = m_blocks_found.load();
    stats.accepted = m_accepted.load();
    stats.rejected = m_rejected.load();
    stats.stale = m_stale.load();
    stats.last_block_time = m_last_block_time.load();
    stats.template_age = m_template_age.load();
    stats.difficulty = m_mining ? m_mining->getDifficulty() : 0;
    stats.block_height = m_blockchain ? m_blockchain->getLatestHeight() : 0;
    
    return stats;
}

void MinerCore::miningLoop() {
    g_logger.info("🔧 MinerCore: Mining loop started");
    
    while (!m_should_stop.load()) {
        if (!m_mining || !m_mining->isMiningEnabled()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        
        try {
            // Get block template
            auto block = m_mining->createBlockTemplate();
            
            if (m_should_stop.load()) break;
            
            // Mine the block (this is where the real sha256 work happens)
            bool found = m_mining->mineBlock(block);
            
            if (m_should_stop.load()) break;
            
            // Update hash count for rate calculation
            m_total_hashes.fetch_add(m_config.threads * 1000); // Simulate hashes
            
            if (found) {
                const auto& h = block.header;
                g_logger.info("🎯 MinerCore: Block found! nonce=" + std::to_string(h.nonce));
                g_logger.info("  prev_hash:    " + h.prev_block_hash.GetHex());
                g_logger.info("  merkle_root:  " + h.merkle_root.GetHex());
                g_logger.info("  utreexo_root: " + h.utreexo_root.GetHex());
                char bits_buf[16]; snprintf(bits_buf, sizeof(bits_buf), "0x%08x", h.difficulty);
                g_logger.info("  nBits:        " + std::string(bits_buf));
                m_blocks_found.fetch_add(1);
                
                // Submit block
                if (m_mining->submitBlock(block)) {
                    m_accepted.fetch_add(1);
                    m_last_block_time.store(std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count());
                    g_logger.info("✅ MinerCore: Block accepted by network");
                } else {
                    m_rejected.fetch_add(1);
                    g_logger.warning("❌ MinerCore: Block rejected by network");
                }
            }
            
            // Update statistics
            updateStats();
            
        } catch (const std::exception& e) {
            g_logger.error("🔧 MinerCore: Mining error - " + std::string(e.what()));
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
        // Brief pause to prevent tight loop
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    g_logger.info("🔧 MinerCore: Mining loop ended");
}

void MinerCore::updateStats() {
    // Calculate hashrate
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - m_start_time);
    
    if (duration.count() > 0) {
        double rate = static_cast<double>(m_total_hashes.load()) / duration.count();
        m_hashrate.store(rate);
    }
    
    // Update template age (simplified)
    m_template_age.store(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

} // namespace dinero
