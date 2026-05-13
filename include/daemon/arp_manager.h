#pragma once

#include <string>
#include <optional>
#include <mutex>
#include <thread>
#include <chrono>

namespace dinero {

/**
 * ArpInfo - Anchor Reference Price data structure
 *
 * Represents the current reference price for DIN in USD,
 * used as a soft price anchor during early market phase.
 */
struct ArpInfo {
    double price_usd;           // Reference price (e.g., 0.10 USD/DIN)
    std::string timestamp;      // ISO 8601 timestamp of last update
    std::string source;         // Source: "manual", "blended", "market"
    double confidence;          // Market confidence 0.0-1.0 (0=pure ARP, 1=pure market)
};

/**
 * ArpManager - Dynamic Anchor Reference Price system
 *
 * Features:
 * - Loads initial ARP from config/arp.json
 * - Blends ARP with market data using weighted average
 * - Smooth transition from ARP → Market as confidence grows
 * - Auto-refresh capability for periodic updates
 * - Thread-safe singleton pattern
 *
 * Economic Strategy:
 * - Pre-launch: 100% ARP ($0.10 USD/DIN)
 * - Day 1: 70% ARP + 30% Market
 * - Week 1: 40% ARP + 60% Market
 * - Week 2+: 100% Market (confidence = 1.0)
 */
class ArpManager {
public:
    static ArpManager& instance();

    // Load ARP from config file
    bool loadFromConfig(const std::string& configPath = "config/arp.json");

    // Save current ARP to config
    bool saveToConfig(const std::string& configPath = "config/arp.json");

    // Get current static ARP
    std::optional<ArpInfo> getCurrent();

    // Get blended price (ARP + market data)
    std::optional<ArpInfo> getBlended(double marketRate, double confidence);

    // Manually set ARP value
    void setPrice(double priceUsd, const std::string& source = "manual");

    // Auto-refresh loop (runs in background thread)
    void startAutoRefresh(unsigned intervalSeconds = 86400);
    void stopAutoRefresh();

    // Calculate confidence based on market age
    static double calculateConfidence(int daysSinceListing);

private:
    ArpManager() = default;
    ~ArpManager();

    std::string nowUTC();
    double fetchFromBridgeAverage();

    ArpInfo current_;
    // Use function-local static for mutex to avoid static initialization order issues
    static std::mutex& get_mutex() {
        static std::mutex mtx;
        return mtx;
    }

    std::thread refreshThread_;
    bool stopRefresh_ = false;
};

} // namespace dinero
