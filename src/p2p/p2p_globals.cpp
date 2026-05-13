#include "p2p_globals.h"

namespace dinero {

// Definition: initialized to nullptr, set by daemon during startup
P2PManager* dinero::legacy::g_peer_manager() = nullptr;

} // namespace dinero
