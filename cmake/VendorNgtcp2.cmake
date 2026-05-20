# Vendored ngtcp2 QUIC transport dependency (Phase B2).
#
# This intentionally builds libngtcp2 core only: no examples, no tests, no TLS
# crypto backend, and no mainnet relay activation. The encrypted transport
# layer will wire TLS/session handling in a later slice.

if(NOT DINERO_ENABLE_QUIC)
  message(STATUS "ngtcp2 QUIC transport dependency: disabled (DINERO_ENABLE_QUIC=OFF)")
  return()
endif()

set(DINERO_NGTCP2_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/ngtcp2")
set(DINERO_NGTCP2_LIB_DIR "${DINERO_NGTCP2_SOURCE_DIR}/lib")
set(DINERO_NGTCP2_BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/ngtcp2-build")

if(NOT EXISTS "${DINERO_NGTCP2_SOURCE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR
    "DINERO_ENABLE_QUIC=ON requires vendored ngtcp2 at third_party/ngtcp2.\n"
    "Run: git submodule update --init third_party/ngtcp2")
endif()

set(DINERO_NGTCP2_VERSION "1.22.1")

function(_dinero_ngtcp2_hex_version out_var major minor patch)
  math(EXPR _dinero_ngtcp2_version_dec "${major} * 256 * 256 + ${minor} * 256 + ${patch}")
  set(_dinero_ngtcp2_version_hex "0x")
  foreach(_dinero_ngtcp2_i RANGE 5 0 -1)
    math(EXPR _dinero_ngtcp2_num "(${_dinero_ngtcp2_version_dec} >> (4 * ${_dinero_ngtcp2_i})) & 15")
    string(SUBSTRING "0123456789abcdef" ${_dinero_ngtcp2_num} 1 _dinero_ngtcp2_num_hex)
    string(APPEND _dinero_ngtcp2_version_hex "${_dinero_ngtcp2_num_hex}")
  endforeach()
  set(${out_var} "${_dinero_ngtcp2_version_hex}" PARENT_SCOPE)
endfunction()

_dinero_ngtcp2_hex_version(DINERO_NGTCP2_VERSION_NUM 1 22 1)

include(CheckIncludeFile)
include(CheckSymbolExists)
include(CheckTypeSize)

check_include_file("arpa/inet.h" HAVE_ARPA_INET_H)
check_include_file("netinet/in.h" HAVE_NETINET_IN_H)
check_include_file("netinet/ip.h" HAVE_NETINET_IP_H)
check_include_file("unistd.h" HAVE_UNISTD_H)
check_include_file("sys/endian.h" HAVE_SYS_ENDIAN_H)
check_include_file("endian.h" HAVE_ENDIAN_H)
check_include_file("byteswap.h" HAVE_BYTESWAP_H)
check_include_file("asm/types.h" HAVE_ASM_TYPES_H)
check_include_file("linux/netlink.h" HAVE_LINUX_NETLINK_H)
check_include_file("linux/rtnetlink.h" HAVE_LINUX_RTNETLINK_H)

check_type_size("ssize_t" SIZEOF_SSIZE_T)
if(SIZEOF_SSIZE_T STREQUAL "")
  set(ssize_t ptrdiff_t)
endif()

if(HAVE_ENDIAN_H)
  check_symbol_exists(be64toh "endian.h" HAVE_DECL_BE64TOH)
endif()
if(NOT HAVE_DECL_BE64TOH AND HAVE_SYS_ENDIAN_H)
  check_symbol_exists(be64toh "sys/endian.h" HAVE_DECL_BE64TOH)
endif()

check_symbol_exists(bswap_64 "byteswap.h" HAVE_DECL_BSWAP_64)
check_symbol_exists(explicit_bzero "string.h" HAVE_EXPLICIT_BZERO)
check_symbol_exists(memset_s "string.h" HAVE_MEMSET_S)

if(CMAKE_C_BYTE_ORDER STREQUAL "BIG_ENDIAN")
  set(WORDS_BIGENDIAN 1)
endif()

set(PACKAGE_VERSION "${DINERO_NGTCP2_VERSION}")
set(PACKAGE_VERSION_NUM "${DINERO_NGTCP2_VERSION_NUM}")

file(MAKE_DIRECTORY
  "${DINERO_NGTCP2_BINARY_DIR}"
  "${DINERO_NGTCP2_BINARY_DIR}/lib/includes/ngtcp2"
)
configure_file(
  "${DINERO_NGTCP2_SOURCE_DIR}/cmakeconfig.h.in"
  "${DINERO_NGTCP2_BINARY_DIR}/config.h"
)
configure_file(
  "${DINERO_NGTCP2_LIB_DIR}/includes/ngtcp2/version.h.in"
  "${DINERO_NGTCP2_BINARY_DIR}/lib/includes/ngtcp2/version.h"
  @ONLY
)

set(DINERO_NGTCP2_SOURCES
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_pkt.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_conv.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_str.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_vec.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_buf.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_conn.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_mem.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_pq.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_map.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_rob.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_ppe.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_crypto.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_err.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_range.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_acktr.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_rtb.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_frame_chain.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_strm.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_idtr.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_gaptr.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_ringbuf.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_log.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_qlog.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_cid.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_ksl.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_cc.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_bbr.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_addr.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_path.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_pv.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_pmtud.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_version.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_rst.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_window_filter.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_opl.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_balloc.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_objalloc.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_unreachable.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_transport_params.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_settings.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_callbacks.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_dcidtr.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_pcg.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_ratelim.c"
  "${DINERO_NGTCP2_LIB_DIR}/ngtcp2_conn_info.c"
)

add_library(dinero_ngtcp2 STATIC ${DINERO_NGTCP2_SOURCES})
target_include_directories(dinero_ngtcp2
  PUBLIC
    "${DINERO_NGTCP2_LIB_DIR}/includes"
    "${DINERO_NGTCP2_BINARY_DIR}/lib/includes"
  PRIVATE
    "${DINERO_NGTCP2_BINARY_DIR}"
)
target_compile_definitions(dinero_ngtcp2
  PUBLIC
    DINERO_HAVE_NGTCP2=1
    NGTCP2_STATICLIB
  PRIVATE
    BUILDING_NGTCP2
    HAVE_CONFIG_H
)
set_target_properties(dinero_ngtcp2 PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_VISIBILITY_PRESET hidden
)

message(STATUS "ngtcp2 QUIC transport dependency: vendored static core enabled (${DINERO_NGTCP2_VERSION})")
