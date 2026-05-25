// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "din_json.h"

namespace dinero { class P2PService; }

namespace dinero::rpc {

// Implements the relay_hints.list JSON-RPC method.
//
// Walks P2PManager::relay_hints_by_target_ under its mutex, copies into
// a value-type snapshot, releases the lock, then builds the JSON.
//
// Returns shape:
//   { "rpc_schema": "din.rpc.v1",
//     "targets": [ { target_node_id_hex, endpoints: [...] }, ... ],
//     "total_targets": N,
//     "ttl_seconds": kHintTtl,
//     "max_failures": kHintMaxFailures }
//
// Empty cache returns total_targets:0 + targets:[]. Never errors.
din::Json HandleRelayHintsList(dinero::P2PService* p2p_service);

}  // namespace dinero::rpc
