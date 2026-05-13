#pragma once

#include "lightning/invoice.h"
#include "lightning/onion.h"
#include "common/status.h"
#include <vector>
#include <optional>

namespace dinero {
namespace lightning {

/**
 * @file payment_utils.h
 * @brief Payment routing utilities for Lightning Network
 *
 * Helper functions for building routes, converting between invoice and onion formats,
 * and managing payment pathfinding.
 */

/**
 * @brief Build a payment route from invoice route hints
 * @param invoice BOLT #11 invoice with route hints
 * @param our_node_id Our node's public key (33 bytes)
 * @param amount_msat Payment amount in milliuna
 * @param final_cltv_delta Final CLTV delta for destination
 * @return Result with vector of RouteHop or error
 *
 * This function converts invoice RouteHint structures into onion RouteHop structures
 * suitable for OnionBuilder. It handles:
 * - Direct payments (no route hints)
 * - Single-hop payments via route hints
 * - Multi-hop payments through intermediate nodes
 */
Result<std::vector<RouteHop>> buildRouteFromInvoice(
    const Invoice& invoice,
    const std::vector<uint8_t>& our_node_id,
    uint64_t amount_msat,
    uint32_t final_cltv_delta
);

/**
 * @brief Calculate fees for a route
 * @param route Vector of route hops
 * @param final_amount_msat Final amount to be received
 * @return Total amount to send (including all fees)
 */
uint64_t calculateRouteFees(
    const std::vector<RouteHop>& route,
    uint64_t final_amount_msat
);

/**
 * @brief Check if we have a direct channel to the destination
 * @param destination_node_id Destination node public key
 * @param channels Vector of our active channels
 * @return true if direct channel exists
 */
bool hasDirectChannel(
    const std::vector<uint8_t>& destination_node_id,
    const std::vector<std::string>& channel_ids
);

} // namespace lightning
} // namespace dinero
