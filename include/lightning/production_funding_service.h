#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// Production Funding Service (L1→L2 Adapter)
// ═══════════════════════════════════════════════════════════════════════════
// Wraps funding transaction creation logic to implement IFundingService interface.
//
// ARCHITECTURE:
// - Extracts funding TX logic from ChannelManager::openChannel()
// - Provides clean interface for L2 core to request funding transactions
// - Handles UTXO selection, transaction building, and signing
//
// USAGE:
//   auto funding_service = std::make_shared<ProductionFundingService>(
//       wallet_api, daemon_ctx, node_pubkey
//   );
//   auto core = std::make_unique<ChannelManagerCore>(
//       chain_oracle, wallet_oracle, funding_service, db
//   );
// ═══════════════════════════════════════════════════════════════════════════

#include "lightning/funding_service.h"
#include "wallet/dinero_wallet_api.h"
#include <vector>
#include <cstdint>

// Forward declarations
struct DaemonContext;

namespace dinero {
namespace lightning {

/**
 * Production implementation of IFundingService.
 * Creates and signs Lightning channel funding transactions.
 */
class ProductionFundingService : public ::lightning::IFundingService {
public:
    /**
     * Constructor
     * @param wallet_api Pointer to IWalletAPI implementation (NOT owned)
     * @param daemon_ctx Reference to DaemonContext (NOT owned)
     * @param node_pubkey Local node public key (33 bytes compressed)
     */
    ProductionFundingService(
        wallet::IWalletAPI* wallet_api,
        DaemonContext& daemon_ctx,
        const std::vector<uint8_t>& node_pubkey
    );

    ~ProductionFundingService() override = default;

    // Disable copy and move
    ProductionFundingService(const ProductionFundingService&) = delete;
    ProductionFundingService& operator=(const ProductionFundingService&) = delete;
    ProductionFundingService(ProductionFundingService&&) = delete;
    ProductionFundingService& operator=(ProductionFundingService&&) = delete;

    // ═══════════════════════════════════════════════════════════════════════
    // IFundingService Implementation
    // ═══════════════════════════════════════════════════════════════════════

    std::optional<::lightning::FundingTxResult> createFunding(
        uint64_t amount_sats,
        const std::string& remote_pubkey,
        const std::string& local_pubkey,
        uint32_t csv_delay,
        uint64_t feerate_sat_per_kvb
    ) override;

    bool broadcastFunding(const std::string& tx_hex) override;

    uint64_t estimateFundingFee(uint32_t target_blocks = 6) const override;

private:
    wallet::IWalletAPI* m_wallet_api;  // NOT owned (injected dependency)
    DaemonContext& m_daemon_ctx;       // NOT owned (injected dependency)
    std::vector<uint8_t> m_node_pubkey;  // Local node public key (33 bytes)
};

} // namespace lightning
} // namespace dinero
