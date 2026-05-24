// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "din_json.h"

namespace dinero { class P2PService; }

namespace dinero::rpc {

// Implements the dynamic_p2p.observe JSON-RPC method.
//
// Returns a snapshot of the Dynamic P2P state: mode (active/observe/off),
// governor counts + candidate lists (when enabled), and the full
// PeerQualitySnapshot for each currently-connected peer.
//
// Never throws and never returns a JSON-RPC error: when DPP is in OFF
// mode or P2PService is unavailable, returns a valid object with
// {enabled: false, mode: "off", governor: null, peers: []}.
din::Json HandleDynamicP2PObserve(dinero::P2PService* p2p_service);

}  // namespace dinero::rpc
