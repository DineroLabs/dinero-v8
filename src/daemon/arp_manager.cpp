#include "daemon/arp_manager.h"
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <ctime>
#include <cmath>
#include <json/json.h>

namespace dinero {

ArpManager& ArpManager::instance() {
    static ArpManager instance;
    return instance;
}

ArpManager::~ArpManager() {
    stopAutoRefresh();
}

std::string ArpManager::nowUTC() {
    auto now = std::time(nullptr);
    std::tm tm = *std::gmtime(&now);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

bool ArpManager::loadFromConfig(const std::string& configPath) {
    std::lock_guard<std::mutex> lock(get_mutex());

    std::ifstream file(configPath);
    if (!file.is_open()) {
        std::cerr << "[ArpManager] Config not found: " << configPath << std::endl;
        std::cerr << "[ArpManager] Using default: $0.10 USD/DIN" << std::endl;

        // Set default values
        current_.price_usd = 0.10;
        current_.timestamp = nowUTC();
        current_.source = "default";
        current_.confidence = 0.0;

        return false;
    }

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errs;

    if (!Json::parseFromStream(builder, file, &root, &errs)) {
        std::cerr << "[ArpManager] JSON parse error: " << errs << std::endl;
        return false;
    }

    current_.price_usd = root.get("reference_price_usd", 0.10).asDouble();
    current_.timestamp = root.get("last_updated", nowUTC()).asString();
    current_.source = root.get("source", "config").asString();
    current_.confidence = root.get("confidence", 0.0).asDouble();

    std::cout << "[ArpManager] Loaded ARP: $" << current_.price_usd << " USD/DIN" << std::endl;
    std::cout << "[ArpManager] Source: " << current_.source << std::endl;
    std::cout << "[ArpManager] Confidence: " << (current_.confidence * 100) << "%" << std::endl;

    return true;
}

bool ArpManager::saveToConfig(const std::string& configPath) {
    std::lock_guard<std::mutex> lock(get_mutex());

    Json::Value root;
    root["reference_price_usd"] = current_.price_usd;
    root["last_updated"] = current_.timestamp;
    root["source"] = current_.source;
    root["confidence"] = current_.confidence;

    std::ofstream file(configPath);
    if (!file.is_open()) {
        std::cerr << "[ArpManager] Failed to save config: " << configPath << std::endl;
        return false;
    }

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";
    file << Json::writeString(builder, root);

    std::cout << "[ArpManager] Saved ARP config to " << configPath << std::endl;
    return true;
}

std::optional<ArpInfo> ArpManager::getCurrent() {
    std::lock_guard<std::mutex> lock(get_mutex());
    return current_;
}

std::optional<ArpInfo> ArpManager::getBlended(double marketRate, double confidence) {
    std::lock_guard<std::mutex> lock(get_mutex());

    // Clamp confidence to [0.0, 1.0]
    confidence = std::max(0.0, std::min(1.0, confidence));

    // Calculate weights
    double arpWeight = 1.0 - confidence;  // High early, decays to 0
    double liveWeight = confidence;        // Low early, grows to 1

    // Weighted average
    double blendedPrice = (arpWeight * current_.price_usd) + (liveWeight * marketRate);

    ArpInfo info;
    info.price_usd = blendedPrice;
    info.timestamp = nowUTC();
    info.confidence = confidence;

    // Determine source description
    if (confidence < 0.1) {
        info.source = "arp_only";
    } else if (confidence < 0.9) {
        std::ostringstream oss;
        oss << "blended_" << static_cast<int>(arpWeight * 100) << "arp_"
            << static_cast<int>(liveWeight * 100) << "market";
        info.source = oss.str();
    } else {
        info.source = "market_only";
    }

    std::cout << "[ArpManager] Blended: $" << blendedPrice << " USD/DIN" << std::endl;
    std::cout << "[ArpManager]   ARP ($" << current_.price_usd << ") × "
              << static_cast<int>(arpWeight * 100) << "%" << std::endl;
    std::cout << "[ArpManager]   Market ($" << marketRate << ") × "
              << static_cast<int>(liveWeight * 100) << "%" << std::endl;
    std::cout << "[ArpManager]   Source: " << info.source << std::endl;

    return info;
}

void ArpManager::setPrice(double priceUsd, const std::string& source) {
    std::lock_guard<std::mutex> lock(get_mutex());

    current_.price_usd = priceUsd;
    current_.timestamp = nowUTC();
    current_.source = source;

    std::cout << "[ArpManager] Updated ARP: $" << priceUsd << " USD/DIN ("
              << source << ")" << std::endl;
}

double ArpManager::calculateConfidence(int daysSinceListing) {
    // Confidence growth curve:
    // Day 0: 0.0 (100% ARP)
    // Day 1: 0.3 (70% ARP, 30% Market)
    // Day 5: 0.6 (40% ARP, 60% Market)
    // Day 14+: 1.0 (100% Market)

    if (daysSinceListing < 0) return 0.0;
    if (daysSinceListing >= 14) return 1.0;

    // Sigmoid-like growth: confidence = 1 / (1 + e^(-0.5 * (days - 7)))
    double x = daysSinceListing - 7.0;
    double confidence = 1.0 / (1.0 + std::exp(-0.5 * x));

    return confidence;
}

double ArpManager::fetchFromBridgeAverage() {
    // Week 7: Bridge rate averaging implementation placeholder
    // This would query multiple providers (Coinbase, Binance, LiFi, etc.) and return a weighted average
    // 
    // Implementation requirements:
    // 1. HTTP client for API calls (curl, libcurl, or similar)
    // 2. API keys/authentication for each provider
    // 3. Rate limiting and error handling
    // 4. Weighted averaging algorithm (by volume, reliability, etc.)
    // 5. Caching to avoid excessive API calls
    //
    // For now, return 0.0 to indicate no market data available
    // The system will continue using config-based ARP values
    
    std::cout << "[ArpManager] fetchFromBridgeAverage() - Bridge rate averaging requires external API integration" << std::endl;
    std::cout << "[ArpManager] Using config-based ARP until bridge integration is implemented" << std::endl;
    return 0.0;  // Return 0 to indicate no market data available
}

void ArpManager::startAutoRefresh(unsigned intervalSeconds) {
    stopRefresh_ = false;

    refreshThread_ = std::thread([this, intervalSeconds]() {
        std::cout << "[ArpManager] Auto-refresh started (interval: "
                  << intervalSeconds << "s)" << std::endl;

        while (!stopRefresh_) {
            std::this_thread::sleep_for(std::chrono::seconds(intervalSeconds));

            if (stopRefresh_) break;

            std::cout << "[ArpManager] Running auto-refresh..." << std::endl;

            double marketRate = fetchFromBridgeAverage();
            if (marketRate > 0) {
                // Market data available - update with blended approach
                auto arp = getCurrent();
                if (arp) {
                    // Increase confidence over time (simple example)
                    double newConfidence = std::min(1.0, arp->confidence + 0.1);
                    auto blended = getBlended(marketRate, newConfidence);

                    if (blended) {
                        setPrice(blended->price_usd, "auto_blended");

                        std::lock_guard<std::mutex> lock(get_mutex());
                        current_.confidence = newConfidence;

                        saveToConfig("config/arp.json");
                    }
                }
            } else {
                std::cout << "[ArpManager] No market data - keeping current ARP" << std::endl;
            }
        }

        std::cout << "[ArpManager] Auto-refresh stopped" << std::endl;
    });
}

void ArpManager::stopAutoRefresh() {
    if (refreshThread_.joinable()) {
        stopRefresh_ = true;
        refreshThread_.join();
    }
}

} // namespace dinero
