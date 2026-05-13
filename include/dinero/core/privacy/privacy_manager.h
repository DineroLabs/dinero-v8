#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <map>

namespace din::privacy {

struct PrivacyConfig {
  // Anti-merge/cluster controls
  bool avoid_cluster_merging{true};
  bool prefer_single_input{true};
  bool consolidate_under_load{false};
  
  // Fresh change & output randomization
  bool always_fresh_change{true};
  bool randomize_output_position{true};
  bool stagger_amounts{true};
  int64_t stagger_tolerance_sats{100}; // Within fee tolerance
  
  // Locktime & anti-fee-sniping
  bool enable_locktime{true};
  bool anti_fee_sniping{true};
  int32_t locktime_height_offset{11}; // BIP-113 compatible
  
  // Address reuse prevention
  bool hard_refuse_reuse{true};
  bool warn_on_reuse{true};
  bool auto_derive_on_reuse{true};
  
  // Privacy-aware coin selection
  bool cluster_weighting{true};
  bool reject_toxic_change{true};
  int64_t toxic_change_threshold_sats{1000};
  
  // Labeling & metadata minimization
  bool enable_sp_labeling{true};
  bool never_log_derivation_paths{true};
  
  // PSBT policies
  bool enforce_sighash_all{true};
  bool forbid_anyonecanpay{true};
};

class PrivacyManager {
public:
  explicit PrivacyManager(const PrivacyConfig& config);
  
  // Anti-clustering controls
  bool shouldAvoidClusterMerge(const std::vector<std::string>& input_clusters) const;
  bool shouldPreferSingleInput(int64_t amount, int64_t fee_rate) const;
  bool shouldConsolidateUnderLoad(int64_t utxo_count, int64_t fee_rate) const;
  
  // Change management
  bool shouldUseFreshChange() const;
  size_t randomizeOutputPosition(size_t output_count) const;
  int64_t staggerAmount(int64_t base_amount, size_t output_index) const;
  
  // Locktime management
  int32_t computeLocktime(int32_t current_height) const;
  bool shouldUseAntiFeeSniping() const;
  
  // Address reuse prevention
  bool shouldRefuseReuse(const std::string& address) const;
  bool shouldWarnOnReuse(const std::string& address) const;
  bool shouldAutoDeriveOnReuse() const;
  
  // Coin selection
  bool shouldUseClusterWeighting() const;
  bool shouldRejectToxicChange(int64_t change_amount) const;
  
  // PSBT policies
  bool shouldEnforceSighashAll() const;
  bool shouldForbidAnyoneCanPay() const;
  
  // Configuration
  const PrivacyConfig& getConfig() const { return config_; }
  void updateConfig(const PrivacyConfig& new_config) { config_ = new_config; }

private:
  PrivacyConfig config_;
  
  // Internal state for tracking
  std::map<std::string, int> address_usage_count_;
  std::vector<std::string> recent_addresses_;
  
  // Helper methods
  bool isAddressRecentlyUsed(const std::string& address) const;
  void recordAddressUsage(const std::string& address);
};

} // namespace din::privacy
