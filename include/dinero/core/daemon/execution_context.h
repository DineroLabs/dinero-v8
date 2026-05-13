#pragma once
#include <memory>
#include "wallet/tx_builder_iface.h"
#include "wallet/wallet_iface.h"

namespace din {

struct ExecutionContext {
  ITxBuilder* tx_builder{nullptr}; // or std::shared_ptr<ITxBuilder>
  IKeyStore* key_store{nullptr};   // Real keystore for PSBT signing
  
  // Future subsystems can be added here:
  // IChainView* chain_view{nullptr};
  // IFeeEstimator* fee_estimator{nullptr};
  // IWalletDB* wallet_db{nullptr};
  
  // Validation
  bool hasTxBuilder() const { return tx_builder != nullptr; }
  bool hasKeyStore() const { return key_store != nullptr; }
};

} // namespace din
