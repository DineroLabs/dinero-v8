// SPDX-License-Identifier: MIT
// Dinero - Blockchain RPC Method Metadata
#pragma once

namespace dinero {
namespace rpc {

/**
 * Register all blockchain RPC methods with comprehensive metadata
 *
 * Adds detailed documentation to blockchain RPC methods including:
 * - Descriptions
 * - Parameter specifications
 * - Return value documentation
 * - Help text with examples
 *
 * This improves RPC UX by making methods discoverable and self-documenting.
 */
void registerBlockchainRPCMetadata();

} // namespace rpc
} // namespace dinero
