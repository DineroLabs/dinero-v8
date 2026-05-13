# StaticSecp256k1.cmake
# =====================
# 
# This module provides a static libsecp256k1 target built from source
# instead of relying on system/Homebrew dynamic libraries.
# 
# Features:
# - Static linking (no runtime dependencies)
# - Built from Bitcoin Core's official source
# - Includes all necessary modules (ECDH, recovery, extrakeys, etc.)
# - No external dependencies

# Set the path to our static libsecp256k1
set(SECP256K1_STATIC_DIR "${CMAKE_SOURCE_DIR}/secp-prefix")
set(SECP256K1_STATIC_LIB "${SECP256K1_STATIC_DIR}/lib/libsecp256k1.a")
set(SECP256K1_STATIC_INCLUDE "${SECP256K1_STATIC_DIR}/include")

# Verify the static library exists
if(NOT EXISTS "${SECP256K1_STATIC_LIB}")
    message(FATAL_ERROR "Static libsecp256k1 not found at ${SECP256K1_STATIC_LIB}")
endif()

if(NOT EXISTS "${SECP256K1_STATIC_INCLUDE}/secp256k1.h")
    message(FATAL_ERROR "Static libsecp256k1 headers not found at ${SECP256K1_STATIC_INCLUDE}")
endif()

# Create imported static target
add_library(secp256k1_static STATIC IMPORTED)
set_target_properties(secp256k1_static PROPERTIES
    IMPORTED_LOCATION "${SECP256K1_STATIC_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${SECP256K1_STATIC_INCLUDE}"
    INTERFACE_COMPILE_DEFINITIONS "SECP256K1_STATIC"
)

# Set up the target with proper compile definitions
target_compile_definitions(secp256k1_static INTERFACE
    SECP256K1_STATIC
    SECP256K1_ENABLE_MODULE_ECDH
    SECP256K1_ENABLE_MODULE_RECOVERY
    SECP256K1_ENABLE_MODULE_EXTRAKEYS
)

# Add platform-specific dependencies
if(APPLE)
    # macOS needs Security framework for RNG
    target_link_libraries(secp256k1_static INTERFACE "-framework Security")
elseif(WIN32)
    # Windows needs BCrypt for RNG
    target_link_libraries(secp256k1_static INTERFACE bcrypt)
else()
    # Linux needs getrandom (usually available in glibc 2.25+)
    # If not available, we'll use /dev/urandom fallback
endif()

# Print status
message(STATUS "Static libsecp256k1 configured:")
message(STATUS "  Library: ${SECP256K1_STATIC_LIB}")
message(STATUS "  Headers: ${SECP256K1_STATIC_INCLUDE}")
message(STATUS "  Size: $(wc -c < ${SECP256K1_STATIC_LIB}) bytes")

# Create an alias for easier use
add_library(secp256k1 ALIAS secp256k1_static)
