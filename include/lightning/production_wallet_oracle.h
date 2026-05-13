#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// Production Wallet Oracle (L1→L2 Adapter)
// ═══════════════════════════════════════════════════════════════════════════
// Wraps IWalletAPI to implement IWalletOracle interface.
//
// ARCHITECTURE:
// - Adapter pattern: Existing L1 service → Oracle interface
// - Enables ChannelManagerCore to use production wallet without L1 dependencies
// - Converts between proto types (dinerod::UTXO) and oracle types (IWalletOracle::UTXO)
//
// USAGE:
//   auto wallet_oracle = std::make_shared<ProductionWalletOracle>(wallet_api);
//   auto core = std::make_unique<ChannelManagerCore>(
//       chain_oracle, wallet_oracle, funding_service, db
//   );
// ═══════════════════════════════════════════════════════════════════════════

#include "lightning/wallet_oracle.h"
#include "wallet/dinero_wallet_api.h"

namespace dinero {
namespace lightning {

/**
 * Production implementation of IWalletOracle.
 * Wraps IWalletAPI to provide wallet queries to ChannelManagerCore.
 */
class ProductionWalletOracle : public ::lightning::IWalletOracle {
public:
    /**
     * Constructor
     * @param wallet_api Pointer to IWalletAPI implementation (NOT owned)
     */
    explicit ProductionWalletOracle(wallet::IWalletAPI* wallet_api);

    ~ProductionWalletOracle() override = default;

    // Disable copy and move
    ProductionWalletOracle(const ProductionWalletOracle&) = delete;
    ProductionWalletOracle& operator=(const ProductionWalletOracle&) = delete;
    ProductionWalletOracle(ProductionWalletOracle&&) = delete;
    ProductionWalletOracle& operator=(ProductionWalletOracle&&) = delete;

    // ═══════════════════════════════════════════════════════════════════════
    // IWalletOracle Implementation
    // ═══════════════════════════════════════════════════════════════════════

    bool isAvailable() const override;

    uint64_t getBalance() const override;

    uint64_t getConfirmedBalance() const override;

    std::vector<UTXO> listUTXOs(int min_confirmations = 1) const override;

    std::optional<UTXO> getUTXO(const std::string& txid, uint32_t vout) const override;

    std::vector<uint8_t> getRevocationBasepointSecret(
        const std::string& channel_id
    ) const override;

private:
    wallet::IWalletAPI* m_wallet_api;  // NOT owned (injected dependency)

    /**
     * Convert dinerod::UTXO (proto type) to IWalletOracle::UTXO (oracle type)
     * @param proto_utxo UTXO from IWalletAPI (proto format)
     * @return UTXO in oracle format
     */
    static UTXO convertProtoToOracle(const dinerod::UTXO& proto_utxo);
};

} // namespace lightning
} // namespace dinero
