#pragma once

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║                    WALLET UTXO ADAPTER                                     ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║                                                                           ║
// ║  Bridges IUTXOProvider interface to wallet::UTXOIndex                     ║
// ║                                                                           ║
// ║  ARCHITECTURE: Wallet derives from consensus (correct direction)          ║
// ║    - Consensus defines canonical UTXO model (OutPoint + UTXOEntry)        ║
// ║    - Wallet extends with ownership info (derivation path, etc.)           ║
// ║    - This adapter converts between the two                                ║
// ║                                                                           ║
// ║  NOTE: This adapter includes wallet headers because it's a BRIDGE.        ║
// ║  Consensus code should use ConsensusUTXOAdapter, not this.                ║
// ║                                                                           ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

#include "consensus/interfaces/iutxo_provider.h"
#include "wallet/utxo_index.h"  // Wallet layer (this adapter is the bridge)

namespace dinero {
namespace consensus {

/**
 * WalletUTXOAdapter - Bridges IUTXOProvider to wallet UTXOIndex
 *
 * PURPOSE: Allow legacy code using UTXOIndex to work with IUTXOProvider interface
 *
 * USAGE:
 *   UTXOIndex wallet_utxos("wallet.db");
 *   WalletUTXOAdapter adapter(&wallet_utxos);
 *   // Use adapter via IUTXOProvider interface
 *
 * NOTE: For new consensus code, prefer ConsensusUTXOAdapter with consensus::UTXOSet.
 * This adapter is primarily for wallet operations that need ownership tracking.
 */
class WalletUTXOAdapter : public IUTXOProvider {
public:
    /**
     * Construct adapter wrapping a wallet UTXO index
     *
     * @param wallet_index The wallet UTXO index to wrap (must outlive adapter)
     */
    explicit WalletUTXOAdapter(UTXOIndex* wallet_index);

    ~WalletUTXOAdapter() override = default;

    // Disable copy
    WalletUTXOAdapter(const WalletUTXOAdapter&) = delete;
    WalletUTXOAdapter& operator=(const WalletUTXOAdapter&) = delete;

    // ═══════════════════════════════════════════════════════════════════════
    // IUTXOProvider interface implementation
    // Converts between consensus types and wallet types
    // ═══════════════════════════════════════════════════════════════════════

    std::optional<UTXOEntry> GetUTXO(const OutPoint& outpoint) const override;
    bool AddUTXO(const OutPoint& outpoint, const UTXOEntry& entry) override;
    bool SpendUTXO(const OutPoint& outpoint, uint32_t spend_height) override;
    bool DeleteUTXO(const OutPoint& outpoint) override;
    bool HasUTXO(const OutPoint& outpoint) const override;

    // ═══════════════════════════════════════════════════════════════════════
    // Wallet-specific methods (NOT part of IUTXOProvider)
    // These require wallet layer types
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Check if a script belongs to this wallet
     *
     * @param scriptPubKey The script to check
     * @return Derivation path if owned, nullopt if not
     */
    std::optional<std::string> IsOurScript(const std::vector<uint8_t>& scriptPubKey) const;

    /**
     * Get all unspent UTXOs (wallet enumeration)
     *
     * WARNING: Expensive for large UTXO sets
     */
    std::vector<WalletUTXO> GetUnspentUTXOs() const;

private:
    UTXOIndex* wallet_index_;  // Wrapped wallet UTXO index (not owned)

    // Type conversion helpers
    static UTXOEntry ToUTXOEntry(const WalletUTXO& utxo);
    static WalletUTXO ToWalletUTXO(const OutPoint& outpoint, const UTXOEntry& entry);
};

} // namespace consensus
} // namespace dinero
