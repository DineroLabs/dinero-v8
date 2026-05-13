# Dinero Common Module
# Shared code used by all components: SHA256d, RocksDBManager, RPCClient, utils

# Source files for common module
set(COMMON_SOURCES
    sha256d.cpp
    rpc_client.cpp
    utils.cpp
    logger.cpp
    config_manager.cpp
    json_utils.cpp
    network_utils.cpp
    thread_pool.cpp
)

# Conditionally add RocksDB-dependent files if enabled
if(DIN_ENABLE_ROCKSDB)
    list(APPEND COMMON_SOURCES 
        rocksdb_manager.cpp
        blockchain_db.cpp
        blockchain_db_cf.cpp
        blockchain_db_backup.cpp
    )
else()
    # Add SQLite-only blockchain_db implementation if available
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/blockchain_db_sqlite.cpp")
        list(APPEND COMMON_SOURCES blockchain_db_sqlite.cpp)
    endif()
endif()

# Create common library
add_library(dinero_common STATIC ${COMMON_SOURCES})

# Set include directories for common library
target_include_directories(dinero_common
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

# Link libraries for common module
target_link_libraries(dinero_common
    PUBLIC
        CURL::libcurl
        ${JSONCPP_LIBRARIES}
        pthread
)

# Conditionally link RocksDB if enabled
if(DIN_ENABLE_ROCKSDB)
    target_link_libraries(dinero_common PUBLIC ${ROCKSDB_LIBRARIES})
    target_compile_options(dinero_common PRIVATE ${ROCKSDB_CFLAGS_OTHER})
endif()

# Compiler flags for common module
target_compile_options(dinero_common PRIVATE
    ${JSONCPP_CFLAGS_OTHER}
)

# Set C++ standard for common module
set_target_properties(dinero_common PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

message(STATUS "  ✅ dinero_common - Shared code library") 