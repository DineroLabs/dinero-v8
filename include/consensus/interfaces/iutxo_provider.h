#pragma once

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║                    CONSENSUS LAYER BOUNDARY                                ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║                                                                           ║
// ║  This interface defines the UTXO operations needed by consensus code.     ║
// ║                                                                           ║
// ║  CRITICAL INVARIANTS:                                                     ║
// ║    - NO wallet includes allowed                                           ║
// ║    - NO wallet types (WalletUTXO, etc.)                                   ║
// ║    - ONLY consensus primitives (OutPoint, UTXOEntry)                      ║
// ║                                                                           ║
// ║  Direction of trust: Consensus → Wallet (never reverse)                   ║
// ║    - Consensus defines canonical UTXO model                               ║
// ║    - Wallet derives its view from consensus                               ║
// ║                                                                           ║
// ║  This ensures:                                                            ║
// ║    - Consensus compiles without wallet code                               ║
// ║    - Wallet can be rebuilt from consensus                                 ║
// ║    - Mining is stateless (no wallet dependency)                           ║
// ║    - Mobile/light clients are architecturally possible                    ║
// ║                                                                           ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

#include "consensus/outpoint.h"     // OutPoint (txid + vout)
#include "consensus/utxo_entry.h"   // UTXOEntry (value, script, height, coinbase)
#include <optional>

namespace dinero {
namespace consensus {

/**
 * IUTXOProvider - Abstract interface for UTXO set operations
 *
 * PURPOSE: Provide consensus layer with UTXO access without wallet dependency
 *
 * This interface uses ONLY consensus types:
 *   - OutPoint: txid + vout (identifies a UTXO)
 *   - UTXOEntry: value + scriptPubKey + height + coinbase flag
 *
 * NO wallet types (WalletUTXO, derivation paths, address strings, etc.)
 *
 * Implementations:
 *   - ConsensusUTXOAdapter: Wraps consensus::UTXOSet (production)
 *   - WalletUTXOAdapter: Wraps UTXOIndex (legacy, for wallet operations)
 *   - MockUTXOProvider: For testing (no DB required)
 */
class IUTXOProvider {
public:
    virtual ~IUTXOProvider() = default;

    /**
     * Get a UTXO by outpoint
     *
     * @param outpoint The transaction output to look up
     * @return UTXOEntry if found and unspent, std::nullopt otherwise
     */
    virtual std::optional<UTXOEntry> GetUTXO(const OutPoint& outpoint) const = 0;

    /**
     * Add a new UTXO to the active set
     *
     * @param outpoint The transaction output identifier
     * @param entry The UTXO data (value, script, height, etc.)
     * @return true if added successfully, false on error (e.g., duplicate)
     */
    virtual bool AddUTXO(const OutPoint& outpoint, const UTXOEntry& entry) = 0;

    /**
     * Mark a UTXO as spent (remove from active set)
     *
     * @param outpoint The transaction output to spend
     * @param spend_height Block height where spend occurred (for undo tracking)
     * @return true if UTXO was found and spent, false if not found
     */
    virtual bool SpendUTXO(const OutPoint& outpoint, uint32_t spend_height) = 0;

    /**
     * Delete a UTXO from the active set (for reorg undo)
     *
     * IDEMPOTENT: Returns true if UTXO was deleted OR was already absent.
     * This is critical for reorg safety - DisconnectBlock may try to delete
     * UTXOs that were never added (e.g., outputs belonging to other wallets).
     *
     * @param outpoint The transaction output to delete
     * @return true if deleted or already absent (idempotent success)
     */
    virtual bool DeleteUTXO(const OutPoint& outpoint) = 0;

    /**
     * Check if a UTXO exists and is unspent
     *
     * @param outpoint The transaction output to check
     * @return true if UTXO exists and is unspent, false otherwise
     */
    virtual bool HasUTXO(const OutPoint& outpoint) const = 0;
};

} // namespace consensus
} // namespace dinero
