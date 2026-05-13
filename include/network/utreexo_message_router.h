// Copyright (c) 2025 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "storage/chain_data_view.h"
#include <memory>
#include <functional>

namespace dinero {

// Forward declarations
class PeerConnection;
class P2PMessage;
class IChainDataView;

namespace network {

class BridgeNode;
class StatelessNode;

/**
 * UtreexoMessageRouter - Pure message dispatch for Utreexo P2P protocol
 *
 * **Purpose:** Route Utreexo messages to appropriate handlers without coupling
 *              to block processing, chainstate, or other daemon concerns.
 *
 * **Triggered by:** Phase 7.4.2 test isolation needs
 *
 * **Responsibilities:**
 * - Deserialize getutxoproof, getutxohdrs messages
 * - Call BridgeNode handlers with callbacks
 * - Serialize utxoproof, utxohdrs responses
 * - Route responses to StatelessNode
 *
 * **Does NOT:**
 * - Process blocks (no BlockAcceptor)
 * - Mutate chainstate (no ChainstateService)
 * - Serialize transactions (no TransactionSerializer)
 * - Handle undo data (no UndoRecord)
 *
 * **Design Philosophy:**
 * This is a pure message router. It knows about P2P framing and callbacks,
 * but nothing about consensus, block validation, or state transitions.
 *
 * **Testing:**
 * Can be tested in isolation with MockChainDataView and MockBridgeNode.
 * No heavyweight dependencies required.
 */
class UtreexoMessageRouter {
public:
    /**
     * @brief Construct message router
     * @param bridge_node BridgeNode for proof generation (can be null)
     * @param stateless_node StatelessNode for proof consumption (can be null)
     * @param chain_view Read-only chain data access
     */
    explicit UtreexoMessageRouter(
        BridgeNode* bridge_node,
        StatelessNode* stateless_node,
        IChainDataView* chain_view
    );

    ~UtreexoMessageRouter() = default;

    // Disable copy, enable move
    UtreexoMessageRouter(const UtreexoMessageRouter&) = delete;
    UtreexoMessageRouter& operator=(const UtreexoMessageRouter&) = delete;
    UtreexoMessageRouter(UtreexoMessageRouter&&) = default;
    UtreexoMessageRouter& operator=(UtreexoMessageRouter&&) = default;

    /**
     * @brief Handle incoming getutxoproof request (bridge node mode)
     * @param peer Requesting peer
     * @param message P2P message containing request
     * @return true if handled successfully
     */
    bool handleGetUtreexoProof(std::shared_ptr<PeerConnection> peer, const P2PMessage& message);

    /**
     * @brief Handle incoming getutxohdrs request (bridge node mode)
     * @param peer Requesting peer
     * @param message P2P message containing request
     * @return true if handled successfully
     */
    bool handleGetUtreexoHeaders(std::shared_ptr<PeerConnection> peer, const P2PMessage& message);

    /**
     * @brief Handle incoming utxoproof response (stateless node mode)
     * @param peer Responding peer
     * @param message P2P message containing proof
     * @return true if handled successfully
     */
    bool handleUtreexoProof(std::shared_ptr<PeerConnection> peer, const P2PMessage& message);

    /**
     * @brief Handle incoming utxohdrs response (stateless node mode)
     * @param peer Responding peer
     * @param message P2P message containing headers
     * @return true if handled successfully
     */
    bool handleUtreexoHeaders(std::shared_ptr<PeerConnection> peer, const P2PMessage& message);

    // Dependency injection
    void setBridgeNode(BridgeNode* bridge_node) { bridge_node_ = bridge_node; }
    BridgeNode* getBridgeNode() const { return bridge_node_; }

    void setStatelessNode(StatelessNode* stateless_node) { stateless_node_ = stateless_node; }
    StatelessNode* getStatelessNode() const { return stateless_node_; }

    void setChainView(IChainDataView* chain_view) { chain_view_ = chain_view; }
    IChainDataView* getChainView() const { return chain_view_; }

private:
    BridgeNode* bridge_node_{nullptr};
    StatelessNode* stateless_node_{nullptr};
    IChainDataView* chain_view_{nullptr};
};

} // namespace network
} // namespace dinero
