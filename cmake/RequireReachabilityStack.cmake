# Release gate for the P2P reachability stack.
#
# A node without UPnP / NAT-PMP cannot open router ports, and one without
# QUIC + its TLS bridge cannot carry encrypted relay traffic. Each of those used
# to drop out of a build silently -- a missing library was a STATUS line in
# PortMapping.cmake, QUIC defaults OFF, and an OpenSSL without the QUIC TLS
# callbacks only disables the bridge -- and shipped binaries lost all three.
#
# With DINERO_REQUIRE_REACHABILITY_STACK=ON (release lanes) anything missing
# fails configure. Otherwise it is a WARNING, so a dev build still works but no
# longer looks finished. Feature names match scripts/ci/check_reachability_stack.py.

function(dinero_require_reachability_stack)
  set(_missing "")
  if(NOT DINERO_ENABLE_PORTMAPPING OR NOT DINERO_PORTMAP_HAVE_MINIUPNPC)
    list(APPEND _missing upnp)
  endif()
  if(NOT DINERO_ENABLE_PORTMAPPING OR NOT DINERO_PORTMAP_HAVE_NATPMP)
    list(APPEND _missing natpmp)
  endif()
  if(NOT DINERO_ENABLE_QUIC)
    list(APPEND _missing quic)
  endif()
  # VendorNgtcp2.cmake return()s before setting the backend when QUIC is off.
  if(NOT DINERO_ENABLE_QUIC OR NOT DEFINED DINERO_NGTCP2_CRYPTO_BACKEND
     OR DINERO_NGTCP2_CRYPTO_BACKEND STREQUAL "none")
    list(APPEND _missing quic_crypto)
  endif()

  if(NOT _missing)
    message(STATUS "Reachability stack: upnp, natpmp, quic, quic_crypto compiled in")
    return()
  endif()

  list(JOIN _missing ", " _missing_text)
  if(DINERO_REQUIRE_REACHABILITY_STACK)
    message(FATAL_ERROR
      "Release build requires the reachability stack; missing: ${_missing_text}")
  endif()
  message(WARNING
    "Reachability stack incomplete (dev build): missing ${_missing_text} — this "
    "binary cannot open router ports and/or carry encrypted relay traffic")
endfunction()
