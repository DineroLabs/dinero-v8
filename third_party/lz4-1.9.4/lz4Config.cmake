# lz4Config.cmake - CMake package config for vendored LZ4
#
# This file allows RocksDB to find our vendored LZ4 static library

if(NOT TARGET lz4::lz4)
    add_library(lz4::lz4 ALIAS lz4_static)
endif()

set(lz4_FOUND TRUE)
set(lz4_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/lib")
set(lz4_LIBRARIES lz4::lz4)
