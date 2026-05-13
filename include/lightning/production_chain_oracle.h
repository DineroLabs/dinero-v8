#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// Production Chain Oracle (L1→L2 Adapter)
// ═══════════════════════════════════════════════════════════════════════════
// Wraps DaemonContext to implement IChainOracle interface.
//
// ARCHITECTURE:
// - Adapter pattern: Existing L1 service → Oracle interface
// - Enables ChannelManagerCore to query blockchain without daemon dependencies
// - Provides access to chainstate and mempool through abstraction
//
// USAGE:
//   auto chain_oracle = std::make_shared<ProductionChainOracle>(daemon_ctx);
//   auto core = std::make_unique<ChannelManagerCore>(
//       chain_oracle, wallet_oracle, funding_service, db
//   );
// ═══════════════════════════════════════════════════════════════════════════

#include "lightning/chain_oracle.h"

// Forward declarations to avoid heavy includes
struct DaemonContext;

namespace dinero {
namespace lightning {

/**
 * Production implementation of IChainOracle.
 * Wraps DaemonContext to provide blockchain queries to ChannelManagerCore.
 */
class ProductionChainOracle : public ::lightning::IChainOracle {
public:
    /**
     * Constructor
     * @param daemon_ctx Reference to DaemonContext (NOT owned)
     */
    explicit ProductionChainOracle(DaemonContext& daemon_ctx);

    ~ProductionChainOracle() override = default;

    // Disable copy and move
    ProductionChainOracle(const ProductionChainOracle&) = delete;
    ProductionChainOracle& operator=(const ProductionChainOracle&) = delete;
    ProductionChainOracle(ProductionChainOracle&&) = delete;
    ProductionChainOracle& operator=(ProductionChainOracle&&) = delete;

    // ═══════════════════════════════════════════════════════════════════════
    // IChainOracle Implementation
    // ═══════════════════════════════════════════════════════════════════════

    uint64_t getBlockHeight() const override;

    std::optional<std::string> getBlockHash(uint64_t height) const override;

    bool isUnspent(const std::string& txid, uint32_t vout) const override;

    bool broadcastTransaction(const std::string& tx_hex) override;

    bool isInMempool(const std::string& txid) const override;

    std::optional<std::string> getTransaction(const std::string& txid) const override;

    std::optional<uint64_t> getTransactionHeight(const std::string& txid) const override;

private:
    DaemonContext& m_daemon_ctx;  // NOT owned (injected dependency)
};

} // namespace lightning
} // namespace dinero
