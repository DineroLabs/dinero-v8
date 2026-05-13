# RPC Component - Extended Handlers
# Production-grade RPC handlers for wallet, mining, transactions, and diagnostics

# Find required dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONCPP REQUIRED jsoncpp)

# Source files for extended RPC handlers
set(RPC_EXTENDED_SOURCES
    wallet_rpc_handlers.cpp
    transaction_rpc_handlers.cpp
    mining_rpc_handlers.cpp
    diagnostics_rpc_handlers.cpp
    rpc_canonicalizer.cpp
)

# Create static library for extended RPC handlers
add_library(dinero_rpc_extended STATIC ${RPC_EXTENDED_SOURCES})

# Link libraries for RPC handlers
target_link_libraries(dinero_rpc_extended
    PUBLIC
        dinero_common
        ${JSONCPP_LIBRARIES}
)

# Include directories
target_include_directories(dinero_rpc_extended
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
        ${JSONCPP_INCLUDE_DIRS}
)

# Compiler flags
target_compile_options(dinero_rpc_extended PRIVATE
    ${JSONCPP_CFLAGS_OTHER}
)

message(STATUS "  ✅ dinero_rpc_extended - Extended RPC handlers library")
