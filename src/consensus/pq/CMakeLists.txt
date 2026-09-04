# Dinero v7 PQ primitives library (dinero_pq)
#
# Wraps the vendored PQClean ML-DSA-65 reference implementation with a
# narrow C++ API. This library is the ONLY bridge between consensus-critical
# C++ code and the vendored C crypto — no other source file in the tree
# should #include PQClean headers directly.
#
# Contents:
#   - scheme_registry.{h,cpp}  the PQSchemeRegistry lookup (scheme_id → params)
#   - ml_dsa_65.{h,cpp}        thin façade over PQClean for scheme_id = 0x01
#
# When FALCON / SPHINCS+ flip from DARK_RESERVED to ACCEPT (future activation
# fork), add one .cpp here and one row in the registry. No other change.

add_library(dinero_pq STATIC
    scheme_registry.cpp
    ml_dsa_65.cpp
    # Explicit seeded ML-DSA-65 keypair (NOT via randombytes hook). Uses
    # PQClean's exposed internal symbols to produce a deterministic pubkey
    # from a 32-byte seed. Bit-identical across architectures.
    ml_dsa_65_keygen.c
    # Phase 5: P2MR consensus primitives — script recognition, witness
    # codec, Merkle verifier, end-to-end spend verifier. No block/mempool
    # integration yet; this is the pure library a future v7 daemon calls.
    p2mr_consensus.cpp
    # Wallet-side HKDF-SHA256 → KeygenFromSeed pipeline (Phase 4c.1).
    # Pure; no storage, no RPC. Links OpenSSL for HMAC and SHA-256.
    ${CMAKE_SOURCE_DIR}/src/wallet/pq_derivation.cpp
    # RAII wrapper that OPENSSL_cleanse's the 4032-byte ML-DSA secret on
    # every path out of scope (Phase 4c.1.5).
    ${CMAKE_SOURCE_DIR}/src/wallet/secure_keypair.cpp
    # Phase 4c.2.a: AES-256-GCM seed encryption helpers (OpenSSL EVP).
    ${CMAKE_SOURCE_DIR}/src/wallet/aead_seed.cpp
    # Phase 4c.2.b: SQLite-backed v7 P2MR address store.
    ${CMAKE_SOURCE_DIR}/src/wallet/v7_p2mr_store.cpp
    # Phase 4c.3: pure-C++ RPC handlers (no JSON layer yet). Thin wrappers
    # over the store + derivation + AEAD primitives above.
    ${CMAKE_SOURCE_DIR}/src/rpc/v7_pq_handlers.cpp
    # Phase 10 Commit 2: hybrid KeyProvider that handles both legacy ECDSA
    # inputs and v7 P2MR inputs through the normal wallet send path.
    # Uses SignP2MR + SerializeP2MRWitness; implements the KeyProvider
    # interface declared in wallet/transaction_signer.h.
    ${CMAKE_SOURCE_DIR}/src/wallet/wallet_key_provider.cpp
)

target_include_directories(dinero_pq PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

# ml_dsa_65_keygen.c #includes PQClean's internal headers (api.h, params.h,
# polyvec.h, fips202.h, symmetric.h). Expose those paths PRIVATE so only
# this lib sees them; consumers go through ml_dsa_65.h.
target_include_directories(dinero_pq PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/pqclean/common
    ${CMAKE_SOURCE_DIR}/third_party/pqclean/crypto_sign/ml-dsa-65/clean
)

target_link_libraries(dinero_pq PRIVATE
    pqclean_ml_dsa_65
    dinero_crypto     # CSHA256 leaf and request hashing
    # HMAC-SHA256 used by pq_derivation.cpp. OpenSSL::Crypto is imported
    # in the root CMakeLists.txt before this subdir is added.
    OpenSSL::Crypto
    # AES-GCM (aead_seed.cpp) + SQLite (v7_p2mr_store.cpp).
    sqlite3
)

set_target_properties(dinero_pq PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)
