foreach(required_var MAINWINDOW_SOURCE MAINWINDOW_HEADER UTREEXO_RPC_SOURCE)
  if(NOT DEFINED ${required_var} OR NOT EXISTS "${${required_var}}")
    message(FATAL_ERROR "${required_var} must name an existing source file")
  endif()
endforeach()

file(READ "${MAINWINDOW_SOURCE}" mainwindow_source)
file(READ "${MAINWINDOW_HEADER}" mainwindow_header)
file(READ "${UTREEXO_RPC_SOURCE}" utreexo_rpc_source)

foreach(required
    "Utreexo Validation"
    "Role: --"
    "State: --"
    "Storage: --"
    "blockchain.getutreexocommitment"
    "bridges connected"
    "Proof serving active"
    "Compact validator"
    "disk savings: measuring")
  string(FIND "${mainwindow_source}" "${required}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "Missing Utreexo validation-panel behavior: ${required}")
  endif()
endforeach()

foreach(required
    "lblUtreexoRole_"
    "lblUtreexoState_"
    "lblUtreexoStorage_")
  string(FIND "${mainwindow_header}" "${required}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "Missing Utreexo validation-panel member: ${required}")
  endif()
endforeach()

foreach(required
    "validation_role"
    "verified_height"
    "bridge_enabled"
    "bridge_active"
    "bridge_peer_count"
    "compact_state_bytes"
    "forest_memory_bytes_estimate"
    "retains_full_state"
    "disk_savings_available"
    "UtreexoStump::fromForest")
  string(FIND "${utreexo_rpc_source}" "${required}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "Missing Utreexo telemetry field or guarded derivation: ${required}")
  endif()
endforeach()

# The panel must not invent a disk-savings estimate from the approximate
# per-leaf forest-memory constant.
string(FIND "${utreexo_rpc_source}" "result[\"disk_savings_available\"] = false" savings_guard)
if(savings_guard EQUAL -1)
  message(FATAL_ERROR "Utreexo telemetry must mark disk savings unavailable until measured")
endif()

# A BridgeNode object is wired during normal daemon setup even when capability
# advertisement is disabled. The UI must report proof serving active only when
# both the configuration and runtime object agree.
string(FIND "${utreexo_rpc_source}"
  "config.utreexo_bridge && chainstate->GetBridgeNode() != nullptr"
  bridge_config_guard)
if(bridge_config_guard EQUAL -1)
  message(FATAL_ERROR "Bridge-active telemetry must honor utreexo-bridge configuration")
endif()
