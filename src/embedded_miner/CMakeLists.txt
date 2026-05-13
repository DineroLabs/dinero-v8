# Dinero Embedded Miner Component
# C++ miner with RocksDB integration and CLI11 interface

# Source files for embedded miner
set(EMBEDDED_MINER_SOURCES
    main.cpp
    mining_core.cpp
)

# Create embedded miner executable
add_executable(embedded_miner ${EMBEDDED_MINER_SOURCES})

# Link libraries for embedded miner (including common library)
target_link_libraries(embedded_miner
    PRIVATE
        dinero_common
        CLI11::CLI11
)

# Compiler flags for embedded miner
target_compile_options(embedded_miner PRIVATE
    ${JSONCPP_CFLAGS_OTHER}
)

if(DIN_ENABLE_ROCKSDB)
    target_compile_options(embedded_miner PRIVATE ${ROCKSDB_CFLAGS_OTHER})
endif()

# Install embedded miner
install(TARGETS embedded_miner
    RUNTIME DESTINATION bin
)

message(STATUS "  ✅ embedded_miner - C++ miner with RocksDB integration") 