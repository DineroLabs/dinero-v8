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
    # The earnings card reports TWO figures and they must stay
    # distinguishable. Lifetime only ever rises; unspent falls when the
    # operator moves funds out. Collapsing them, or labelling one as the
    # other, is the regression this guards — it previously read a bare
    # unspent balance under an earnings heading.
    "Fee earnings (verified on-chain)"
    "lifetime"
    "unspent now"
    # Lifetime must be summed from the address history, never taken from
    # the balance call. `getaddressbalance` is a UTXO-set sum: using it
    # here would make "lifetime" fall every time the operator swept their
    # fee, which is the exact mislabel above wearing a new name.
    "blockchain.getaddresshistory")
  string(FIND "${POOL_SOURCE}" "${REQUIRED_TEXT}" FOUND_AT)
  if(FOUND_AT EQUAL -1)
    message(FATAL_ERROR "Pool cockpit regression: missing '${REQUIRED_TEXT}'")
  endif()
endforeach()

string(FIND "${POOL_SOURCE}" "Total received by this address" FALSE_EARNINGS_AT)
if(NOT FALSE_EARNINGS_AT EQUAL -1)
  message(FATAL_ERROR "Pool cockpit regression: unspent balance is mislabeled as lifetime earnings")
endif()

# A single page of history is 200 entries against addresses that hold
# thousands, so a lifetime total that does not page is a floor presented
# as a total — on DineroSJ it read 2,000 DIN against a true 33,905.
string(FIND "${POOL_SOURCE}" "nextFromHeight" PAGING_AT)
if(PAGING_AT EQUAL -1)
  message(FATAL_ERROR
    "Pool cockpit regression: lifetime earnings no longer page the full address history")
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
    "bool earnings_in_flight_ = false"
    # The balance and the history reply land independently; without its
    # own flag one arriving would re-arm the button while the other was
    # still walking the chain.
    "bool lifetime_in_flight_ = false")
  string(FIND "${POOL_HEADER}" "${REQUIRED_TEXT}" FOUND_AT)
  if(FOUND_AT EQUAL -1)
    message(FATAL_ERROR "Pool panel single-flight regression: missing '${REQUIRED_TEXT}'")
  endif()
endforeach()
