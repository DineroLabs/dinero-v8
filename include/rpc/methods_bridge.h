#pragma once

namespace dinero {
namespace rpc {

/**
 * Register Fiat Bridge RPC methods
 *
 * Methods:
 * - bridge.getrate <from> <to>
 * - bridge.convert <from> <to> <amount> [provider]
 * - bridge.providers
 * - bridge.status
 */
void registerBridgeRPC();

} // namespace rpc
} // namespace dinero
