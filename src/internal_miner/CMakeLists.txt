# Dinero Internal Miner Component
# GUI-controlled mining integrated with web interface

# Source files for internal miner
set(INTERNAL_MINER_SOURCES
    main.cpp
    gui_mining.cpp
)

# Create internal miner executable
add_executable(internal_miner ${INTERNAL_MINER_SOURCES})

# Link libraries for internal miner (including common library)
target_link_libraries(internal_miner
    PRIVATE
        dinero_common
        CLI11::CLI11
)

# Compiler flags for internal miner
target_compile_options(internal_miner PRIVATE
    ${JSONCPP_CFLAGS_OTHER}
)

# Install internal miner
install(TARGETS internal_miner
    RUNTIME DESTINATION bin
)

message(STATUS "  ✅ internal_miner - GUI-controlled mining") 