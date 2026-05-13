// Copyright (c) 2025 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "network/utreexo_message_router.h"
#include "network/bridge_node.h"
#include "network/stateless_node.h"
#include "network/utreexo_messages.h"
#include "daemon/peer_connection.h"
#include "daemon/p2p_message.h"
#include "common/logger.h"
#include "primitives/block.h"

namespace dinero {
namespace network {

UtreexoMessageRouter::UtreexoMessageRouter(
    BridgeNode* bridge_node,
    StatelessNode* stateless_node,
    IChainDataView* chain_view
) : bridge_node_(bridge_node),
    stateless_node_(stateless_node),
    chain_view_(chain_view)
{
}

bool UtreexoMessageRouter::handleGetUtreexoProof(
    std::shared_ptr<PeerConnection> peer,
    const P2PMessage& message
) {
    g_logger.info("Handling getutxoproof message from peer " + peer->getPeerId());

    // Check if bridge node is enabled
    if (!bridge_node_) {
        g_logger.warning("Received getutxoproof but bridge node is not enabled");
        return false;
    }

    // Deserialize request
    const GetUtreexoProofMessage* req = dynamic_cast<const GetUtreexoProofMessage*>(&message);
    if (!req) {
        g_logger.error("Failed to cast message to GetUtreexoProofMessage");
        return false;
    }

    // Phase 7.4.4: Enhanced validation for DoS protection
    if (!req->isValid()) {
        g_logger.warning("Invalid getutxoproof request from peer " + peer->getPeerId() +
                        " (size=" + std::to_string(req->block_hashes.size()) +
                        ", max=" + std::to_string(GetUtreexoProofMessage::MAX_BATCH_SIZE) + ")");
        return false;
    }

    // Phase 7.4.4: Additional checks
    if (req->block_hashes.empty()) {
        g_logger.warning("Empty getutxoproof request from peer " + peer->getPeerId());
        return false;
    }

    // Block provider callback - fetches block from ChainDB via IChainDataView
    auto block_provider = [this](const uint256& block_hash) -> std::optional<Block> {
        if (!chain_view_) {
            g_logger.error("ChainDB not available for block lookup");
            return std::nullopt;
        }

        // Lookup block from chain database (Phase 7.4.2.1: Use IChainDataView API)
        auto result = chain_view_->getBlock(block_hash);
        if (result.ok()) {
            return result.value();
        }
        return std::nullopt;
    };

    try {
        // Generate proofs using BridgeNode
        auto proof_result = bridge_node_->HandleProofRequest(*req, block_provider);

        // Send each proof back to requesting peer
        for (const auto& proof : proof_result.proofs) {
            // Wrap in P2PMessage for network transmission
            auto p2p_msg = std::make_shared<UtreexoProofP2PMessage>();
            p2p_msg->message = std::make_shared<UtreexoProofMessage>(proof);
            if (!peer->sendMessage(*p2p_msg)) {
                g_logger.error("Failed to send utxoproof to peer " + peer->getPeerId());
                return false;
            }
        }

        g_logger.info("Sent " + std::to_string(proofs.size()) + " utxoproof messages to peer " + peer->getPeerId());
        return true;

    } catch (const std::exception& e) {
        g_logger.error("Exception handling getutxoproof from peer " + peer->getPeerId() + ": " + e.what());
        return false;
    }
}

bool UtreexoMessageRouter::handleGetUtreexoHeaders(
    std::shared_ptr<PeerConnection> peer,
    const P2PMessage& message
) {
    g_logger.info("Handling getutxohdrs message from peer " + peer->getPeerId());

    // Check if bridge node is enabled
    if (!bridge_node_) {
        g_logger.warning("Received getutxohdrs but bridge node is not enabled");
        return false;
    }

    // Deserialize request
    const GetUtreexoHeadersMessage* req = dynamic_cast<const GetUtreexoHeadersMessage*>(&message);
    if (!req) {
        g_logger.error("Failed to cast message to GetUtreexoHeadersMessage");
        return false;
    }

    // Header provider callback - fetches header by hash (Phase 7.4.2.1: Use IChainDataView API)
    auto header_provider = [this](const uint256& block_hash) -> std::optional<BlockHeader> {
        if (!chain_view_) {
            g_logger.error("ChainDB not available for header lookup");
            return std::nullopt;
        }

        // Lookup header from chain database
        auto result = chain_view_->getHeader(block_hash);
        if (result.ok()) {
            return result.value();
        }
        return std::nullopt;
    };

    // Header by height provider callback (Phase 7.4.2.1: Use IChainDataView API)
    auto header_by_height_provider = [this](uint32_t height) -> std::optional<BlockHeader> {
        if (!chain_view_) {
            g_logger.error("ChainDB not available for header lookup by height");
            return std::nullopt;
        }

        // Lookup block hash by height, then header
        auto hash_result = chain_view_->getBlockHashByHeight(height);
        if (!hash_result.ok()) {
            return std::nullopt;
        }

        auto header_result = chain_view_->getHeader(hash_result.value());
        if (header_result.ok()) {
            return header_result.value();
        }
        return std::nullopt;
    };

    try {
        // Generate headers response using BridgeNode
        UtreexoHeadersMessage response = bridge_node_->HandleHeadersRequest(
            *req,
            header_provider,
            header_by_height_provider
        );

        // Wrap in P2PMessage for network transmission
        auto p2p_msg = std::make_shared<UtreexoHeadersP2PMessage>();
        p2p_msg->message = std::make_shared<UtreexoHeadersMessage>(response);
        if (!peer->sendMessage(*p2p_msg)) {
            g_logger.error("Failed to send utxohdrs to peer " + peer->getPeerId());
            return false;
        }

        g_logger.info("Sent utxohdrs message to peer " + peer->getPeerId() +
                     " with " + std::to_string(response.headers.size()) + " headers");
        return true;

    } catch (const std::exception& e) {
        g_logger.error("Exception handling getutxohdrs from peer " + peer->getPeerId() + ": " + e.what());
        return false;
    }
}

bool UtreexoMessageRouter::handleUtreexoProof(
    std::shared_ptr<PeerConnection> peer,
    const P2PMessage& message
) {
    // Phase 8.5: Consistent null checks (no silent success)
    if (!stateless_node_) {
        g_logger.debug("Received utxoproof but stateless node is not enabled");
        return false;  // Not enabled, message not processed
    }

    const UtreexoProofMessage* proof_msg = dynamic_cast<const UtreexoProofMessage*>(&message);
    if (!proof_msg) {
        g_logger.error("UtreexoMessageRouter: Failed to cast message to UtreexoProofMessage");
        return false;
    }

    // Phase 8.1: Pure delegation
    return stateless_node_->onProofResponse(peer.get(), *proof_msg);
}

bool UtreexoMessageRouter::handleUtreexoHeaders(
    std::shared_ptr<PeerConnection> peer,
    const P2PMessage& message
) {
    // Phase 8.5: Consistent null checks (no silent success)
    if (!stateless_node_) {
        g_logger.debug("Received utxohdrs but stateless node is not enabled");
        return false;  // Not enabled, message not processed
    }

    const UtreexoHeadersMessage* headers_msg = dynamic_cast<const UtreexoHeadersMessage*>(&message);
    if (!headers_msg) {
        g_logger.error("UtreexoMessageRouter: Failed to cast message to UtreexoHeadersMessage");
        return false;
    }

    // Phase 8.1: Pure delegation
    return stateless_node_->onHeadersResponse(peer.get(), *headers_msg);
}

} // namespace network
} // namespace dinero
