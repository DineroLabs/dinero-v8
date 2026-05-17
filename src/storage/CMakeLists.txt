# Chainstate storage layer.
#
# Keep this target close to the persistence sources it owns. A few adapter
# sources still live outside src/storage because they bridge consensus, wallet,
# and chainstate APIs.

add_library(dinero_chainstate STATIC
  ${CMAKE_SOURCE_DIR}/src/storage/chain_db.cpp             # ChainDB (RocksDB backend)
  ${CMAKE_SOURCE_DIR}/src/storage/persistent_utxo_adapter.cpp  # Bridges consensus to persistence
  ${CMAKE_SOURCE_DIR}/src/storage/block_storage.cpp        # Block/undo flatfile management
  ${CMAKE_SOURCE_DIR}/src/storage/disk_space_monitor.cpp   # Disk space monitoring
  # Phase 39: ChainManager DELETED (header, implementation, stubs all removed)
  # All usage migrated to ChainstateService / ChainDB / BlockStorage.
  # startup_validator.cpp temporarily disabled - needs API fixes.
  ${CMAKE_SOURCE_DIR}/src/wallet/utxo_index.cpp            # UTXO index (uses RocksDB)
  ${CMAKE_SOURCE_DIR}/src/sqlite_open.cpp                  # Minimal SQLite stub (compatibility)
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
  dinero_consensus
  dinero_crypto
  jsoncpp_static
  secp256k1
  RocksDB::rocksdb
)
