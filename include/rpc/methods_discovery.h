#pragma once

namespace din {
namespace rpc {

/**
 * Register RPC discovery methods
 *
 * This registers:
 * - rpc.discover - List all available RPC methods
 * - rpc.info - Get RPC server information
 */
void register_discovery_methods();

} // namespace rpc
} // namespace din
