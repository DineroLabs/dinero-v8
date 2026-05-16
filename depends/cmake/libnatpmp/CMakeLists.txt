cmake_minimum_required(VERSION 3.21)

project(dinero_dep_libnatpmp LANGUAGES C)

if(NOT LIBNATPMP_SOURCE_DIR)
  message(FATAL_ERROR "LIBNATPMP_SOURCE_DIR is required")
endif()

set(_natpmp_sources
  "${LIBNATPMP_SOURCE_DIR}/natpmp.c"
  "${LIBNATPMP_SOURCE_DIR}/getgateway.c")

if(WIN32)
  list(APPEND _natpmp_sources "${LIBNATPMP_SOURCE_DIR}/wingettimeofday.c")
endif()

add_library(natpmp STATIC ${_natpmp_sources})
target_include_directories(natpmp PUBLIC "${LIBNATPMP_SOURCE_DIR}")
target_compile_definitions(natpmp PRIVATE ENABLE_STRNATPMPERR)

if(WIN32)
  target_compile_definitions(natpmp PUBLIC NATPMP_STATICLIB)
  target_link_libraries(natpmp PUBLIC ws2_32 iphlpapi)
endif()

install(TARGETS natpmp ARCHIVE DESTINATION lib)
install(FILES
        "${LIBNATPMP_SOURCE_DIR}/natpmp.h"
        "${LIBNATPMP_SOURCE_DIR}/natpmp_declspec.h"
        DESTINATION include)

if(WIN32)
  set(_libnatpmp_pc_cflags "-I\${includedir} -DNATPMP_STATICLIB")
else()
  set(_libnatpmp_pc_cflags "-I\${includedir}")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/libnatpmp.pc"
"prefix=${CMAKE_INSTALL_PREFIX}
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: libnatpmp
Description: NAT-PMP lightweight library
URL: https://miniupnp.tuxfamily.org/
Version: 20230423
Libs: -L\${libdir} -lnatpmp
Cflags: ${_libnatpmp_pc_cflags}
")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/libnatpmp.pc"
        DESTINATION lib/pkgconfig)
