# Vendored STATIC libcurl for the Windows MSVC lanes. Defines CURL::libcurl as
# an imported static target linked against the vendored static OpenSSL 3.5.7,
# so no libcurl/openssl DLLs are needed at runtime. Include AFTER ThirdParty.cmake
# (which sets OPENSSL_ROOT_DIR / DINERO_VENDORED_OPENSSL_VERSION) and BEFORE any
# curl consumer subdirectory.
option(DINERO_VENDORED_CURL "Use vendored static libcurl instead of find_package(CURL)" ${MSVC})
set(DINERO_VENDORED_CURL_DIR "" CACHE PATH "Override dir containing prebuilt vendored static libcurl")

if(DINERO_VENDORED_CURL AND NOT TARGET CURL::libcurl)
  if(DINERO_VENDORED_CURL_DIR)
    set(_curl_root "${DINERO_VENDORED_CURL_DIR}")
  else()
    file(GLOB _curl_candidates "${CMAKE_SOURCE_DIR}/third_party/curl-*/prebuilt/windows-x86_64-msvc")
    list(SORT _curl_candidates)
    list(REVERSE _curl_candidates)
    list(LENGTH _curl_candidates _n)
    if(_n EQUAL 0)
      message(FATAL_ERROR
        "DINERO_VENDORED_CURL is ON but no vendored static libcurl was found under "
        "third_party/curl-*/prebuilt/windows-x86_64-msvc. Run scripts/build-curl-vendored.ps1 first.")
    endif()
    list(GET _curl_candidates 0 _curl_root)
  endif()

  set(_curl_lib "${_curl_root}/lib/libcurl.lib")
  set(_curl_inc "${_curl_root}/include")
  if(NOT EXISTS "${_curl_lib}")
    message(FATAL_ERROR "Vendored static libcurl missing: ${_curl_lib}. Run scripts/build-curl-vendored.ps1.")
  endif()

  set(_ossl "${CMAKE_SOURCE_DIR}/third_party/openssl-${DINERO_VENDORED_OPENSSL_VERSION}/prebuilt/windows-x86_64-msvc")
  add_library(CURL::libcurl STATIC IMPORTED GLOBAL)
  set_target_properties(CURL::libcurl PROPERTIES
    IMPORTED_LOCATION "${_curl_lib}"
    INTERFACE_INCLUDE_DIRECTORIES "${_curl_inc}"
    INTERFACE_COMPILE_DEFINITIONS "CURL_STATICLIB"
    INTERFACE_LINK_LIBRARIES "${_ossl}/libssl.lib;${_ossl}/libcrypto.lib;ws2_32;crypt32;wldap32;bcrypt;normaliz;advapi32")
  set(CURL_FOUND TRUE)
  message(STATUS "Using vendored STATIC libcurl: ${_curl_lib}")
  message(STATUS "  linked against vendored OpenSSL ${DINERO_VENDORED_OPENSSL_VERSION}")
endif()
