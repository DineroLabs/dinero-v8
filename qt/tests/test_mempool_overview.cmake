if(NOT DEFINED SOURCE_FILE)
  message(FATAL_ERROR "SOURCE_FILE is required")
endif()

file(READ "${SOURCE_FILE}" SOURCE)

foreach(REQUIRED_TEXT
    [[rpc_->call("mempool.getrawmempool", QJsonArray{true})]]
    [[method == "mempool.getrawmempool"]]
    [[tblMempoolOverview_ = new QTableWidget(0, 4)]]
    [[if (changed) loadTransactionHistory()]]
    [[No pending transactions shown while the local node is syncing]]
    [[Mempool: unavailable]])
  string(FIND "${SOURCE}" "${REQUIRED_TEXT}" FOUND_AT)
  if(FOUND_AT EQUAL -1)
    message(FATAL_ERROR "Qt mempool Overview regression: missing '${REQUIRED_TEXT}'")
  endif()
endforeach()

message(STATUS "Qt mempool Overview production wiring is present")
