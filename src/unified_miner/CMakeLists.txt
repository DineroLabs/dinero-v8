# Dinero Unified Miner Component
# Single binary with subcommands for all three mining clients

# Source files for unified miner
set(UNIFIED_MINER_SOURCES
    main.cpp
    gui_miner.cpp
    embedded_miner.cpp
    lightweight_miner.cpp
    miner_manager.cpp
)

# Create unified miner executable
add_executable(dinerominer ${UNIFIED_MINER_SOURCES})

# Link libraries for unified miner (including common library)
target_link_libraries(dinerominer
    PRIVATE
        dinero_common
        CLI11::CLI11
)

# Compiler flags for unified miner
target_compile_options(dinerominer PRIVATE
    ${JSONCPP_CFLAGS_OTHER}
    -DUNIFIED_MINER_BUILD
)

if(DIN_ENABLE_ROCKSDB)
    target_compile_options(dinerominer PRIVATE ${ROCKSDB_CFLAGS_OTHER})
endif()

# Install unified miner
install(TARGETS dinerominer
    RUNTIME DESTINATION bin
)

message(STATUS "  ✅ dinerominer - Unified miner with subcommands") 