# Live P2P/network sources compiled into dinero_core.
# This file is included from the repository root so paths remain root-relative.

set(DINERO_CORE_P2P_SOURCES
  # P2P layer
  src/p2p/peer_scoring.cpp
  src/p2p/connection_manager.cpp
  src/p2p/rate_limiter.cpp
  src/p2p/network_limits_monitor.cpp
  src/p2p/block_download_scheduler.cpp
  src/p2p/headers_first_sync.cpp
  src/p2p/multi_peer_headers_sync.cpp
  src/p2p/compact_block.cpp
  src/p2p/addrman.cpp
  src/p2p/addr_v2.cpp                # NAT traversal Phase 1A.2: BIP155 typed addr gossip
  src/p2p/peer_manager_posix.cpp

  # Network glue used by the daemon core
  src/network/network_invariants.cpp
  src/network/port_mapper.cpp
  src/network/udp_socket.cpp         # NAT traversal Phase B1: UDP socket facility
  src/daemon/p2p_message.cpp
  src/daemon/peer_connection.cpp
  src/daemon/p2p_manager.cpp
  src/daemon/node_identity.cpp        # NAT traversal Phase 1A: dineroid handshake keypair
  src/daemon/header_serialization.cpp
  src/network/utreexo_messages.cpp
  src/network/stateless_node.cpp

  # P2P infrastructure
  src/p2p/orphan_block_pool.cpp
  src/p2p/block_presence.cpp
)
