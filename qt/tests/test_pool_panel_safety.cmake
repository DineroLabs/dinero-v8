file(READ "${SOURCE_FILE}" POOL_SOURCE)
file(READ "${HEADER_FILE}" POOL_HEADER)

foreach(REQUIRED_TEXT
    "address.isLoopback()"
    "scheme != QStringLiteral(\"https\")"
    "scheme == QStringLiteral(\"http\") && loopback"
    "req.setTransferTimeout(8000)"
    "req.setAttribute(kKindAttr, kKindStatus)"
    "if (status_in_flight_)"
    "if (payout_in_flight_)"
    "if (earnings_in_flight_)")
  string(FIND "${POOL_SOURCE}" "${REQUIRED_TEXT}" FOUND_AT)
  if(FOUND_AT EQUAL -1)
    message(FATAL_ERROR "Pool panel safety regression: missing '${REQUIRED_TEXT}'")
  endif()
endforeach()

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
