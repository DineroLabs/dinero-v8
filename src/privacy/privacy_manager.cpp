#include "privacy/privacy_manager.h"
#include <random>
#include <algorithm>
#include <chrono>
#include <set>

namespace din::privacy {

PrivacyManager::PrivacyManager(const PrivacyConfig& config) : config_(config) {}

// Anti-clustering controls
bool PrivacyManager::shouldAvoidClusterMerge(const std::vector<std::string>& input_clusters) const {
  if (!config_.avoid_cluster_merging) return false;
  
  // Check if inputs come from different clusters
  std::set<std::string> unique_clusters(input_clusters.begin(), input_clusters.end());
  return unique_clusters.size() > 1;
}

bool PrivacyManager::shouldPreferSingleInput(int64_t amount, int64_t fee_rate) const {
  if (!config_.prefer_single_input) return false;
  
  // Prefer single input for small amounts or high fee rates
  return amount < 100000 || fee_rate > 50; // 0.001 DIN or 50 sat/vB
}

bool PrivacyManager::shouldConsolidateUnderLoad(int64_t utxo_count, int64_t fee_rate) const {
  if (!config_.consolidate_under_load) return false;
  
  // Consolidate when we have many UTXOs and low fee rates
  return utxo_count > 50 && fee_rate < 10; // 50+ UTXOs and <10 sat/vB
}

// Change management
bool PrivacyManager::shouldUseFreshChange() const {
  return config_.always_fresh_change;
}

size_t PrivacyManager::randomizeOutputPosition(size_t output_count) const {
  if (!config_.randomize_output_position || output_count <= 1) {
    return output_count - 1; // Last position
  }
  
  static std::random_device rd;
  static std::mt19937 gen(rd());
  std::uniform_int_distribution<size_t> dis(0, output_count - 1);
  
  return dis(gen);
}

int64_t PrivacyManager::staggerAmount(int64_t base_amount, size_t output_index) const {
  if (!config_.stagger_amounts) return base_amount;
  
  // Add small random variation within tolerance
  static std::random_device rd;
  static std::mt19937 gen(rd());
  std::uniform_int_distribution<int64_t> dis(
    -config_.stagger_tolerance_sats, 
    config_.stagger_tolerance_sats
  );
  
  return base_amount + dis(gen);
}

// Locktime management
int32_t PrivacyManager::computeLocktime(int32_t current_height) const {
  if (!config_.enable_locktime) return 0;
  
  return current_height + config_.locktime_height_offset;
}

bool PrivacyManager::shouldUseAntiFeeSniping() const {
  return config_.anti_fee_sniping;
}

// Address reuse prevention
bool PrivacyManager::shouldRefuseReuse(const std::string& address) const {
  if (!config_.hard_refuse_reuse) return false;
  
  return isAddressRecentlyUsed(address);
}

bool PrivacyManager::shouldWarnOnReuse(const std::string& address) const {
  if (!config_.warn_on_reuse) return false;
  
  return isAddressRecentlyUsed(address);
}

bool PrivacyManager::shouldAutoDeriveOnReuse() const {
  return config_.auto_derive_on_reuse;
}

// Coin selection
bool PrivacyManager::shouldUseClusterWeighting() const {
  return config_.cluster_weighting;
}

bool PrivacyManager::shouldRejectToxicChange(int64_t change_amount) const {
  if (!config_.reject_toxic_change) return false;
  
  return change_amount < config_.toxic_change_threshold_sats;
}

// PSBT policies
bool PrivacyManager::shouldEnforceSighashAll() const {
  return config_.enforce_sighash_all;
}

bool PrivacyManager::shouldForbidAnyoneCanPay() const {
  return config_.forbid_anyonecanpay;
}

// Helper methods
bool PrivacyManager::isAddressRecentlyUsed(const std::string& address) const {
  return std::find(recent_addresses_.begin(), recent_addresses_.end(), address) != recent_addresses_.end();
}

void PrivacyManager::recordAddressUsage(const std::string& address) {
  // Add to recent addresses (keep last 100)
  recent_addresses_.push_back(address);
  if (recent_addresses_.size() > 100) {
    recent_addresses_.erase(recent_addresses_.begin());
  }
  
  // Increment usage count
  address_usage_count_[address]++;
}

} // namespace din::privacy
