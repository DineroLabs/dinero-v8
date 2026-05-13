# PQClean vendored subset for Dinero v7
#
# Builds libpqclean_ml_dsa_65.a — a static library containing the clean
# ref implementation of ML-DSA-65 + the minimal PQClean common utils it
# depends on (fips202, randombytes).
#
# Consensus usage: link via the C++ wrapper at src/consensus/pq/ml_dsa_65.{h,cpp}.
# Do NOT link PQClean directly from consensus code — go through the wrapper.

cmake_minimum_required(VERSION 3.15)

set(PQCLEAN_ML_DSA_65_DIR ${CMAKE_CURRENT_SOURCE_DIR}/crypto_sign/ml-dsa-65/clean)
set(PQCLEAN_COMMON_DIR    ${CMAKE_CURRENT_SOURCE_DIR}/common)

# ML-DSA-65 clean sources (vendored as-is from PQClean upstream pin)
file(GLOB PQCLEAN_ML_DSA_65_SOURCES
    ${PQCLEAN_ML_DSA_65_DIR}/*.c
)

# Common utils needed by ML-DSA-65: SHAKE (fips202) + randombytes (keygen/sign only)
set(PQCLEAN_COMMON_SOURCES
    ${PQCLEAN_COMMON_DIR}/fips202.c
    ${PQCLEAN_COMMON_DIR}/randombytes.c
)

add_library(pqclean_ml_dsa_65 STATIC
    ${PQCLEAN_ML_DSA_65_SOURCES}
    ${PQCLEAN_COMMON_SOURCES}
)

target_include_directories(pqclean_ml_dsa_65 PUBLIC
    ${PQCLEAN_ML_DSA_65_DIR}
    ${PQCLEAN_COMMON_DIR}
)

# Portable C99 - no architecture-specific flags. Consensus must be
# deterministic across x86-64 / aarch64 / whatever else. If a host
# compiler opportunistically auto-vectorizes that's fine as long as
# output bytes are identical, which for the ref impl they are.
set_target_properties(pqclean_ml_dsa_65 PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

# PQClean files use some common compiler warnings as informational; silence
# known-harmless ones so our -Werror-clean build isn't blocked by vendor code.
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(pqclean_ml_dsa_65 PRIVATE
        -Wno-unused-parameter
        -Wno-unused-function
        -Wno-sign-compare
    )
endif()
