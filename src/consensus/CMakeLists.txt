# Consensus target wiring.
#
# This file is included from the repository root CMakeLists.txt rather than
# added as a subdirectory because the legacy source lists below intentionally
# use root-relative paths.

# ═══════════════════════════════════════════════════════════════════════════
# LAYER ISOLATION: Three Persistence Layers (November 7, 2025)
# ═══════════════════════════════════════════════════════════════════════════
#
# Clean separation of database dependencies:
#   1. dinero_consensus   → Pure consensus rules (NO database)
#   2. dinero_chainstate  → RocksDB ONLY (consensus storage)
#   3. dinero_wallet      → SQLite ONLY (user data)
#
# This ensures:
#   - Wallet doesn't pull in RocksDB
#   - Chainstate doesn't pull in SQLite (except for sqlite_open.cpp stub)
#   - Each layer is independently testable
# ═══════════════════════════════════════════════════════════════════════════

# ─────────────────────────────────────────────────────────────────────────
# Layer 0a: PURE Consensus Core (NO IO, NO database, NO threading)
# ─────────────────────────────────────────────────────────────────────────
# This is the extractable consensus library: libdinero-consensus
# Can be used in mobile wallets, hardware wallets, browsers (WASM)
# See include/dinero_consensus.h for public API
# ─────────────────────────────────────────────────────────────────────────
add_library(dinero_consensus_core STATIC
  # Pure UTXO set (no database, no threading)
  src/consensus/utxo_publication.cpp
  src/consensus/consensus_utxo_set.cpp
  src/consensus/consensus_invariants.cpp

  # Utreexo accumulator (pure cryptographic)
  src/consensus/utreexo_accumulator.cpp
  src/consensus/utreexo_stump.cpp
  src/consensus/utreexo_delta_codec.cpp  # UD sidecar codec + forward replay (campaign phase 2)
  src/consensus/csn_replay_data.cpp     # Historical CSN targets and spent metadata

  # Merkle computation (pure math)
  src/consensus/merkle_root.cpp

  # BIP158 GCS block filters (pure math + SipHash)
  src/consensus/block_filter.cpp
  src/consensus/filter_commitment.cpp
  src/consensus/merkle_proof.cpp

  # PoW verification (pure math)
  src/consensus/pow.cpp
  src/consensus/chainwork.cpp
  src/consensus/difficulty_validation.cpp

  # Script execution (pure computation)
  src/consensus/script.cpp
  src/consensus/script_validation.cpp
  src/consensus/script_crypto.cpp
  src/consensus/crypto/sighash_bip143.cpp

  # Block undo data (pure data structures)
  src/consensus/block_undo.cpp

  # Subsidy and limits (pure constants)
  src/consensus/coinbase_maturity.cpp
  src/consensus/sigops.cpp
  src/consensus/freeze.cpp

  # Primitives (pure data structures)
  src/primitives/block.cpp
  src/primitives/uint256.cpp
  src/primitives/transaction.cpp
  src/primitives/transaction_serializer.cpp
  src/primitives/hash_domains.cpp
  src/common/sha256d.cpp
  src/common/serialization.cpp
  src/crypto/ripemd160.cpp
)

target_include_directories(dinero_consensus_core PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(dinero_consensus_core PUBLIC
  dinero_logging
  dinero_crypto
  dinero_pq        # Phase 6: P2MR spend validator (VerifyP2MRSpend) in script_validation.cpp
  dinero_shielded  # v7 Shielded pool: commitment tree + nullifier set + validation
  secp256k1
  OpenSSL::Crypto
  zstd  # For Utreexo proof compression
)

if(APPLE)
  target_link_libraries(dinero_consensus_core PRIVATE
    "-framework Security"
    "-framework CoreFoundation"
  )
endif()

set_target_properties(dinero_consensus_core PROPERTIES
  OUTPUT_NAME "dinero-consensus"
)

message(STATUS "✅ dinero_consensus_core (pure library) configured")

# ─────────────────────────────────────────────────────────────────────────
# Layer 0b: Full Consensus Rules (includes persistence adapters)
# ─────────────────────────────────────────────────────────────────────────
# IMPORTANT — RELATIONSHIP TO dinero_consensus_core (above):
#
# dinero_consensus_core (output: dinero-consensus.lib) is the "pure" tier:
# pure math, no DB, no networking, no threading. Targets that want to
# exercise consensus logic in isolation (test_consensus_core_standalone,
# test_utreexo_stump_invariant, future mobile/WASM consumers) link it
# directly. ~30 sources.
#
# dinero_consensus (output: dinero_consensus.lib) is the "full" tier:
# the pure surface PLUS persistence adapters (ChainDB, CoinsViewCache,
# GlobalUTXOSet, block_index, witness_commitment, etc.). dinerod and
# every daemon-side test target link this one.
#
# The two libraries are PARALLEL BY DESIGN, not nested. ~30 source files
# below appear in BOTH libraries (consensus_utxo_set, script,
# script_validation, script_crypto, sighash_bip143, utreexo_accumulator,
# block_filter, filter_commitment, merkle_root, merkle_proof, pow,
# chainwork, difficulty_validation, sigops, freeze, block_undo, etc.).
# This is intentional: it lets either library be linked alone without
# pulling the other's tier. The cost is ~30 files compiled twice — fine
# at ~3s per file, acceptable for the architectural clarity.
#
# Possible future cleanup: make dinero_consensus PUBLIC-dep
# dinero_consensus_core and delete the duplicated sources from the
# add_library() list below. That requires careful link-order auditing
# (today's parallel layout sidesteps several MSVC linker order quirks)
# and would touch consensus-critical paths. Tracked but not blocking.
#
add_library(dinero_consensus STATIC
  src/consensus/block_reward.cpp     # Shared exact reward checks, no daemon/storage dependency
  src/consensus/chainparams_impl.cpp   # Chain parameters (mainnet/testnet/regtest)
  src/consensus/chainwork.cpp          # arith_uint256 for difficulty calculations
  src/consensus/block_index.cpp        # Phase 3B: Block index management and fork choice
  src/consensus/block_undo.cpp         # Block undo data structures
  src/consensus/merkle_root.cpp        # Phase 11a.2: Canonical merkle root computation
  src/consensus/witness_commitment.cpp # Phase 11c.1: Witness commitment structure (DINW magic)
  src/consensus/block_filter.cpp      # BIP158 GCS block filters for compact filter commitment
  src/consensus/filter_commitment.cpp # DNRF coinbase filter commitment (SegWit-style OP_RETURN)
  src/consensus/merkle_proof.cpp      # Merkle inclusion proof for coinbase verification
  src/consensus/utxo_publication.cpp
  src/consensus/consensus_utxo_set.cpp # Phase 2: Pure in-memory UTXO set (no DB, no mutex)
  src/consensus/stateless_verification.cpp # Phase 2.2: Stateless block verification (pure, no DB)
  src/consensus/consensus_invariants.cpp   # Forensic-grade invariant assertions
  src/consensus/tx_parser.cpp          # Transaction parsing
  src/consensus/script_verify.cpp     # Script verification
  src/consensus/script.cpp            # Script class (BIP 141 witness detection)
  src/consensus/script_interpreter.cpp # Script interpreter (VerifyScript, createP2* functions)
  src/consensus/script_crypto.cpp     # Script crypto (SHA256_Hash, HASH160_Hash, etc.)
  src/consensus/script_sighash.cpp    # Sighash calculation for signatures
  src/consensus/crypto/sighash_bip143.cpp  # BIP143 sighash for SegWit v0 (consensus layer)
  src/consensus/script_validation.cpp # Phase F.11: Minimal script validation engine (explicit, not VM-based)
  src/consensus/script_verify_sig.cpp # Signature verification (CheckECDSASignature, CheckSchnorrSignature)
  src/consensus/tapscript_interpreter.cpp  # Tapscript execution (BIP342)
  src/consensus/covenants.cpp             # Phase 28: Covenant opcodes (CTV, CSFS, TXHASH, CCV)
  src/consensus/transaction_validator.cpp  # Transaction validation logic
  src/consensus/tx_validation.cpp          # Phase M.1: validateTransaction() entry point
  src/consensus/signature_cache.cpp        # Phase M.0: Signature verification cache (g_signature_cache)
  src/consensus/script_cache.cpp           # Phase M.0: Script verification cache
  # ❌ REMOVED: chain_manager_global.cpp (Phase N: eliminated g_chain_manager global)
  # NOTE: pow_consensus_engine.cpp moved to dinero_chainstate (needs ChainDB)
  src/consensus/coinbase_maturity.cpp  # Coinbase maturity rules (100 blocks)
  src/consensus/freeze.cpp             # Phase D.4: Consensus freeze + runtime integrity verification
  src/consensus/sigops.cpp             # Signature operation counting (mainnet blocker fix)
  src/consensus/pow.cpp                # Bitcoin Core-compatible PoW verification (MAINNET BLOCKER)
  src/consensus/utreexo_accumulator.cpp  # Phase 34.1: Utreexo forest accumulator
  src/consensus/utreexo_stump.cpp         # Phase 34.2: Utreexo stump (roots-only accumulator)
  src/consensus/utreexo_delta_codec.cpp   # UD sidecar codec + forward replay (campaign phase 2)
  src/consensus/csn_replay_data.cpp      # Historical CSN targets and spent metadata
  src/consensus/cpu_budget_monitor.cpp   # Phase E.2.d: CPU budget monitoring and validation timeouts
  src/consensus/undo.cpp                 # UndoRecord serialization for ChainDB
  src/address/addr_types.cpp           # Address type definitions
  src/crypto/ripemd160.cpp             # Crypto primitives
  src/common/serialization.cpp         # Serialization utilities
  src/primitives/block.cpp             # Block data structures
  src/primitives/uint256.cpp           # 256-bit hash type
  src/primitives/transaction.cpp       # Phase M.0: Canonical Transaction methods (size/weight/serialize)
  src/primitives/transaction_serializer.cpp  # TransactionSerializer helpers (ToHex, FromHex, Deserialize)
  src/primitives/hash_domains.cpp      # Phase M.3: Semantic hash domains (TxId, BlockHash, etc.)
  src/common/sha256d.cpp               # SHA256d hashing
  # Task 2.5 Step 3: pq kernel folded in (its primitive deps, uint256::IsNull
  # and Transaction::GetBaseSize, are defined only in the consensus tier, so
  # declaring them from dinero_pq would close a cycle).
  src/consensus/pq/scheme_registry.cpp
  src/consensus/pq/ml_dsa_65.cpp
  src/consensus/pq/ml_dsa_65_keygen.c
  src/consensus/pq/p2mr_consensus.cpp
)

# Folded ml_dsa_65_keygen.c needs PQClean's internal headers and archive,
# exactly as dinero_pq declares them (src/consensus/pq/CMakeLists.txt).
target_include_directories(dinero_consensus PRIVATE
  ${CMAKE_SOURCE_DIR}/third_party/pqclean/common
  ${CMAKE_SOURCE_DIR}/third_party/pqclean/crypto_sign/ml-dsa-65/clean
)
target_link_libraries(dinero_consensus PRIVATE pqclean_ml_dsa_65)

target_include_directories(dinero_consensus BEFORE PUBLIC
  ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include  # Must come BEFORE system paths to override Homebrew headers
)
target_include_directories(dinero_consensus PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
)

if(APPLE)
  target_link_libraries(dinero_consensus PUBLIC
    dinero_logging
    dinero_crypto
    dinero_zk       # Shielded proof system support
    jsoncpp_static  # Bundled (no Homebrew dependency)
    secp256k1
    OpenSSL::Crypto # script_crypto.cpp uses SHA256/RIPEMD160 directly
    zstd            # For Utreexo proof compression
    RocksDB::rocksdb  # Phase M.1: CoinsDB requires RocksDB (CMake target); export drops at Task 6.1a
  )
else()
  target_link_libraries(dinero_consensus PUBLIC
    dinero_logging
    dinero_crypto
    dinero_zk       # Shielded proof system support
    jsoncpp_static  # Bundled (same as Mac)
    secp256k1
    OpenSSL::Crypto # script_crypto.cpp uses SHA256/RIPEMD160 directly
    zstd            # For Utreexo proof compression
    RocksDB::rocksdb  # Phase M.1: CoinsDB requires RocksDB (CMake target); export drops at Task 6.1a
  )
endif()
