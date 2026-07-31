# Vendored STATIC libcurl for the Windows MSVC lanes. Defines CURL::libcurl as
# an imported static target linked against the vendored static OpenSSL 3.5.7,
# so no libcurl/openssl DLLs are needed at runtime. Include AFTER ThirdParty.cmake
# (which sets OPENSSL_ROOT_DIR / DINERO_VENDORED_OPENSSL_VERSION) and BEFORE any
# curl consumer subdirectory.
#
# DINERO_VENDORED_CURL_VERSION is the single source of truth and MUST match
# scripts/build-curl-vendored.ps1.
#
# An earlier revision discovered the prebuilt with
#   file(GLOB third_party/curl-*/...) + list(SORT) + list(REVERSE) + list(GET 0)
# i.e. "lexicographically highest directory wins". That is not version ordering:
# with curl-8.21.0, curl-8.11.1 and curl-8.9 all present it selects curl-8.9,
# because "9" > "2" as a string. On a clean CI runner only one directory exists,
# so the bug is invisible there — but a developer or packaging machine with an
# older curl-* tree left behind would silently link an unintended, possibly
# vulnerable, curl into a shipped binary. Resolve the exact path instead, and
# verify what is actually in it.
set(DINERO_VENDORED_CURL_VERSION "8.21.0" CACHE STRING "Pinned vendored curl version")
option(DINERO_VENDORED_CURL "Use vendored static libcurl instead of find_package(CURL)" ${MSVC})
set(DINERO_VENDORED_CURL_DIR "" CACHE PATH "Override dir containing prebuilt vendored static libcurl")

if(DINERO_VENDORED_CURL AND NOT TARGET CURL::libcurl)
  if(DINERO_VENDORED_CURL_DIR)
    set(_curl_root "${DINERO_VENDORED_CURL_DIR}")
  else()
    set(_curl_root
      "${CMAKE_SOURCE_DIR}/third_party/curl-${DINERO_VENDORED_CURL_VERSION}/prebuilt/windows-x86_64-msvc")
  endif()

  if(NOT IS_DIRECTORY "${_curl_root}")
    message(FATAL_ERROR
      "DINERO_VENDORED_CURL is ON but the pinned vendored static libcurl was not found at "
      "${_curl_root}. Run scripts/build-curl-vendored.ps1 first "
      "(expected curl ${DINERO_VENDORED_CURL_VERSION}).")
  endif()

  set(_curl_lib "${_curl_root}/lib/libcurl.lib")
  set(_curl_inc "${_curl_root}/include")
  if(NOT EXISTS "${_curl_lib}")
    message(FATAL_ERROR "Vendored static libcurl missing: ${_curl_lib}. Run scripts/build-curl-vendored.ps1.")
  endif()

  # --- Gate 1: the headers we compile against must declare the pinned version.
  # The override path is validated too, so -DDINERO_VENDORED_CURL_DIR cannot be
  # used to smuggle in an unverified tree.
  set(_curlver_h "${_curl_inc}/curl/curlver.h")
  if(NOT EXISTS "${_curlver_h}")
    message(FATAL_ERROR "Vendored curl headers incomplete: ${_curlver_h} is missing.")
  endif()
  file(STRINGS "${_curlver_h}" _curlver_line REGEX "^#define[ \t]+LIBCURL_VERSION[ \t]+\"")
  if(NOT _curlver_line)
    message(FATAL_ERROR "Could not read LIBCURL_VERSION from ${_curlver_h}.")
  endif()
  string(REGEX REPLACE "^#define[ \t]+LIBCURL_VERSION[ \t]+\"([^\"]+)\".*$" "\\1"
         _curlver "${_curlver_line}")
  if(NOT _curlver STREQUAL DINERO_VENDORED_CURL_VERSION)
    message(FATAL_ERROR
      "Vendored curl version mismatch: headers at ${_curl_inc} declare LIBCURL_VERSION "
      "\"${_curlver}\" but this build pins ${DINERO_VENDORED_CURL_VERSION}. "
      "Rebuild with scripts/build-curl-vendored.ps1.")
  endif()

  # --- Gate 2: build metadata must agree on BOTH curl and OpenSSL. This is what
  # catches a prebuilt produced against a different crypto baseline.
  set(_curl_meta "${_curl_root}/.dinero-build-meta")
  if(NOT EXISTS "${_curl_meta}")
    message(FATAL_ERROR
      "Vendored curl build metadata missing: ${_curl_meta}. "
      "Rebuild with scripts/build-curl-vendored.ps1.")
  endif()
  file(STRINGS "${_curl_meta}" _meta_curl REGEX "^CURL_VERSION=")
  file(STRINGS "${_curl_meta}" _meta_ossl REGEX "^OPENSSL_VERSION=")
  string(REPLACE "CURL_VERSION=" "" _meta_curl "${_meta_curl}")
  string(REPLACE "OPENSSL_VERSION=" "" _meta_ossl "${_meta_ossl}")
  string(STRIP "${_meta_curl}" _meta_curl)
  string(STRIP "${_meta_ossl}" _meta_ossl)
  if(NOT _meta_curl STREQUAL DINERO_VENDORED_CURL_VERSION)
    message(FATAL_ERROR
      "Vendored curl metadata mismatch: ${_curl_meta} says CURL_VERSION=${_meta_curl} "
      "but this build pins ${DINERO_VENDORED_CURL_VERSION}.")
  endif()
  if(NOT _meta_ossl STREQUAL DINERO_VENDORED_OPENSSL_VERSION)
    message(FATAL_ERROR
      "Vendored curl was built against OpenSSL ${_meta_ossl} but this build uses "
      "OpenSSL ${DINERO_VENDORED_OPENSSL_VERSION} (${_curl_meta}). Rebuild libcurl "
      "with scripts/build-curl-vendored.ps1 so a single crypto baseline ships.")
  endif()

  set(_ossl "${CMAKE_SOURCE_DIR}/third_party/openssl-${DINERO_VENDORED_OPENSSL_VERSION}/prebuilt/windows-x86_64-msvc")
  add_library(CURL::libcurl STATIC IMPORTED GLOBAL)
  set_target_properties(CURL::libcurl PROPERTIES
    IMPORTED_LOCATION "${_curl_lib}"
    INTERFACE_INCLUDE_DIRECTORIES "${_curl_inc}"
    INTERFACE_COMPILE_DEFINITIONS "CURL_STATICLIB"
    INTERFACE_LINK_LIBRARIES "${_ossl}/libssl.lib;${_ossl}/libcrypto.lib;ws2_32;crypt32;wldap32;bcrypt;normaliz;advapi32")
  set(CURL_FOUND TRUE)
  message(STATUS "Using vendored STATIC libcurl ${_curlver}: ${_curl_lib}")
  message(STATUS "  linked against vendored OpenSSL ${DINERO_VENDORED_OPENSSL_VERSION}")
endif()
