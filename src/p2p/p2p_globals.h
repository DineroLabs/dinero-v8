#pragma once

// Forward declaration
class P2PManager;

namespace dinero {

// Global pointer to the P2PManager instance (set by daemon main)
extern P2PManager* g_peer_manager;

} // namespace dinero
