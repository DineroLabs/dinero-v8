file(READ "${SOURCE_FILE}" POOL_SOURCE)
file(READ "${HEADER_FILE}" POOL_HEADER)
file(READ "${MAINWINDOW_FILE}" MAINWINDOW_SOURCE)

foreach(REQUIRED_TEXT
    "address.isLoopback()"
    "scheme != QStringLiteral(\"https\")"
    "scheme == QStringLiteral(\"http\") && loopback"
    "req.setTransferTimeout(8000)"
    "QNetworkRequest::ManualRedirectPolicy"
    "req.setAttribute(kKindAttr, kKindStatus)"
    "if (status_in_flight_)"
    "if (payout_in_flight_)"
    "if (earnings_in_flight_)")
  string(FIND "${POOL_SOURCE}" "${REQUIRED_TEXT}" FOUND_AT)
  if(FOUND_AT EQUAL -1)
    message(FATAL_ERROR "Pool panel safety regression: missing '${REQUIRED_TEXT}'")
  endif()
endforeach()

foreach(REQUIRED_TEXT
    "schema_version"
    "Malformed pool status"
    "STALE"
    "Connected sessions:"
    "PPLNS contributors (not connected sessions)"
    "Share activity history (stored locally)"
    "pool/payoutJournal/"
    "pool/feeJournal/"
    "Outcome uncertain"
    "Change operator fee"
    "Bring the cockpit online."
    "Current unspent fee balance (verified on-chain)"
    "spent outputs are intentionally excluded")
  string(FIND "${POOL_SOURCE}" "${REQUIRED_TEXT}" FOUND_AT)
  if(FOUND_AT EQUAL -1)
    message(FATAL_ERROR "Pool cockpit regression: missing '${REQUIRED_TEXT}'")
  endif()
endforeach()

string(FIND "${POOL_SOURCE}" "Total received by this address" FALSE_EARNINGS_AT)
if(NOT FALSE_EARNINGS_AT EQUAL -1)
  message(FATAL_ERROR "Pool cockpit regression: unspent balance is mislabeled as lifetime earnings")
endif()

string(FIND "${MAINWINDOW_SOURCE}" "poolPanel_ = new PoolPanel(rpc_, this);" POOL_TAB_AT)
if(POOL_TAB_AT EQUAL -1)
  message(FATAL_ERROR "Pool panel visibility regression: Pool tab is not created")
endif()

string(FIND "${MAINWINDOW_SOURCE}"
  "#if defined(DIN_ENABLE_LIQUIDITY_VAULT_UI)" VAULT_GATE_START)
if(VAULT_GATE_START EQUAL -1)
  message(FATAL_ERROR "Pool panel visibility regression: Liquidity Vault gate is missing")
endif()
string(SUBSTRING "${MAINWINDOW_SOURCE}" ${VAULT_GATE_START} -1 VAULT_GATE_TAIL)
string(FIND "${VAULT_GATE_TAIL}" "#endif" VAULT_GATE_END)
if(VAULT_GATE_END EQUAL -1)
  message(FATAL_ERROR "Pool panel visibility regression: Liquidity Vault gate is unterminated")
endif()
string(SUBSTRING "${VAULT_GATE_TAIL}" 0 ${VAULT_GATE_END} VAULT_GATED_SOURCE)
string(FIND "${VAULT_GATED_SOURCE}" "poolPanel_ = new PoolPanel" POOL_IN_VAULT_GATE)
if(NOT POOL_IN_VAULT_GATE EQUAL -1)
  message(FATAL_ERROR
    "Pool panel visibility regression: Pool tab is coupled to the disabled Liquidity Vault gate")
endif()

message(STATUS "Qt Pool panel transport and single-flight safety checks passed")

foreach(REQUIRED_TEXT
    "bool status_in_flight_ = false"
    "bool payout_in_flight_ = false"
    "bool earnings_in_flight_ = false")
  string(FIND "${POOL_HEADER}" "${REQUIRED_TEXT}" FOUND_AT)
  if(FOUND_AT EQUAL -1)
    message(FATAL_ERROR "Pool panel single-flight regression: missing '${REQUIRED_TEXT}'")
  endif()
endforeach()
