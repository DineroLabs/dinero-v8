# Chainstate storage layer.
#
# Keep this target close to the persistence sources it owns. A few adapter
# sources still live outside src/storage because they bridge consensus, wallet,
# and chainstate APIs.

add_library(dinero_chainstate STATIC
  ${CMAKE_SOURCE_DIR}/src/storage/chain_db.cpp             # ChainDB (RocksDB backend)
  ${CMAKE_SOURCE_DIR}/src/storage/chainstate_metadata.cpp  # Header-sync restart metadata
  ${CMAKE_SOURCE_DIR}/src/storage/chain_db_wallet_view.cpp # Wallet chain view over ChainDB (Task 5.2)
  ${CMAKE_SOURCE_DIR}/src/consensus/coins_db.cpp           # CoinsDB (UTXO database backend; moved from consensus, Task 2.1)
  ${CMAKE_SOURCE_DIR}/src/consensus/coins_view_cache.cpp   # CoinsViewCache (L1 UTXO cache; moved from consensus, Task 2.1)
  ${CMAKE_SOURCE_DIR}/src/indexing/utxo_position_index.cpp # Global UTXO -> Utreexo position index (moved from consensus, Task 2.2)
  ${CMAKE_SOURCE_DIR}/src/storage/orchard_state.cpp         # Staged Orchard block state/undo
  ${CMAKE_SOURCE_DIR}/src/storage/legacy_retirement.cpp     # Staged frozen legacy receipt/undo
  ${CMAKE_SOURCE_DIR}/src/storage/persistent_utxo_adapter.cpp  # Bridges consensus to persistence
  ${CMAKE_SOURCE_DIR}/src/storage/block_storage.cpp        # Block/undo flatfile management
  ${CMAKE_SOURCE_DIR}/src/storage/forest_restore.cpp       # Checkpoint + UD-sidecar replay (campaign phase 3)
  ${CMAKE_SOURCE_DIR}/src/storage/checkpoint_retention.cpp # Offline, replay-verified checkpoint retention
  ${CMAKE_SOURCE_DIR}/src/storage/disk_space_monitor.cpp   # Disk space monitoring
  # Phase 39: ChainManager DELETED (header, implementation, stubs all removed)
  # All usage migrated to ChainstateService / ChainDB / BlockStorage.
  # startup_validator.cpp temporarily disabled - needs API fixes.
  ${CMAKE_SOURCE_DIR}/src/consensus/pow_consensus_engine.cpp   # PoW engine (needs ChainDB)
  ${CMAKE_SOURCE_DIR}/src/chainstate/mtp_lookup.cpp        # BIP68 MTP lookup factory
)

target_include_directories(dinero_chainstate BEFORE PUBLIC
  ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
)
target_include_directories(dinero_chainstate PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/src/daemon
)

target_link_libraries(dinero_chainstate PUBLIC
  dinero_logging
  dinero_sqlite
  dinero_consensus
  dinero_crypto
  jsoncpp_static
  secp256k1
  RocksDB::rocksdb
)
