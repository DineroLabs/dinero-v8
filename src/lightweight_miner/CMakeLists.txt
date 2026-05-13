# Dinero Lightweight Miner Component
# Portable CLI miner with minimal dependencies

# Source files for lightweight miner
set(LIGHTWEIGHT_MINER_SOURCES
    main.cpp
    mining_core.cpp
)

# Create lightweight miner executable
add_executable(lightweight_miner ${LIGHTWEIGHT_MINER_SOURCES})

# Link libraries for lightweight miner (including common library)
target_link_libraries(lightweight_miner
    PRIVATE
        dinero_common
        CLI11::CLI11
)

# Compiler flags for lightweight miner
target_compile_options(lightweight_miner PRIVATE
    ${JSONCPP_CFLAGS_OTHER}
)

# Install lightweight miner
install(TARGETS lightweight_miner
    RUNTIME DESTINATION bin
)

message(STATUS "  ✅ lightweight_miner - Portable CLI miner") 