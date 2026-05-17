# Standalone developer/miner tools.
#
# This file is included from the repository root so paths remain root-relative
# and helper functions/variables from the root CMake file stay in scope.

# Genesis miner tool (legacy 80-byte header)
add_executable(genesis_miner tools/genesis_miner.cpp)
target_link_libraries(genesis_miner PRIVATE dinero_consensus dinero_crypto pthread)
target_include_directories(genesis_miner PRIVATE ${CMAKE_SOURCE_DIR}/src)

# Genesis miner v3 (128-byte BlockHeader v1 with Utreexo commitment)
add_executable(genesis_miner_v3 tools/genesis_miner_v3_correct.cpp)
target_link_libraries(genesis_miner_v3 PRIVATE
    dinero_consensus
    dinero_crypto
    pthread
    OpenSSL::SSL
    OpenSSL::Crypto
)
target_include_directories(genesis_miner_v3 PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

# Genesis miner v7 (same BlockHeader v1; new motto + timestamp)
add_executable(genesis_miner_v7 tools/genesis_miner_v7.cpp)
target_link_libraries(genesis_miner_v7 PRIVATE
    dinero_consensus
    dinero_crypto
    pthread
    OpenSSL::SSL
    OpenSSL::Crypto
)
target_include_directories(genesis_miner_v7 PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

# premine_miner removed — no premine in Fair Launch v3

# Genesis bundle verifier (standalone, no DB/RPC/datadir)
add_executable(verify_genesis_bundle
    tools/verify_genesis_bundle.cpp
    # dinero_consensus_core references active-chain accessors from Utreexo
    # helpers; compile the canonical chainparams implementation directly.
    src/consensus/chainparams_impl.cpp
)
target_link_libraries(verify_genesis_bundle PRIVATE
    dinero_consensus_core
    dinero_crypto
    jsoncpp_static
    pthread
    OpenSSL::SSL
    OpenSSL::Crypto
    zstd
)
target_include_directories(verify_genesis_bundle PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

# Raw Taproot key generator (no wallet dependency)
# DISABLED: tools/taproot_keygen.cpp not found - uncomment when added
# add_executable(taproot_keygen tools/taproot_keygen.cpp)
# target_link_libraries(taproot_keygen PRIVATE
#   secp256k1
#   OpenSSL::SSL
#   OpenSSL::Crypto
# )
# target_include_directories(taproot_keygen PRIVATE
#   ${CMAKE_SOURCE_DIR}/vendor/include
#   ${CMAKE_SOURCE_DIR}/include
# )

# compute_premine_commitment and verify_premine_seed removed — no premine in Fair Launch v3

# CPU miner (external process)
add_executable(dinero-miner tools/dinero_miner.cpp)
target_link_libraries(dinero-miner PRIVATE
  dinero_crypto
  jsoncpp_static
  ${DINERO_CURL_TARGET}
)
target_include_directories(dinero-miner PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/external
  /opt/homebrew/include
)
target_compile_definitions(dinero-miner PRIVATE
  DINERO_CLI_VERSION=${PROJECT_VERSION}
  DINERO_CLI_GIT_SHA=${GIT_HASH}
  DINERO_GIT_COMMIT_FULL="${GIT_HASH_FULL}"
  DINERO_CLI_BUILD_DATE=${BUILD_TIME}
  DINERO_BUILD_REPO="dinero"
  DINERO_BUILD_COMPONENT="dinero-miner"
)

# Dedicated Stratum pool worker (external process, CPU-only)
add_executable(dinero-stratum-worker tools/dinero_stratum_worker.cpp)
target_link_libraries(dinero-stratum-worker PRIVATE
  dinero_crypto
  jsoncpp_static
  ${DINERO_CURL_TARGET}
)
target_include_directories(dinero-stratum-worker PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/external
  /opt/homebrew/include
)
target_compile_definitions(dinero-stratum-worker PRIVATE
  DINERO_CLI_VERSION=${PROJECT_VERSION}
  DINERO_CLI_GIT_SHA=${GIT_HASH}
  DINERO_GIT_COMMIT_FULL="${GIT_HASH_FULL}"
  DINERO_CLI_BUILD_DATE=${BUILD_TIME}
  DINERO_BUILD_REPO="dinero"
  DINERO_BUILD_COMPONENT="dinero-stratum-worker"
)

# GPU miner (external process, uses Metal/CUDA/OpenCL backends)
add_executable(dinero-gpu-miner tools/dinero_gpu_miner.cpp)
target_link_libraries(dinero-gpu-miner PRIVATE
  dinero_gpu_mining
  dinero_crypto
  jsoncpp_static
  ${DINERO_CURL_TARGET}
)
target_include_directories(dinero-gpu-miner PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/external
  /opt/homebrew/include
)
target_compile_definitions(dinero-gpu-miner PRIVATE
  DINERO_CLI_VERSION=${PROJECT_VERSION}
  DINERO_CLI_GIT_SHA=${GIT_HASH}
  DINERO_GIT_COMMIT_FULL="${GIT_HASH_FULL}"
  DINERO_CLI_BUILD_DATE=${BUILD_TIME}
  DINERO_BUILD_REPO="dinero"
  DINERO_BUILD_COMPONENT="dinero-gpu-miner"
)

# SIMD benchmark tool
add_executable(bench-simd tools/bench_simd.cpp)
target_link_libraries(bench-simd PRIVATE dinero_crypto)
target_include_directories(bench-simd PRIVATE ${CMAKE_SOURCE_DIR}/src)

# Embed VS_VERSION_INFO into each tool binary on Windows.
dinero_attach_win32_versioninfo(dinero-miner          "Dinero CPU miner")
dinero_attach_win32_versioninfo(dinero-stratum-worker "Dinero Stratum worker client")
dinero_attach_win32_versioninfo(dinero-gpu-miner      "Dinero GPU miner (CUDA/OpenCL)")

set(DINERO_TOOL_TARGETS
  genesis_miner
  dinero-miner
  dinero-stratum-worker
  dinero-gpu-miner
  bench-simd
)
