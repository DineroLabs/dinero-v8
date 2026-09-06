if(NOT DEFINED SHIELDED_SOURCE OR NOT EXISTS "${SHIELDED_SOURCE}")
  message(FATAL_ERROR "SHIELDED_SOURCE must name shieldedwidget.cpp")
endif()
if(NOT DEFINED MAINWINDOW_SOURCE OR NOT EXISTS "${MAINWINDOW_SOURCE}")
  message(FATAL_ERROR "MAINWINDOW_SOURCE must name mainwindow.cpp")
endif()
if(NOT DEFINED WALLET_RPC_SOURCE OR NOT EXISTS "${WALLET_RPC_SOURCE}")
  message(FATAL_ERROR "WALLET_RPC_SOURCE must name methods_wallet_context.cpp")
endif()

file(READ "${SHIELDED_SOURCE}" shielded_source)
file(READ "${MAINWINDOW_SOURCE}" mainwindow_source)
file(READ "${WALLET_RPC_SOURCE}" wallet_rpc_source)

foreach(required
    "if (!isShieldedWidgetRpc(method))"
    "[status] waiting for embedded daemon RPC..."
    "[status] daemon RPC connected")
  string(FIND "${shielded_source}" "${required}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "Missing Shielded RPC status behavior: ${required}")
  endif()
endforeach()

set(phantom_method "wallet.getviewkey" "info")
string(JOIN "" phantom_method ${phantom_method})
string(FIND "${mainwindow_source}" "${phantom_method}" phantom_rpc)
if(NOT phantom_rpc EQUAL -1)
  message(FATAL_ERROR "Qt still calls nonexistent wallet.getviewkeyinfo RPC")
endif()

foreach(required
    "result[\"account_index\"] = 0"
    "result[\"fingerprint\"] = hd_wallet->GetMasterFingerprintHex()")
  string(FIND "${wallet_rpc_source}" "${required}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "wallet.getinfo is missing identity metadata: ${required}")
  endif()
endforeach()
