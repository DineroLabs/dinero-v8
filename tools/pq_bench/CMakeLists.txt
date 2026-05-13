# V7 PQ bench + round-trip tests.
# Neither target is part of the default dinerod build.

# ML-DSA-65 verify benchmark — standalone, links PQClean directly to isolate
# any future overhead added by the C++ façade.
add_executable(ml_dsa_bench ml_dsa_bench.cpp)
target_link_libraries(ml_dsa_bench PRIVATE pqclean_ml_dsa_65)
set_target_properties(ml_dsa_bench PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# PQ primitives + registry round-trip tests — uses the dinero_pq façade,
# exercising exactly the surface consensus callers will use. Also links
# OpenSSL::Crypto for HMAC-SHA256 used by the wallet-identity HKDF vector.
add_executable(pq_roundtrip_test pq_roundtrip_test.cpp)
target_link_libraries(pq_roundtrip_test PRIVATE dinero_pq OpenSSL::Crypto)
set_target_properties(pq_roundtrip_test PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# V7 wallet PQ derivation library tests (Phase 4c.1 coverage).
add_executable(pq_derivation_test pq_derivation_test.cpp)
target_link_libraries(pq_derivation_test PRIVATE dinero_pq OpenSSL::Crypto)
set_target_properties(pq_derivation_test PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# V7 PQ wallet RPC handler integration test (Phase 4c.3).
add_executable(v7_pq_rpc_test
    v7_pq_rpc_test.cpp
    # Uses wallet address codec + bech32 encoder; compile directly to
    # keep this phase's target off dinero_core.
    ${CMAKE_SOURCE_DIR}/src/wallet/p2mr_address.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/bech32_encoder.cpp
)
target_include_directories(v7_pq_rpc_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/daemon
)
target_link_libraries(v7_pq_rpc_test PRIVATE dinero_pq OpenSSL::Crypto sqlite3)
set_target_properties(v7_pq_rpc_test PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# V7 P2MR store + AEAD round-trip integration test (Phase 4c.2).
add_executable(v7_p2mr_store_test
    v7_p2mr_store_test.cpp
    # Wallet address codec + the bech32 encoder it wraps. We compile these
    # directly into the test instead of pulling in dinero_core, which isn't
    # yet consumed by our Phase-4 test surface.
    ${CMAKE_SOURCE_DIR}/src/wallet/p2mr_address.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/bech32_encoder.cpp
)
target_include_directories(v7_p2mr_store_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/daemon
)
target_link_libraries(v7_p2mr_store_test PRIVATE dinero_pq OpenSSL::Crypto sqlite3)
set_target_properties(v7_p2mr_store_test PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# V7 P2MR consensus primitives integration test (Phase 5) + VWU tests (Phase 8).
# Uses dinero_pq + wallet::BuildP2MRScriptPubKey + Transaction type.
#
# Compiles chainparams_impl.cpp directly to satisfy GetActiveChain() references
# from dinero_consensus_core (consensus_utxo_set.cpp / utreexo_accumulator.cpp).
# The macOS linker tolerated the unresolved ref when nothing in the test binary
# exercised those paths; GNU ld on Linux strictly rejects at link time. Linking
# the full dinero_consensus instead would drag in dinero_zk, which has a
# separate pre-existing submodule drift on some build hosts.
add_executable(p2mr_verify_test
    p2mr_verify_test.cpp
    ${CMAKE_SOURCE_DIR}/src/wallet/p2mr_address.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/bech32_encoder.cpp
    ${CMAKE_SOURCE_DIR}/src/consensus/chainparams_impl.cpp
)
target_include_directories(p2mr_verify_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/daemon
)
target_link_libraries(p2mr_verify_test PRIVATE dinero_pq dinero_consensus_core OpenSSL::Crypto)
set_target_properties(p2mr_verify_test PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# V7 P2MR bech32m address codec round-trip tests.
# Compiles the codec + the existing Bech32Encoder into the test binary
# directly to avoid pulling in dinero_core at this early phase. A proper
# wallet-layer library integration happens in Phase 7.
add_executable(p2mr_address_test
    p2mr_address_test.cpp
    ${CMAKE_SOURCE_DIR}/src/wallet/p2mr_address.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/bech32_encoder.cpp
)
target_include_directories(p2mr_address_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src          # for "daemon/bech32_encoder.h" (header lives in src/daemon)
    ${CMAKE_SOURCE_DIR}/src/daemon   # bech32_encoder.h has no "daemon/" path prefix inside itself
)
set_target_properties(p2mr_address_test PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# V7 Shielded pool round-trip tests.
add_executable(shielded_pool_test shielded_pool_test.cpp)
target_link_libraries(shielded_pool_test PRIVATE dinero_shielded OpenSSL::Crypto)
target_include_directories(shielded_pool_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
set_target_properties(shielded_pool_test PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# V7 Adversarial consensus hardening suite.
add_executable(shielded_adversarial_test shielded_adversarial_test.cpp)
target_link_libraries(shielded_adversarial_test PRIVATE dinero_shielded OpenSSL::Crypto)
set_target_properties(shielded_adversarial_test PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
