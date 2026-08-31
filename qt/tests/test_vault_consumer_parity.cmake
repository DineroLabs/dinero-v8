file(READ "${SOURCE_FILE}" VAULT_SOURCE)

foreach(REQUIRED_TEXT
    "din1p… Taproot address"
    "Amount (DIN):"
    "account_spendable_una_ <= 0 || amount > account_spendable_una_"
    "The daemon's binding is the source of truth"
    "isTransientConnectionError(code, message)"
    "event_log_->clear()"
    "reconcileWithdrawalJournal()"
    "Advanced / Operator Details")
  string(FIND "${VAULT_SOURCE}" "${REQUIRED_TEXT}" FOUND_AT)
  if(FOUND_AT EQUAL -1)
    message(FATAL_ERROR "Vault consumer parity regression: missing '${REQUIRED_TEXT}'")
  endif()
endforeach()

string(FIND "${VAULT_SOURCE}" "scriptPubKey hex" LEGACY_DESTINATION)
if(NOT LEGACY_DESTINATION EQUAL -1)
  message(FATAL_ERROR "Vault UI must request a din1p address, not scriptPubKey hex")
endif()

string(FIND "${VAULT_SOURCE}" "appendLog(QString(\"✗ %1 [%2]: %3\")" ERROR_LOG)
string(FIND "${VAULT_SOURCE}" "if (isTransientConnectionError(code, message))" TRANSIENT_GUARD)
if(ERROR_LOG EQUAL -1 OR TRANSIENT_GUARD EQUAL -1 OR TRANSIENT_GUARD GREATER ERROR_LOG)
  message(FATAL_ERROR "Transient daemon failures must be suppressed before activity logging")
endif()
