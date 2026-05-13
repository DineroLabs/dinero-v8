//
// methods_utreexo_bridge.cpp
// Utreexo Bridge RPC - Proof serving for light clients (CSN)
//
// Endpoints:
//   utreexo.getproof     - Get Utreexo proof for a UTXO
//   utreexo.getroots     - Get current accumulator roots
//   utreexo.getstate     - Get full accumulator state info
//   utreexo.getcache     - Get bridge proof cache stats
//   utreexo.getgossip    - Get proof gossip health stats
//
// These endpoints enable light clients to:
//   - Fetch proofs on demand when spending
//   - Bootstrap their Stump from current roots
//   - Verify they're on the correct chain
//
// Security: Read-only, no mutations. Proofs are self-verifying.
//
// Implementation: These are aliases to existing blockchain.* methods,
// providing a cleaner API surface for CSN (Compact State Node) clients.
//

#include "rpc/rpc_registry.h"
#include "rpc/methods_utreexo.h"
#include "common/logger.h"

extern RpcRegistry g_rpcRegistry;

//=============================================================================
// Registration - Register utreexo.* aliases for bridge API
//=============================================================================
//
// Maps:
//   utreexo.getproof  -> blockchain.getutxoproof
//   utreexo.getroots  -> blockchain.getutreexoroots
//   utreexo.getstate  -> blockchain.getutreexostats
//   utreexo.getcache  -> blockchain.getutreexocachestats
//   utreexo.getgossip -> blockchain.getutreexogossipstats
//

void RegisterUtreexoBridgeRPC() {
    dinero::g_logger.info("[Utreexo Bridge] Registering RPC methods...");

    // Register aliases for light client API surface
    // These point to the existing blockchain.* implementations

    // utreexo.getproof - Get proof for a UTXO (alias for blockchain.getutxoproof)
    g_rpcRegistry.registerAlias("utreexo.getproof", "blockchain.getutxoproof");

    // utreexo.getroots - Get current roots (alias for blockchain.getutreexoroots)
    g_rpcRegistry.registerAlias("utreexo.getroots", "blockchain.getutreexoroots");

    // utreexo.getstate - Get full state info (alias for blockchain.getutreexostats)
    g_rpcRegistry.registerAlias("utreexo.getstate", "blockchain.getutreexostats");

    // utreexo.getcache - Get bridge proof cache stats (alias for blockchain.getutreexocachestats)
    g_rpcRegistry.registerAlias("utreexo.getcache", "blockchain.getutreexocachestats");

    // utreexo.getgossip - Get proof gossip health stats (alias for blockchain.getutreexogossipstats)
    g_rpcRegistry.registerAlias("utreexo.getgossip", "blockchain.getutreexogossipstats");

    dinero::g_logger.info("[Utreexo Bridge] Registered 5 RPC aliases (getproof, getroots, getstate, getcache, getgossip)");
}
