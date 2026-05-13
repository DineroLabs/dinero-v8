#pragma once
#include <memory>
#include "wallet/tx_builder_iface.h"
#include "wallet/wallet_iface.h"

// Forward declarations
namespace dinero {
  class TxMempool;
  class UTXOView;
  class WalletManager;
  class ChainDB;
}

namespace din {

struct ExecutionContext {
  ITxBuilder* tx_builder{nullptr}; // or std::shared_ptr<ITxBuilder>
  IKeyStore* key_store{nullptr};   // Real keystore for PSBT signing

  // Phase 2A: Core services (eliminate globals)
  dinero::TxMempool* mempool{nullptr};
  dinero::UTXOView* utxo_view{nullptr};

  // Phase 3A: Wallet manager (eliminate g_wallet_manager global)
  dinero::WalletManager* wallet_manager{nullptr};

  // Phase 4B: Chain database (for RPC layer)
  dinero::ChainDB* chain_db{nullptr};

  // Future subsystems can be added here:
  // IChainView* chain_view{nullptr};
  // IFeeEstimator* fee_estimator{nullptr};
  // IWalletDB* wallet_db{nullptr};

  // Validation
  bool hasTxBuilder() const { return tx_builder != nullptr; }
  bool hasKeyStore() const { return key_store != nullptr; }
  bool hasMempool() const { return mempool != nullptr; }
  bool hasUTXOView() const { return utxo_view != nullptr; }
  bool hasWalletManager() const { return wallet_manager != nullptr; }
  bool hasChainDB() const { return chain_db != nullptr; }
};

} // namespace din
