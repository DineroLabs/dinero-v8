# Argon2 - Password Hashing Function (PHC Winner 2015)
# Bundled for Dinero wallet encryption
# Source: https://github.com/P-H-C/phc-winner-argon2
# Version: 20190702
# License: Apache 2.0 / CC0 (Public Domain)

cmake_minimum_required(VERSION 3.15)

project(argon2 LANGUAGES C)

# Argon2 core library (reference implementation - portable)
add_library(argon2 STATIC
    src/argon2.c
    src/core.c
    src/encoding.c
    src/ref.c                    # Reference implementation (portable, no SIMD)
    src/thread.c
    src/blake2/blake2b.c
)

# Public headers
target_include_directories(argon2 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Internal headers
target_include_directories(argon2 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/blake2
)

# Platform-specific settings
if(UNIX AND NOT APPLE)
    # Linux: link pthread for threading
    target_link_libraries(argon2 PRIVATE pthread)
endif()

# Disable warnings for third-party code
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(argon2 PRIVATE -w)
endif()

# Compile as C11
set_target_properties(argon2 PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)

# Summary
message(STATUS "Argon2 reference implementation configured (portable, thread-safe)")

