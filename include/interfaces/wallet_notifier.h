#pragma once

#include <cstdint>
#include <vector>

// Forward declarations
namespace dinero {
    struct Block;  // defined as struct in primitives/block.h — class/struct
                   // mismatch mangles differently on MSVC
    struct Transaction;
}

namespace dinero {

/**
 * @brief Interface for wallet notifications from blockchain events
 *
 * Implements the observer pattern for wallet updates. When blocks are
 * connected to the blockchain (either mined locally or received from peers),
 * registered wallets are automatically notified to update their UTXO sets.
 *
 * This replaces manual isAddressMine() checks scattered throughout the codebase
 * with a clean, event-driven architecture.
 *
 * Implementation note: WalletManager implements this interface and is
 * registered with Chainstate during daemon bootstrap.
 */
class WalletNotifier {
public:
    virtual ~WalletNotifier() = default;

    /**
     * @brief Called when a new block is connected to the active chain
     *
     * The wallet should scan all transactions in the block and update
     * its UTXO set for any outputs belonging to wallet addresses.
     *
     * @param block The block being connected
     * @param height The height of the block in the chain
     */
    virtual void onBlockConnected(const Block& block, uint32_t height) = 0;

    /**
     * @brief Called when a block is disconnected during a reorg
     *
     * The wallet should remove UTXOs from this block and restore
     * any spent outputs that are now unspent again.
     *
     * @param block The block being disconnected
     * @param height The height of the block being removed
     */
    virtual void onBlockDisconnected(const Block& block, uint32_t height) = 0;

    /**
     * @brief Called when a transaction enters the mempool
     *
     * Optional: Allows wallet to track unconfirmed transactions.
     *
     * @param tx The transaction entering the mempool
     */
    virtual void onMempoolTransaction(const Transaction& tx) = 0;
};

} // namespace dinero
