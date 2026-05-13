#include "unified_miner/miner_manager.h"
#include "common/utils.h"
#include <iostream>
#include <sstream>
#include <chrono>
#include <iomanip>

using namespace Dinero::UnifiedMiner;
using namespace Dinero::Common;

MinerManager::MinerManager() : verbose_mode(false) {
    // Initialize RPC client
    RPCConfig config;
    config.url = "http://127.0.0.1:8332";
    config.user = "dinero_Dinero_USB_1754372740";
    config.pass = "5431cfe6e2b435637ec89dc2a85324c3";
    
    rpc_client = std::make_unique<Common::RPCClient>(config);
    
    // Initialize database manager
    db_manager = std::make_unique<Common::RocksDBManager>();
}

MinerManager::~MinerManager() {
    stopAllMiners();
}

void MinerManager::setVerbose(bool verbose) {
    verbose_mode = verbose;
}

bool MinerManager::isVerbose() const {
    return verbose_mode.load();
}

std::string MinerManager::generateMinerId(const std::string& type) {
    auto timestamp = TimeUtils::getCurrentTimestamp();
    auto hostname = NetworkUtils::getHostname();
    return type + "_" + hostname + "_" + std::to_string(timestamp);
}

void MinerManager::updateMinerStatus(const std::string& id, MinerStatus status) {
    std::lock_guard<std::mutex> lock(miners_mutex);
    if (miners.find(id) != miners.end()) {
        miners[id]->status = status;
        if (verbose_mode) {
            std::cout << "🔄 Miner " << id << " status: " << getMinerStatusString(status) << std::endl;
        }
    }
}

void MinerManager::logMinerEvent(const std::string& id, const std::string& event) {
    if (verbose_mode) {
        std::cout << "📝 [" << id << "] " << event << std::endl;
    }
}

bool MinerManager::isMinerRunning(const std::string& id) {
    std::lock_guard<std::mutex> lock(miners_mutex);
    auto it = miners.find(id);
    return it != miners.end() && it->second->status == MinerStatus::RUNNING;
}

void MinerManager::startGuiMiner(int port, const std::string& config_path) {
    std::string id = generateMinerId("gui");
    
    std::lock_guard<std::mutex> lock(miners_mutex);
    auto miner = std::make_shared<MinerInfo>();
    miner->type = "gui";
    miner->id = id;
    miner->status = MinerStatus::STARTING;
    miner->address = "";
    miner->threads = 0;
    miner->hash_rate = 0.0;
    miner->total_hashes = 0;
    miner->blocks_found = 0;
    miner->start_time = TimeUtils::getCurrentTimestamp();
    miner->should_stop = false;
    
    miners[id] = miner;
    
    // Start GUI miner in separate thread
    miner->worker_thread = std::thread([this, miner, port, config_path]() {
        try {
            updateMinerStatus(miner->id, MinerStatus::RUNNING);
            logMinerEvent(miner->id, "GUI miner started on port " + std::to_string(port));
            
            // TODO: Implement actual GUI miner logic
            // For now, just simulate running
            while (!miner->should_stop) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            
            updateMinerStatus(miner->id, MinerStatus::STOPPED);
            logMinerEvent(miner->id, "GUI miner stopped");
        } catch (const std::exception& e) {
            miner->last_error = e.what();
            updateMinerStatus(miner->id, MinerStatus::ERROR);
            logMinerEvent(miner->id, "GUI miner error: " + std::string(e.what()));
        }
    });
    
    std::cout << "🎨 GUI Miner started with ID: " << id << std::endl;
    std::cout << "🌐 Access GUI at: http://localhost:" << port << std::endl;
}

void MinerManager::startEmbeddedMiner(int threads, const std::string& address, 
                                     const std::string& rpc_url, const std::string& rpc_user, 
                                     const std::string& rpc_pass, const std::string& db_path,
                                     bool benchmark_mode, int benchmark_duration, 
                                     const std::string& miner_id) {
    std::string id = miner_id.empty() ? generateMinerId("embedded") : miner_id;
    
    std::lock_guard<std::mutex> lock(miners_mutex);
    auto miner = std::make_shared<MinerInfo>();
    miner->type = "embedded";
    miner->id = id;
    miner->status = MinerStatus::STARTING;
    miner->address = address;
    miner->threads = threads;
    miner->hash_rate = 0.0;
    miner->total_hashes = 0;
    miner->blocks_found = 0;
    miner->start_time = TimeUtils::getCurrentTimestamp();
    miner->should_stop = false;
    
    miners[id] = miner;
    
    // Start embedded miner in separate thread
    miner->worker_thread = std::thread([this, miner, threads, address, rpc_url, rpc_user, rpc_pass, db_path, benchmark_mode, benchmark_duration]() {
        try {
            updateMinerStatus(miner->id, MinerStatus::RUNNING);
            logMinerEvent(miner->id, "Embedded miner started with " + std::to_string(threads) + " threads");
            
            // Initialize database
            if (!db_path.empty()) {
                db_manager->initDatabase(db_path, false, miner->id);
            }
            
            // TODO: Implement actual embedded miner logic
            // For now, just simulate mining
            uint64_t hashes = 0;
            auto start_time = std::chrono::steady_clock::now();
            
            while (!miner->should_stop) {
                // Simulate mining work
                hashes += threads * 1000; // Simulate hash rate
                miner->total_hashes = hashes;
                
                // Calculate hash rate
                auto now = std::chrono::steady_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
                if (duration > 0) {
                    miner->hash_rate = static_cast<double>(hashes) / duration;
                }
                
                // Send heartbeat
                if (db_manager) {
                    db_manager->sendHeartbeat(NetworkUtils::getLocalIP(), threads, 0);
                }
                
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            
            updateMinerStatus(miner->id, MinerStatus::STOPPED);
            logMinerEvent(miner->id, "Embedded miner stopped");
        } catch (const std::exception& e) {
            miner->last_error = e.what();
            updateMinerStatus(miner->id, MinerStatus::ERROR);
            logMinerEvent(miner->id, "Embedded miner error: " + std::string(e.what()));
        }
    });
    
    std::cout << "⚡ Embedded Miner started with ID: " << id << std::endl;
    std::cout << "🎯 Mining address: " << address << std::endl;
    std::cout << "🧵 Threads: " << threads << std::endl;
}

void MinerManager::startLightweightMiner(int threads, const std::string& address,
                                        const std::string& rpc_url, const std::string& rpc_user, 
                                        const std::string& rpc_pass) {
    std::string id = generateMinerId("lightweight");
    
    std::lock_guard<std::mutex> lock(miners_mutex);
    auto miner = std::make_shared<MinerInfo>();
    miner->type = "lightweight";
    miner->id = id;
    miner->status = MinerStatus::STARTING;
    miner->address = address;
    miner->threads = threads;
    miner->hash_rate = 0.0;
    miner->total_hashes = 0;
    miner->blocks_found = 0;
    miner->start_time = TimeUtils::getCurrentTimestamp();
    miner->should_stop = false;
    
    miners[id] = miner;
    
    // Start lightweight miner in separate thread
    miner->worker_thread = std::thread([this, miner, threads, address, rpc_url, rpc_user, rpc_pass]() {
        try {
            updateMinerStatus(miner->id, MinerStatus::RUNNING);
            logMinerEvent(miner->id, "Lightweight miner started with " + std::to_string(threads) + " threads");
            
            // TODO: Implement actual lightweight miner logic
            // For now, just simulate mining
            uint64_t hashes = 0;
            auto start_time = std::chrono::steady_clock::now();
            
            while (!miner->should_stop) {
                // Simulate mining work
                hashes += threads * 500; // Simulate hash rate
                miner->total_hashes = hashes;
                
                // Calculate hash rate
                auto now = std::chrono::steady_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
                if (duration > 0) {
                    miner->hash_rate = static_cast<double>(hashes) / duration;
                }
                
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            
            updateMinerStatus(miner->id, MinerStatus::STOPPED);
            logMinerEvent(miner->id, "Lightweight miner stopped");
        } catch (const std::exception& e) {
            miner->last_error = e.what();
            updateMinerStatus(miner->id, MinerStatus::ERROR);
            logMinerEvent(miner->id, "Lightweight miner error: " + std::string(e.what()));
        }
    });
    
    std::cout << "🚀 Lightweight Miner started with ID: " << id << std::endl;
    std::cout << "🎯 Mining address: " << address << std::endl;
    std::cout << "🧵 Threads: " << threads << std::endl;
}

void MinerManager::stopMiner(const std::string& miner_type) {
    std::lock_guard<std::mutex> lock(miners_mutex);
    
    for (auto& pair : miners) {
        auto& miner = pair.second;
        if (miner_type == "all" || miner->type == miner_type) {
            if (miner->status == MinerStatus::RUNNING || miner->status == MinerStatus::STARTING) {
                miner->should_stop = true;
                updateMinerStatus(miner->id, MinerStatus::STOPPING);
                logMinerEvent(miner->id, "Stopping miner...");
            }
        }
    }
    
    // Wait for threads to finish
    for (auto& pair : miners) {
        auto& miner = pair.second;
        if (miner_type == "all" || miner->type == miner_type) {
            if (miner->worker_thread.joinable()) {
                miner->worker_thread.join();
            }
        }
    }
    
    std::cout << "🛑 Stopped " << miner_type << " miner(s)" << std::endl;
}

void MinerManager::stopAllMiners() {
    stopMiner("all");
}

void MinerManager::showStatus() {
    std::lock_guard<std::mutex> lock(miners_mutex);
    
    if (miners.empty()) {
        std::cout << "📊 No miners running" << std::endl;
        return;
    }
    
    std::cout << "\n📊 Miner Status:" << std::endl;
    std::cout << std::string(50, '-') << std::endl;
    
    for (const auto& pair : miners) {
        const auto& miner = pair.second;
        std::string status_emoji;
        
        switch (miner->status) {
            case MinerStatus::RUNNING: status_emoji = "🟢"; break;
            case MinerStatus::STARTING: status_emoji = "🟡"; break;
            case MinerStatus::STOPPING: status_emoji = "🟠"; break;
            case MinerStatus::STOPPED: status_emoji = "⚪"; break;
            case MinerStatus::ERROR: status_emoji = "🔴"; break;
        }
        
        std::cout << status_emoji << " " << miner->type << " [" << getMinerStatusString(miner->status) << "]";
        if (!miner->address.empty()) {
            std::cout << " - " << miner->address;
        }
        if (miner->threads > 0) {
            std::cout << " (" << miner->threads << " threads)";
        }
        if (miner->hash_rate > 0) {
            std::cout << " - " << std::fixed << std::setprecision(2) << miner->hash_rate << " H/s";
        }
        std::cout << std::endl;
        
        if (!miner->last_error.empty()) {
            std::cout << "   ❌ Error: " << miner->last_error << std::endl;
        }
    }
    
    std::cout << std::string(50, '-') << std::endl;
    std::cout << "Total miners: " << miners.size() << " | Running: " << getRunningMinerCount() << std::endl;
}

Json::Value MinerManager::getStatusJson() {
    Json::Value status;
    std::lock_guard<std::mutex> lock(miners_mutex);
    
    status["total_miners"] = static_cast<int>(miners.size());
    status["running_miners"] = getRunningMinerCount();
    status["timestamp"] = static_cast<uint64_t>(TimeUtils::getCurrentTimestamp());
    
    Json::Value miners_array;
    for (const auto& pair : miners) {
        const auto& miner = pair.second;
        Json::Value miner_info;
        miner_info["id"] = miner->id;
        miner_info["type"] = miner->type;
        miner_info["status"] = getMinerStatusString(miner->status);
        miner_info["address"] = miner->address;
        miner_info["threads"] = miner->threads;
        miner_info["hash_rate"] = miner->hash_rate;
        miner_info["total_hashes"] = static_cast<uint64_t>(miner->total_hashes);
        miner_info["blocks_found"] = miner->blocks_found;
        miner_info["start_time"] = static_cast<uint64_t>(miner->start_time);
        miner_info["last_error"] = miner->last_error;
        
        miners_array.append(miner_info);
    }
    
    status["miners"] = miners_array;
    return status;
}

std::vector<std::string> MinerManager::getRunningMiners() {
    std::vector<std::string> running;
    std::lock_guard<std::mutex> lock(miners_mutex);
    
    for (const auto& pair : miners) {
        if (pair.second->status == MinerStatus::RUNNING) {
            running.append(pair.first);
        }
    }
    
    return running;
}

void MinerManager::startDashboard(int port, bool auto_refresh, int refresh_interval) {
    std::cout << "🎛️ Starting Dashboard on port " << port << std::endl;
    std::cout << "🌐 Access dashboard at: http://localhost:" << port << std::endl;
    
    // TODO: Implement actual dashboard web server
    // For now, just show status
    while (true) {
        showStatus();
        if (!auto_refresh) break;
        std::this_thread::sleep_for(std::chrono::seconds(refresh_interval));
    }
}

bool MinerManager::isAnyMinerRunning() const {
    std::lock_guard<std::mutex> lock(miners_mutex);
    for (const auto& pair : miners) {
        if (pair.second->status == MinerStatus::RUNNING) {
            return true;
        }
    }
    return false;
}

int MinerManager::getRunningMinerCount() const {
    int count = 0;
    std::lock_guard<std::mutex> lock(miners_mutex);
    for (const auto& pair : miners) {
        if (pair.second->status == MinerStatus::RUNNING) {
            count++;
        }
    }
    return count;
}

std::string MinerManager::getMinerStatusString(MinerStatus status) const {
    switch (status) {
        case MinerStatus::STOPPED: return "Stopped";
        case MinerStatus::STARTING: return "Starting";
        case MinerStatus::RUNNING: return "Running";
        case MinerStatus::STOPPING: return "Stopping";
        case MinerStatus::ERROR: return "Error";
        default: return "Unknown";
    }
} 