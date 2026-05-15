if(NOT DINERO_DEPENDS_DIR)
  message(FATAL_ERROR "DINERO_DEPENDS_DIR is required")
endif()
if(NOT DINERO_DEPENDS_PREFIX)
  message(FATAL_ERROR "DINERO_DEPENDS_PREFIX is required")
endif()

file(WRITE "${DINERO_DEPENDS_DIR}/current-prefix.env"
     "DINERO_DEPENDS_PREFIX=${DINERO_DEPENDS_PREFIX}\n")

file(REMOVE_RECURSE "${DINERO_DEPENDS_DIR}/current")
file(CREATE_LINK
     "${DINERO_DEPENDS_PREFIX}"
     "${DINERO_DEPENDS_DIR}/current"
     SYMBOLIC
     RESULT _link_result)

if(NOT _link_result STREQUAL "0")
  message(WARNING
          "Could not create depends/current symlink: ${_link_result}. "
          "Use current-prefix.env or DINERO_DEPENDS_PREFIX explicitly.")
endif()
