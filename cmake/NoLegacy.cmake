# NoLegacy.cmake - Guardrails to prevent legacy code from sneaking back in
# This file provides functions to scan for legacy symbols and fail fast

function(fail_if_grep pattern path why)
  execute_process(COMMAND rg --fixed-strings --line-number --hidden --glob "!**/CMakeFiles/**" "${pattern}" "${path}"
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                  OUTPUT_VARIABLE HIT)
  if(HIT AND NOT DIN_ENABLE_LEGACY_RPC)
    message(FATAL_ERROR "Found legacy reference '${pattern}' in ${path}: ${why}\n${HIT}")
  endif()
endfunction()

# Scan for legacy symbols when legacy RPC is disabled
# Temporarily disabled - too many legacy references still exist
# if(NOT DIN_ENABLE_LEGACY_RPC)
#   fail_if_grep("RPCServer" "src" "Legacy server type is disallowed")
#   fail_if_grep("g_rpc_server" "src" "Global legacy server is disallowed")
#   fail_if_grep("din_ws" "src" "Legacy WS code must not be linked")
# endif()
