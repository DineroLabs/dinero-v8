#include "daemon/mining_safety_gates.h"
#include "daemon/db_meta_utils.hpp"
#include "wallet/wallet_manager.h"
#include "common/logger.h"
#include "storage/chain_direct.h"      // For GetChainHeight()
#include "daemon/services/p2p_service.h"  // Week 4: P2PService access via context
#include "consensus/chainparams.h"      // For Params()
#include "consensus/chainwork.h"        // For arith_uint256 and CompareChainwork
#include "daemon/daemon_context.h"       // Week 3: DaemonContext instead of globals
#include "daemon/services/chainstate_service.h"  // Week 3: ChainstateService access
#include "daemon/services/wallet_service.h"      // Week 3: WalletService access
#include <fstream>
#include <sstream>
#include <cmath>
#include <algorithm>

#if defined(__APPLE__) && !defined(IOS_BUILD)
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/ps/IOPSKeys.h>
#include <sys/sysctl.h>
#define DINERO_HAS_MACOS_POWER_APIS 1
#endif

#ifdef _WIN32
#include <windows.h>
#include <powrprof.h>
#include <winbase.h>
#endif

#ifdef __linux__
#include <unistd.h>
#include <sys/stat.h>
#endif

// Week 3: Static context pointer (initialized by MiningService)
DaemonContext* MiningSafetyGates::ctx_ = nullptr;

MiningSafetyGates::SafetyResult MiningSafetyGates::ValidateMiningSafety(
    const std::string& miningAddress,
    const std::string& network,
    bool enableLocalMining,
    bool userUnderstands
) {
    SafetyResult result;
    
    // Check all safety conditions
    result.sync = CheckSyncStatus();
    result.thermal = CheckThermalStatus();
    result.battery = CheckBatteryStatus();
    result.address = ValidateMiningAddress(miningAddress);
    
    // Determine if mining can start
    result.canStartMining = true;
    std::vector<std::string> blockingReasons;
    
    // Sync validation (critical)
    if (!result.sync.fullysynced) {
        result.canStartMining = false;
        if (result.sync.isIBD) {
            blockingReasons.push_back("Initial blockchain download in progress");
        } else if (result.sync.blocksBehind > MAX_BLOCKS_BEHIND) {
            blockingReasons.push_back("Node is " + std::to_string(result.sync.blocksBehind) + " blocks behind network");
        }
    }

    // Phase 43: Safe Mode validation (critical)
    // Block mining during deep reorgs or other dangerous chain conditions
    if (ctx_ && ctx_->chainstate) {
        auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx_->chainstate);
        if (chainstate && chainstate->IsInSafeMode()) {
            result.canStartMining = false;
            blockingReasons.push_back("SAFE MODE: " + chainstate->GetSafeModeReason());
        }
    }

    // Peer count validation (critical for mainnet)
    // Require at least 2 peers to prevent solo mining on isolated network
    // Week 4: Migrated from dinero::legacy::g_peer_manager() global to ctx_->p2p->get()
    if (ctx_ && ctx_->p2p) {
        auto& p2p_mgr = ctx_->p2p->get();
        if (p2p_mgr.is_running()) {
            size_t peerCount = p2p_mgr.get_peer_count();

            // For mainnet, require minimum 2 peers to avoid mining on self-chain
            if (network == "mainnet" && peerCount < 2) {
                result.canStartMining = false;
                blockingReasons.push_back("Insufficient peers: " + std::to_string(peerCount) + " connected (minimum 2 required for mainnet)");
            }

            // For testnet/regtest, warn but don't block if no peers
            if (network != "mainnet" && peerCount == 0) {
                result.warnings.push_back("Warning: No peers connected - mining in isolated mode");
            }
        }
    }

    // Chainwork validation (critical for mainnet)
    // Prevent mining on chains with insufficient proof-of-work
    // Week 3: MIGRATED - Now uses ctx_->chainstate instead of dinero::legacy::g_chain_db_direct()
    if (network == "mainnet") {
        if (ctx_ && ctx_->chainstate) {
            // Phase 39: Get ChainDB via ChainstateService (ChainManager deleted)
            auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx_->chainstate);
            auto* chain_db = chainstate ? chainstate->GetChainDB() : nullptr;
            if (chain_db) {
                try {
                    // Get current chain tip
                    auto tip_result = chain_db->getTip();
                    if (tip_result.status() == dinero::Status::Ok) {
                        const auto& tip = tip_result.value();

                        // Get minimum chainwork from params
                        const auto& params = dinero::Params();
                        if (!params.nMinimumChainWork.empty()) {
                            // Phase M.0: Inline conversion at presentation boundary
                            int cmp = dinero::CompareChainwork(tip.work.GetHex(), params.nMinimumChainWork);
                            if (cmp < 0) {
                                result.canStartMining = false;
                                blockingReasons.push_back("Insufficient chainwork (current chain has inadequate proof-of-work)");
                            }
                        }
                    }
                } catch (const std::exception& e) {
                    // If we can't check chainwork, warn but don't block (for now)
                    result.warnings.push_back("Could not verify chainwork: " + std::string(e.what()));
                }
            }
        }
    }

    // Address validation (critical)
    if (!result.address.isValid) {
        result.canStartMining = false;
        blockingReasons.push_back("Invalid mining address: " + result.address.validationError);
    } else if (!result.address.isWalletAddress) {
        result.warnings.push_back("Mining address does not belong to this wallet");
    } else if (result.address.isWatchOnly) {
        result.warnings.push_back("Wallet is watch-only - mining rewards cannot be spent");
    }

    // Thermal validation (critical)
    if (!result.thermal.safeToMine) {
        result.canStartMining = false;
        blockingReasons.push_back("Thermal protection: " + result.thermal.thermalReason);
    }

    // Battery validation (warning only, not blocking)
    if (!result.battery.safeToMine) {
        result.warnings.push_back("Battery protection: " + result.battery.batteryReason);
    }

    // Network-specific validation - MINING ENABLED ON ALL NETWORKS
    // No network restrictions - users can mine on mainnet, testnet, and regtest
    // However, mainnet requires peer connectivity (checked above) to prevent self-chain mining
    
    // Set primary blocking reason
    if (!blockingReasons.empty()) {
        result.blockingReason = blockingReasons[0];
    }
    
    return result;
}

MiningSafetyGates::SyncStatus MiningSafetyGates::CheckSyncStatus() {
    SyncStatus status;

    try {
        // Get current blockchain height from ChainDB
        // Week 3: MIGRATED - Now uses ctx_->chainstate instead of dinero::legacy::g_chain_db_direct()
        if (!ctx_ || !ctx_->chainstate) {
            // Chainstate not initialized yet - assume we're in IBD
            status.isIBD = true;
            status.fullysynced = false;
            status.blocksBehind = 999999;
            status.currentHeight = 0;
            status.networkHeight = 0;
            return status;
        }

        // Phase 39: Get ChainDB via ChainstateService (ChainManager deleted)
        auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx_->chainstate);
        auto* chain_db = chainstate ? chainstate->GetChainDB() : nullptr;
        if (!chain_db) {
            status.isIBD = true;
            status.fullysynced = false;
            status.blocksBehind = 999999;
            status.currentHeight = 0;
            status.networkHeight = 0;
            return status;
        }

        status.currentHeight = dinero::storage::GetChainHeight(chain_db);

        // Get network height from connected peers
        // Week 4: Migrated from dinero::legacy::g_peer_manager() global to ctx_->p2p->get()
        if (!ctx_ || !ctx_->p2p) {
            // P2P service not available - can't determine network height
            // If we have blocks, assume we're synced (regtest mode)
            status.networkHeight = status.currentHeight;
            status.blocksBehind = 0;
            status.isIBD = false;
            status.fullysynced = true;
            return status;
        }

        auto& p2p_mgr = ctx_->p2p->get();
        if (!p2p_mgr.is_running()) {
            // P2P not running - can't determine network height
            // If we have blocks, assume we're synced (regtest mode)
            status.networkHeight = status.currentHeight;
            status.blocksBehind = 0;
            status.isIBD = false;
            status.fullysynced = true;
            return status;
        }

        // Get peer information
        auto peers = p2p_mgr.get_connected_peers();
        if (peers.empty()) {
            // No peers connected - might be regtest or isolated
            // Allow mining if we have any blocks (regtest scenario)
            status.networkHeight = status.currentHeight;
            status.blocksBehind = 0;
            status.isIBD = (status.currentHeight == 0);
            status.fullysynced = !status.isIBD;
            return status;
        }

        // Estimate network height from peers
        uint32_t maxPeerHeight = 0;
        for (const auto& peer : peers) {
            maxPeerHeight = std::max(maxPeerHeight, std::max(peer.best_known_height, peer.synced_headers));
        }
        status.networkHeight = maxPeerHeight;

        // Calculate blocks behind
        if (status.networkHeight > status.currentHeight) {
            status.blocksBehind = status.networkHeight - status.currentHeight;
        } else {
            status.blocksBehind = 0;
        }

        // Determine IBD status
        // IBD if we're significantly behind (more than 144 blocks = ~1 day for 10min blocks)
        status.isIBD = (status.blocksBehind > 144);

        // Fully synced if within MAX_BLOCKS_BEHIND and not in IBD
        status.fullysynced = (status.blocksBehind <= MAX_BLOCKS_BEHIND && !status.isIBD);

    } catch (const std::exception& e) {
        // If we can't check sync status, assume we're not synced
        status.isIBD = true;
        status.fullysynced = false;
        status.blocksBehind = 999999;  // Unknown, assume very behind
        status.currentHeight = 0;
        status.networkHeight = 0;
    }

    return status;
}

MiningSafetyGates::ThermalStatus MiningSafetyGates::CheckThermalStatus() {
    ThermalStatus status;
    
    try {
#ifdef DINERO_HAS_MACOS_POWER_APIS
        // macOS thermal monitoring via sysctl
        int temp = 0;
        size_t size = sizeof(temp);
        
        // Try to get CPU temperature (may require different approach on different Macs)
        if (sysctlbyname("machdep.xcpm.cpu_thermal_level", &temp, &size, nullptr, 0) == 0) {
            // Convert thermal level to approximate temperature
            status.cpuTemp = 40.0 + (temp * 10.0);  // Rough approximation
        } else {
            // Fallback: assume safe temperature if we can't read it
            status.cpuTemp = 50.0;
        }
        
#elif defined(_WIN32)
        // Windows thermal monitoring (basic implementation)
        // In a full implementation, this would use WMI queries
        status.cpuTemp = 50.0;  // Conservative safe temperature assumption
        status.safeToMine = true; // Assume thermal conditions are acceptable
        
#elif defined(__linux__)
        // Linux thermal monitoring via /sys/class/thermal
        std::ifstream tempFile("/sys/class/thermal/thermal_zone0/temp");
        if (tempFile.is_open()) {
            int temp_millicelsius;
            tempFile >> temp_millicelsius;
            status.cpuTemp = temp_millicelsius / 1000.0;
        } else {
            status.cpuTemp = 50.0;  // Assume safe if can't read
        }
        
#else
        status.cpuTemp = 50.0;  // Unknown platform, assume safe
#endif
        
        // Check thermal limits
        status.thermalThrottling = (status.cpuTemp > MAX_CPU_TEMP);
        status.safeToMine = (status.cpuTemp < MAX_CPU_TEMP);
        
        if (!status.safeToMine) {
            status.thermalReason = "CPU temperature too high (" + 
                                 std::to_string(static_cast<int>(status.cpuTemp)) + "°C > " +
                                 std::to_string(static_cast<int>(MAX_CPU_TEMP)) + "°C)";
        }
        
    } catch (const std::exception& e) {
        // If thermal monitoring fails, err on the side of caution
        status.cpuTemp = MAX_CPU_TEMP + 10;  // Assume hot
        status.safeToMine = false;
        status.thermalReason = "Unable to monitor CPU temperature";
    }
    
    return status;
}

MiningSafetyGates::BatteryStatus MiningSafetyGates::CheckBatteryStatus() {
    BatteryStatus status;
    
    try {
#ifdef DINERO_HAS_MACOS_POWER_APIS
        // macOS battery monitoring via IOKit
        CFTypeRef powerInfo = IOPSCopyPowerSourcesInfo();
        if (powerInfo) {
            CFArrayRef powerSources = IOPSCopyPowerSourcesList(powerInfo);
            if (powerSources && CFArrayGetCount(powerSources) > 0) {
                CFDictionaryRef powerSource = (CFDictionaryRef)CFArrayGetValueAtIndex(powerSources, 0);
                
                // Check power source type
                CFStringRef powerSourceType = (CFStringRef)CFDictionaryGetValue(powerSource, CFSTR(kIOPSPowerSourceStateKey));
                if (powerSourceType) {
                    CFStringRef batteryPower = CFSTR(kIOPSBatteryPowerValue);
                    status.onBattery = CFStringCompare(powerSourceType, batteryPower, 0) == kCFCompareEqualTo;
                }
                
                // Get battery percentage
                CFNumberRef batteryLevel = (CFNumberRef)CFDictionaryGetValue(powerSource, CFSTR(kIOPSCurrentCapacityKey));
                if (batteryLevel) {
                    CFNumberGetValue(batteryLevel, kCFNumberIntType, &status.batteryPercent);
                }
                
                CFRelease(powerSources);
            }
            CFRelease(powerInfo);
        }
        
#elif defined(_WIN32)
        // Windows battery monitoring
        SYSTEM_POWER_STATUS powerStatus;
        if (GetSystemPowerStatus(&powerStatus)) {
            status.onBattery = (powerStatus.ACLineStatus == 0);  // 0 = offline (on battery)
            status.batteryPercent = powerStatus.BatteryLifePercent;
            if (status.batteryPercent == 255) status.batteryPercent = 100;  // Unknown = assume full
        }
        
#elif defined(__linux__)
        // Linux battery monitoring via /sys/class/power_supply
        std::ifstream acOnline("/sys/class/power_supply/ADP1/online");  // Common AC adapter name
        if (acOnline.is_open()) {
            int online;
            acOnline >> online;
            status.onBattery = (online == 0);
        }
        
        std::ifstream batteryCapacity("/sys/class/power_supply/BAT0/capacity");  // Common battery name
        if (batteryCapacity.is_open()) {
            batteryCapacity >> status.batteryPercent;
        }
        
#else
        // Unknown platform - assume AC power for safety
        status.onBattery = false;
        status.batteryPercent = 100;
#endif
        
        // Determine battery safety
        status.lowBattery = (status.batteryPercent < MIN_BATTERY_PERCENT);
        status.safeToMine = !status.onBattery || (!status.lowBattery);
        
        if (!status.safeToMine) {
            if (status.onBattery && status.lowBattery) {
                status.batteryReason = "Low battery (" + std::to_string(status.batteryPercent) + "% < " + 
                                     std::to_string(MIN_BATTERY_PERCENT) + "%)";
            } else if (status.onBattery) {
                status.batteryReason = "Running on battery power";
            }
        }
        
    } catch (const std::exception& e) {
        // If battery monitoring fails, assume AC power for safety
        status.onBattery = false;
        status.batteryPercent = 100;
        status.safeToMine = true;
    }
    
    return status;
}

MiningSafetyGates::AddressValidation MiningSafetyGates::ValidateMiningAddress(const std::string& address) {
    AddressValidation validation;
    validation.address = address;
    
    try {
        // Basic format validation
        if (address.empty()) {
            validation.validationError = "Empty address";
            return validation;
        }
        
        // Check if it looks like a valid bech32 address for Dinero
        // Accept both "din" (mainnet) and "rdin" (regtest) prefixes
        if (address.length() < 10 || (address.substr(0, 3) != "din" && address.substr(0, 4) != "rdin")) {
            validation.validationError = "Invalid address format (should start with 'din' or 'rdin')";
            return validation;
        }
        
        // Basic bech32-style address validation
        // Full bech32 validation would decode and verify the checksum
        if (address.length() < 42 || address.length() > 62) {
            validation.validationError = "Invalid address length";
            return validation;
        }
        
        validation.isValid = true;
        
        // Check if address belongs to our wallet
        // Week 3: MIGRATED - Now uses ctx_->wallet instead of dinero::legacy::g_wallet_manager()
        if (ctx_ && ctx_->wallet) {
            auto& wallet = ctx_->wallet->get();
            if (wallet.hasActiveWallet()) {
                validation.isWalletAddress = wallet.isAddressMine(address);
                
                // Check if wallet is watch-only (encrypted and locked)
                // A watch-only wallet is one that is encrypted but locked (can't spend)
                validation.isWatchOnly = wallet.isWalletEncrypted() && wallet.isWalletLocked();
                validation.canReceive = true;
            } else {
                // No active wallet
                validation.isWalletAddress = false;
                validation.isWatchOnly = false;
                validation.canReceive = true;  // External addresses can still receive
            }
        } else {
            // No wallet service available
            validation.isWalletAddress = false;
            validation.isWatchOnly = false;
            validation.canReceive = true;  // External addresses can still receive
        }
        
    } catch (const std::exception& e) {
        validation.validationError = "Address validation failed: " + std::string(e.what());
    }
    
    return validation;
}

bool MiningSafetyGates::ShouldPauseMining() {
    // Check if mining should be paused due to runtime conditions

    // Phase 43: Pause if in safe mode (deep reorg protection)
    if (ctx_ && ctx_->chainstate) {
        auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx_->chainstate);
        if (chainstate && chainstate->IsInSafeMode()) {
            return true; // CRITICAL: Pause mining during dangerous chain conditions
        }
    }

    // No peers = no mining (Bitcoin rule: isolated mining wastes energy, creates orphans)
    if (ctx_ && ctx_->p2p) {
        auto& p2p_mgr = ctx_->p2p->get();
        if (p2p_mgr.is_running() && p2p_mgr.get_peer_count() == 0) {
            return true;
        }
    }

    auto thermal = CheckThermalStatus();
    auto battery = CheckBatteryStatus();

    // Pause if thermal limits exceeded
    if (!thermal.safeToMine) {
        return true;
    }

    // Pause if on battery and low power
    if (battery.onBattery && battery.lowBattery) {
        return true;
    }

    return false;
}

std::string MiningSafetyGates::GetPauseReason() {
    // Phase 43: Check safe mode first (highest priority)
    if (ctx_ && ctx_->chainstate) {
        auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx_->chainstate);
        if (chainstate && chainstate->IsInSafeMode()) {
            return "Safe mode: " + chainstate->GetSafeModeReason();
        }
    }

    // No peers = no mining
    if (ctx_ && ctx_->p2p) {
        auto& p2p_mgr = ctx_->p2p->get();
        if (p2p_mgr.is_running() && p2p_mgr.get_peer_count() == 0) {
            return "no_peers";
        }
    }

    auto thermal = CheckThermalStatus();
    auto battery = CheckBatteryStatus();
    
    if (!thermal.safeToMine) {
        return "thermal";
    }
    
    if (battery.onBattery && battery.lowBattery) {
        return "on_battery";
    }
    
    return "";  // No pause needed
}

// Coinbase maturity implementation
CoinbaseMaturity::MaturityInfo CoinbaseMaturity::GetMaturityInfo(uint32_t coinbaseHeight) {
    MaturityInfo info;
    info.blockHeight = coinbaseHeight;
    info.requiredConfirmations = COINBASE_MATURITY;
    
    // Get current blockchain height from blockchain interface
    uint32_t currentHeight = coinbaseHeight;  // Would be obtained from blockchain interface
    
    info.confirmations = (currentHeight >= coinbaseHeight) ? 
                        (currentHeight - coinbaseHeight + 1) : 0;
    
    info.isMatured = (info.confirmations >= COINBASE_MATURITY);
    info.blocksRemaining = info.isMatured ? 0 : 
                          (COINBASE_MATURITY - info.confirmations);
    
    info.timeEstimate = EstimateMaturityTime(info.blocksRemaining);
    
    return info;
}

std::vector<CoinbaseMaturity::MaturityInfo> CoinbaseMaturity::GetAllImmatureCoinbases() {
    std::vector<MaturityInfo> immatureCoinbases;
    
    // Query database for all coinbase transactions with < 100 confirmations
    // In a real implementation, this would query the transaction database
    
    return immatureCoinbases;
}

std::string CoinbaseMaturity::EstimateMaturityTime(int blocksRemaining) {
    if (blocksRemaining <= 0) {
        return "Matured";
    }
    
    // 5 minute block times (300 seconds) - Litecoin-style
    const int BLOCK_TIME_SECONDS = 300;
    int totalSeconds = blocksRemaining * BLOCK_TIME_SECONDS;
    
    if (totalSeconds < 3600) {
        // Less than 1 hour
        int minutes = totalSeconds / 60;
        return "~" + std::to_string(minutes) + " minutes";
    } else if (totalSeconds < 86400) {
        // Less than 1 day
        int hours = totalSeconds / 3600;
        int minutes = (totalSeconds % 3600) / 60;
        return "~" + std::to_string(hours) + "h " + std::to_string(minutes) + "m";
    } else {
        // 1 day or more
        int days = totalSeconds / 86400;
        int hours = (totalSeconds % 86400) / 3600;
        return "~" + std::to_string(days) + " days " + std::to_string(hours) + "h";
    }
}
