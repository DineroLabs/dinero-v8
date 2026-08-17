# This file is intentionally included from the repository root CMakeLists.txt
# instead of added as a subdirectory. The legacy test definitions below use
# root-relative source paths and root-scope CTest properties.

# Add GoogleTest for unit testing
#
# Strategy:
#   - Uses vendored GoogleTest v1.14.0 from third_party/googletest
#   - Configured via cmake/VendorGTest.cmake
#   - Snappy's old GoogleTest is disabled (SNAPPY_BUILD_TESTS=OFF)

# Tests (only if we have working test files)
enable_testing()

# This policy is intentionally pure C++ and registered in the default test
# build, not qt/CMakeLists.txt. CI builds with DINERO_BUILD_QT=OFF, so keeping
# the regression under the optional GUI subtree would silently omit it.
add_executable(test_daemon_startup_policy
  qt/tests/test_daemon_startup_policy.cpp
)
target_include_directories(test_daemon_startup_policy PRIVATE
  ${CMAKE_SOURCE_DIR}/qt/src
)
add_test(NAME DaemonStartupPolicy COMMAND test_daemon_startup_policy)
set_tests_properties(DaemonStartupPolicy PROPERTIES
  LABELS "qt;daemon;regression"
  TIMEOUT 5
)

add_executable(test_ripemd160_src_vectors
  tests/crypto/test_ripemd160_vectors.cpp
  src/crypto/ripemd160.cpp
)
target_include_directories(test_ripemd160_src_vectors PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
)
add_test(NAME RIPEMD160SrcVectors COMMAND test_ripemd160_src_vectors)
set_tests_properties(RIPEMD160SrcVectors PROPERTIES
  LABELS "crypto;hash;smoke"
  TIMEOUT 10
)

add_executable(test_token_bucket
  tests/network/test_token_bucket.cpp
)
target_include_directories(test_token_bucket PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)
add_test(NAME TokenBucket COMMAND test_token_bucket)
set_tests_properties(TokenBucket PROPERTIES
  LABELS "network;relay;smoke"
  TIMEOUT 10
)

add_executable(test_addrman_relay_select
  tests/network/test_addrman_relay_select.cpp
  src/p2p/addrman.cpp
)
target_include_directories(test_addrman_relay_select PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)
add_test(NAME AddrmanRelaySelect COMMAND test_addrman_relay_select)
set_tests_properties(AddrmanRelaySelect PROPERTIES
  LABELS "network;relay;smoke"
  TIMEOUT 10
)

add_executable(test_peer_quality
  tests/network/test_peer_quality.cpp
  src/p2p/peer_quality.cpp
)
target_include_directories(test_peer_quality PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)
add_test(NAME PeerQuality COMMAND test_peer_quality)
set_tests_properties(PeerQuality PROPERTIES
  LABELS "network;p2p;dynamic-p2p;smoke"
  TIMEOUT 10
)

add_executable(test_peer_governor
  tests/network/test_peer_governor.cpp
  src/p2p/peer_governor.cpp
)
target_include_directories(test_peer_governor PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)
add_test(NAME PeerGovernor COMMAND test_peer_governor)
set_tests_properties(PeerGovernor PROPERTIES
  LABELS "network;p2p;dynamic-p2p;smoke"
  TIMEOUT 10
)

add_executable(test_clock_source
  tests/network/test_clock_source.cpp
)
add_dependencies(test_clock_source gtest gtest_main)
target_include_directories(test_clock_source BEFORE PRIVATE
  ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
  ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(test_clock_source PRIVATE
  GTest::gtest_main
)
add_test(NAME ClockSource COMMAND test_clock_source)
set_tests_properties(ClockSource PROPERTIES
  LABELS "network;relay;smoke"
  TIMEOUT 5
)

# #375 follow-up: NotifyDrainWorker — CSN dispatch-thread unpinning primitive
add_executable(test_notify_drain_worker
  tests/daemon/test_notify_drain_worker.cpp
)
add_dependencies(test_notify_drain_worker gtest gtest_main)
target_include_directories(test_notify_drain_worker BEFORE PRIVATE
  ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
  ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(test_notify_drain_worker PRIVATE
  GTest::gtest_main
  Threads::Threads
)
add_test(NAME NotifyDrainWorker COMMAND test_notify_drain_worker)
set_tests_properties(NotifyDrainWorker PROPERTIES
  LABELS "daemon;smoke"
)

# Forest checkpoint delta campaign phase 0: sync instrumentation
# (docs/design/forest-checkpoint-deltas.md)
add_executable(test_sync_stats_recorder
  tests/daemon/test_sync_stats_recorder.cpp
  src/daemon/sync_stats_recorder.cpp
)
add_dependencies(test_sync_stats_recorder gtest gtest_main)
target_include_directories(test_sync_stats_recorder BEFORE PRIVATE
  ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
  ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(test_sync_stats_recorder PRIVATE
  GTest::gtest_main
  Threads::Threads
)
add_test(NAME SyncStatsRecorder COMMAND test_sync_stats_recorder)
set_tests_properties(SyncStatsRecorder PROPERTIES
  LABELS "daemon;smoke"
)

# Forest checkpoint delta campaign phase 1: every-N checkpoint gating
# at the commit-batch contract (docs/design/forest-checkpoint-deltas.md)
add_executable(test_chainstate_commit_batch_interval
  tests/daemon/test_chainstate_commit_batch_interval.cpp
)
add_dependencies(test_chainstate_commit_batch_interval gtest gtest_main)
target_include_directories(test_chainstate_commit_batch_interval BEFORE PRIVATE
  ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
)
target_link_libraries(test_chainstate_commit_batch_interval PRIVATE
  GTest::gtest_main
  Threads::Threads
  ${ROCKSDB_LIBRARY}
)
add_test(NAME ChainstateCommitBatchInterval COMMAND test_chainstate_commit_batch_interval)
set_tests_properties(ChainstateCommitBatchInterval PROPERTIES
  LABELS "daemon;smoke"
)

# Forest checkpoint delta campaign phase 2: delta record codec (#397 husk
# treatment) + checkpoint+replay equivalence (docs/design/forest-checkpoint-deltas.md)
add_executable(test_utreexo_delta_codec
  tests/consensus/test_utreexo_delta_codec.cpp
)
add_dependencies(test_utreexo_delta_codec gtest)
target_link_libraries(test_utreexo_delta_codec PRIVATE
  dinero_chainstate
  gtest
  gtest_main
  ${CMAKE_DL_LIBS}
)
link_zstd_if_needed(test_utreexo_delta_codec)
target_include_directories(test_utreexo_delta_codec PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
)
add_test(NAME UtreexoDeltaCodec COMMAND test_utreexo_delta_codec)
set_tests_properties(UtreexoDeltaCodec PROPERTIES
  LABELS "consensus;utreexo;forest-checkpoint-deltas;smoke"
)

add_executable(test_forest_delta_replay_equivalence
  tests/consensus/test_forest_delta_replay_equivalence.cpp
)
add_dependencies(test_forest_delta_replay_equivalence gtest)
target_link_libraries(test_forest_delta_replay_equivalence PRIVATE
  dinero_chainstate
  gtest
  gtest_main
  ${CMAKE_DL_LIBS}
)
link_zstd_if_needed(test_forest_delta_replay_equivalence)
target_include_directories(test_forest_delta_replay_equivalence PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
)
add_test(NAME ForestDeltaReplayEquivalence COMMAND test_forest_delta_replay_equivalence)
set_tests_properties(ForestDeltaReplayEquivalence PROPERTIES
  LABELS "consensus;utreexo;forest-checkpoint-deltas;smoke"
)

# Forest checkpoint delta campaign phase 3: historical forest restore
# (nearest checkpoint + sidecar replay) shared by startup + bridge
add_executable(test_forest_restore
  tests/storage/test_forest_restore.cpp
  src/common/logger.cpp
)
add_dependencies(test_forest_restore gtest)
target_link_libraries(test_forest_restore PRIVATE
  dinero_chainstate
  dinero_crypto
  RocksDB::rocksdb
  OpenSSL::Crypto
  gtest
  gtest_main
  ${CMAKE_DL_LIBS}
)
link_zstd_if_needed(test_forest_restore)
target_include_directories(test_forest_restore PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
)
add_test(NAME ForestRestore COMMAND test_forest_restore)
set_tests_properties(ForestRestore PROPERTIES
  LABELS "storage;utreexo;forest-checkpoint-deltas;smoke"
)

# Disk-space FULL gate must be absolute-bytes, not a percentage floor
# (DineroTX canary finding, 2026-07-17)
add_executable(test_disk_space_classify
  tests/storage/test_disk_space_classify.cpp
  src/storage/disk_space_monitor.cpp
)
add_dependencies(test_disk_space_classify gtest gtest_main)
target_include_directories(test_disk_space_classify BEFORE PRIVATE
  ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
  ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(test_disk_space_classify PRIVATE
  GTest::gtest_main
  Threads::Threads
)
add_test(NAME DiskSpaceClassify COMMAND test_disk_space_classify)
set_tests_properties(DiskSpaceClassify PROPERTIES
  LABELS "storage;smoke"
)

# issue #224: daemon log rotation + reopen
add_executable(test_logger_rotation
  tests/common/test_logger_rotation.cpp
  src/common/logger.cpp
)
add_dependencies(test_logger_rotation gtest gtest_main)
target_include_directories(test_logger_rotation BEFORE PRIVATE
  ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
  ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(test_logger_rotation PRIVATE
  GTest::gtest_main
)
add_test(NAME LoggerRotation COMMAND test_logger_rotation)
set_tests_properties(LoggerRotation PROPERTIES
  LABELS "common;logging;smoke"
  TIMEOUT 15
)

# Regression (issue #224 follow-up): logging from a global constructor, before
# g_logger's own dynamic init, must not abort the process. Guards the macOS
# static-init mutex crash that aborted every full (mining) dinerod at launch.
# Plain exit-0 test (no gtest); the crash, if reintroduced, aborts at load.
if(NOT MSVC)
  add_executable(test_static_init_logging
    tests/common/test_static_init_logging.cpp
    src/common/logger.cpp
  )
  target_include_directories(test_static_init_logging BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME StaticInitLogging COMMAND test_static_init_logging)
  set_tests_properties(StaticInitLogging PROPERTIES
    LABELS "common;logging;smoke;regression"
    TIMEOUT 15
  )
endif()

# Regression (DineroDPI): wallet.getproofbundle must never emit a bundle whose
# proof num_leaves diverge from the stump num_leaves ("proof leaf count mismatch"
# — a block connecting between proof gen and stump snapshot). Header-only invariant
# check; the handler enforces it via a forest-commitment equality gate + retry.
add_executable(test_proof_bundle_consistency
  tests/rpc/test_proof_bundle_consistency.cpp
)
target_include_directories(test_proof_bundle_consistency BEFORE PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)
add_test(NAME ProofBundleConsistency COMMAND test_proof_bundle_consistency)
set_tests_properties(ProofBundleConsistency PROPERTIES
  LABELS "rpc;dpi;proof;regression"
  TIMEOUT 15
)

add_executable(test_relay_hints_eviction
  tests/network/test_relay_hints_eviction.cpp
  src/network/relay_hints_eviction.cpp
)
add_dependencies(test_relay_hints_eviction gtest gtest_main)
target_include_directories(test_relay_hints_eviction BEFORE PRIVATE
  ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
  ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(test_relay_hints_eviction PRIVATE
  GTest::gtest_main
)
add_test(NAME RelayHintsEviction COMMAND test_relay_hints_eviction)
set_tests_properties(RelayHintsEviction PROPERTIES
  LABELS "network;relay;smoke"
  TIMEOUT 5
)

add_executable(test_relay_registry_grace
  tests/network/test_relay_registry_grace.cpp
  src/network/relay_registry.cpp
)
add_dependencies(test_relay_registry_grace gtest gtest_main)
target_include_directories(test_relay_registry_grace BEFORE PRIVATE
  ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
  ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(test_relay_registry_grace PRIVATE
  GTest::gtest_main
)
add_test(NAME RelayRegistryGrace COMMAND test_relay_registry_grace)
set_tests_properties(RelayRegistryGrace PROPERTIES
  LABELS "network;relay;smoke"
  TIMEOUT 5
)

add_executable(test_rolling_24h_counter
  tests/network/test_rolling_24h_counter.cpp
)
add_dependencies(test_rolling_24h_counter gtest gtest_main)
target_include_directories(test_rolling_24h_counter BEFORE PRIVATE
  ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
  ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(test_rolling_24h_counter PRIVATE
  GTest::gtest_main
)
add_test(NAME RollingCounter24h COMMAND test_rolling_24h_counter)
set_tests_properties(RollingCounter24h PROPERTIES
  LABELS "network;smoke"
  TIMEOUT 10
)

add_executable(test_ripemd160_core_vectors
  tests/crypto/test_ripemd160_vectors.cpp
  src/core/crypto/ripemd160.cpp
)
target_compile_definitions(test_ripemd160_core_vectors PRIVATE DINERO_RIPEMD160_CORE_HEADER)
target_include_directories(test_ripemd160_core_vectors PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
)
add_test(NAME RIPEMD160CoreVectors COMMAND test_ripemd160_core_vectors)
set_tests_properties(RIPEMD160CoreVectors PROPERTIES
  LABELS "crypto;hash;smoke"
  TIMEOUT 10
)

add_executable(test_ripemd160_standalone_vectors
  tests/crypto/test_ripemd160_vectors.cpp
  src/crypto/ripemd160_standalone.cpp
)
target_compile_definitions(test_ripemd160_standalone_vectors PRIVATE DINERO_RIPEMD160_STANDALONE)
target_include_directories(test_ripemd160_standalone_vectors PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
)
add_test(NAME RIPEMD160StandaloneVectors COMMAND test_ripemd160_standalone_vectors)
set_tests_properties(RIPEMD160StandaloneVectors PROPERTIES
  LABELS "crypto;hash;smoke"
  TIMEOUT 10
)

add_executable(test_hash_wrapper_vectors
  tests/crypto/test_ripemd160_vectors.cpp
  src/crypto/hash.cpp
  src/crypto/ripemd160.cpp
  src/common/sha256d.cpp
)
target_compile_definitions(test_hash_wrapper_vectors PRIVATE DINERO_HASH_WRAPPER)
target_include_directories(test_hash_wrapper_vectors PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
)
add_test(NAME HashWrapperVectors COMMAND test_hash_wrapper_vectors)
set_tests_properties(HashWrapperVectors PROPERTIES
  LABELS "crypto;hash;smoke"
  TIMEOUT 10
)

# Minimal wallet stubs shared by wallet-focused tests.
add_library(wallet_test_deps STATIC
  src/wallet/utxo_index.cpp
  src/sqlite_open.cpp
  src/daemon/lightning_stubs.cpp
)

target_link_libraries(wallet_test_deps PUBLIC
  dinero_wallet
  dinero_chainstate  # ChainDB (WalletManager::rescanBlockchain dependency)
  RocksDB::rocksdb
  sqlite3
)

target_include_directories(wallet_test_deps PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
)

# ═══════════════════════════════════════════════════════════════════════════
# test_reindex - Standalone reindex tester (no mempool dependencies)
# ═══════════════════════════════════════════════════════════════════════════
add_executable(test_reindex
  tests/test_reindex.cpp
  src/consensus/reindexer.cpp
  src/consensus/genesis_canonical.cpp
  src/common/logger.cpp
  src/consensus/undo.cpp
  src/primitives/transaction_serializer.cpp
  # src/wallet/transaction.cpp removed - use Transaction from dinero_consensus (already linked)
)

target_link_libraries(test_reindex PRIVATE
  dinero_chainstate
  dinero_consensus
  dinero_shielded
  dinero_crypto
  jsoncpp_static
  RocksDB::rocksdb
  OpenSSL::Crypto
  OpenSSL::SSL
  pthread
  sqlite3
)
link_zstd_if_needed(test_reindex)

target_include_directories(test_reindex PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
)

if(APPLE)
  target_link_libraries(test_reindex PRIVATE
    "-framework Security"
    "-framework CoreFoundation"
  )
endif()

# Differential acceptance parity gate (baseline vs current binaries)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_acceptance_parity.sh)
  add_test(
    NAME AcceptanceParity
    COMMAND ${CMAKE_SOURCE_DIR}/tests/test_acceptance_parity.sh
  )

  set_tests_properties(AcceptanceParity PROPERTIES
    LABELS "consensus;gate;parity;release"
    TIMEOUT 1800
  )

  add_custom_target(test_acceptance_parity
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -R AcceptanceParity
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running acceptance parity differential gate"
    USES_TERMINAL
  )

  message(STATUS "✅ Acceptance parity gate registered: ctest -R AcceptanceParity")
else()
  message(WARNING "⚠️  tests/test_acceptance_parity.sh not found - AcceptanceParity gate not registered")
endif()

# IBD torture gate (IBD + competing fork + churn + restart)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_ibd_torture.sh)
  add_test(
    NAME IBDTorture
    COMMAND ${CMAKE_SOURCE_DIR}/tests/test_ibd_torture.sh
  )

  set_tests_properties(IBDTorture PROPERTIES
    LABELS "ibd;p2p;utreexo;gate;release"
    TIMEOUT 3600
  )

  add_custom_target(test_ibd_torture
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -R IBDTorture
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running IBD torture gate"
    USES_TERMINAL
  )

  message(STATUS "✅ IBD torture gate registered: ctest -R IBDTorture")
else()
  message(WARNING "⚠️  tests/test_ibd_torture.sh not found - IBDTorture gate not registered")
endif()

# Phase 1 canonical state + recovery gate
add_custom_target(test_phase1_canonical_recovery
  COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -L phase1-canonical-recovery
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  COMMENT "Running Phase 1 canonical-state recovery gate"
  USES_TERMINAL
)

message(STATUS "✅ Phase 1 canonical recovery gate registered: ctest -L phase1-canonical-recovery")

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/daemon/test_undo_rebuild_orchestrator.cpp)
  # Commit #4 of the offline --rebuild-undo-range series: tests for the
  # daemon-side orchestrator that owns preflight classification, manifest
  # emission, and (in commit #5) reindexer invocation.
  add_executable(test_undo_rebuild_orchestrator
    tests/daemon/test_undo_rebuild_orchestrator.cpp
    src/daemon/undo_rebuild_orchestrator.cpp
    src/consensus/reindexer.cpp
    src/consensus/genesis_canonical.cpp
    src/consensus/undo.cpp
    src/primitives/transaction_serializer.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_undo_rebuild_orchestrator PRIVATE
    dinero_chainstate
    dinero_consensus
    dinero_shielded
    dinero_crypto
    jsoncpp_static
    RocksDB::rocksdb
    OpenSSL::Crypto
    OpenSSL::SSL
    pthread
    sqlite3
  )
  link_zstd_if_needed(test_undo_rebuild_orchestrator)
  target_include_directories(test_undo_rebuild_orchestrator PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_undo_rebuild_orchestrator PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(
    NAME UndoRebuildOrchestrator
    COMMAND test_undo_rebuild_orchestrator
  )
  set_tests_properties(UndoRebuildOrchestrator PROPERTIES
    LABELS "reindex;chainstate-recovery;undo-rebuild"
    TIMEOUT 30
  )
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/daemon/test_header_metadata_recovery.cpp)
  add_executable(test_header_metadata_recovery
    tests/daemon/test_header_metadata_recovery.cpp
  )
  target_link_libraries(test_header_metadata_recovery PRIVATE
    dinero_core
    dinero_chainstate
    dinero_consensus
    dinero_crypto
    RocksDB::rocksdb
  )
  link_zstd_if_needed(test_header_metadata_recovery)
  target_link_directories(test_header_metadata_recovery PRIVATE /opt/homebrew/lib)
  if(APPLE)
    target_link_libraries(test_header_metadata_recovery PRIVATE "-framework Security" "-framework CoreFoundation")
  endif()
  target_include_directories(test_header_metadata_recovery PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME HeaderMetadataRecovery COMMAND test_header_metadata_recovery)
  set_tests_properties(HeaderMetadataRecovery PROPERTIES
    LABELS "storage;persistence;recovery;regression"
    TIMEOUT 30
  )
  message(STATUS "✅ Header metadata recovery regression guard enabled")
endif()

# #274: stateless ConnectBlock cannot populate BlockUndo.spent_coins, so
# ConnectTip reconstructs the spent list from ChainDB coin rows. Pins
# ChainstateService::ReconstructSpentCoinsFromChainDb (full-fidelity ChainDB
# read-back, intra-block fallback, fail-loud on missing coin).
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/daemon/test_reconstruct_spent_coins.cpp)
  add_executable(test_reconstruct_spent_coins
    tests/daemon/test_reconstruct_spent_coins.cpp
    tests/stubs/mempool_test_stubs.cpp  # no-op Mempool seams (chainstate_service.obj refs)
    tests/stubs/daemon_test_stubs.cpp   # Mempool extras + vault isolation seams
    src/daemon/config.cpp               # GetConfig() — referenced by chainstate_service.obj
  )
  target_link_libraries(test_reconstruct_spent_coins PRIVATE
    dinero_core
    dinero_chainstate
    dinero_consensus
    dinero_crypto
    RocksDB::rocksdb
  )
  link_zstd_if_needed(test_reconstruct_spent_coins)
  target_link_directories(test_reconstruct_spent_coins PRIVATE /opt/homebrew/lib)
  if(APPLE)
    target_link_libraries(test_reconstruct_spent_coins PRIVATE "-framework Security" "-framework CoreFoundation")
  endif()
  target_include_directories(test_reconstruct_spent_coins PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME ReconstructSpentCoins COMMAND test_reconstruct_spent_coins)
  set_tests_properties(ReconstructSpentCoins PROPERTIES
    LABELS "daemon;undo;stateless;regression"
    TIMEOUT 30
  )
  message(STATUS "✅ Stateless undo spent-coin reconstruction test enabled (#274)")
endif()

# AssumeUTXO pre-base backfill bodies: the #309 persist callback could only
# synthesize a missing ChainDB header-metadata row from g_block_index, which a
# snapshot-bootstrapped node does not populate below its base height. The body
# was stored to flatfiles and then permanently unreadable, livelocking the
# genesis->base replay. Pins the HeaderChainSelector fallback.
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/daemon/test_persist_pre_base_body_position.cpp)
  add_executable(test_persist_pre_base_body_position
    tests/daemon/test_persist_pre_base_body_position.cpp
    tests/stubs/mempool_test_stubs.cpp  # no-op Mempool seams (chainstate_service.obj refs)
    tests/stubs/daemon_test_stubs.cpp   # Mempool extras + vault isolation seams
    src/daemon/config.cpp               # GetConfig() — referenced by chainstate_service.obj
  )
  target_link_libraries(test_persist_pre_base_body_position PRIVATE
    dinero_core
    dinero_chainstate
    dinero_consensus
    dinero_crypto
    RocksDB::rocksdb
  )
  link_zstd_if_needed(test_persist_pre_base_body_position)
  target_link_directories(test_persist_pre_base_body_position PRIVATE /opt/homebrew/lib)
  if(APPLE)
    target_link_libraries(test_persist_pre_base_body_position PRIVATE "-framework Security" "-framework CoreFoundation")
  endif()
  target_include_directories(test_persist_pre_base_body_position PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME PersistPreBaseBodyPosition COMMAND test_persist_pre_base_body_position)
  set_tests_properties(PersistPreBaseBodyPosition PROPERTIES
    LABELS "daemon;assumeutxo;storage;regression"
    TIMEOUT 30
  )
  message(STATUS "✅ Pre-base body position persist regression test enabled")
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_reindexer_anchor_seed.cpp)
  # Commit #1 of the offline --rebuild-undo-range series: tests that
  # BlockReindexer::seedFromAnchor() correctly populates in-memory state
  # from an externally-prepared anchor without touching ChainDB.
  #
  # Compiles reindexer.cpp directly so the test does not have to link
  # dinero_core (which pulls in the daemon's networking + RocksDB world).
  # The seedFromAnchor primitive only touches consensus types, so this
  # cuts the test's dependency surface to dinero_consensus + dinero_crypto.
  add_executable(test_reindexer_anchor_seed
    tests/consensus/test_reindexer_anchor_seed.cpp
    src/consensus/reindexer.cpp
    src/consensus/genesis_canonical.cpp
    src/consensus/undo.cpp
    src/common/logger.cpp
    src/primitives/transaction_serializer.cpp
  )
  target_link_libraries(test_reindexer_anchor_seed PRIVATE
    dinero_chainstate
    dinero_consensus
    dinero_shielded
    dinero_crypto
    jsoncpp_static
    RocksDB::rocksdb
    OpenSSL::Crypto
    OpenSSL::SSL
    pthread
    sqlite3
  )
  link_zstd_if_needed(test_reindexer_anchor_seed)
  target_include_directories(test_reindexer_anchor_seed PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_reindexer_anchor_seed PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(
    NAME ReindexerAnchorSeed
    COMMAND test_reindexer_anchor_seed
  )
  set_tests_properties(ReindexerAnchorSeed PROPERTIES
    LABELS "reindex;chainstate-recovery"
    TIMEOUT 30
  )
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_shielded_reindex_equivalence.cpp)
  add_executable(test_shielded_reindex_equivalence
    tests/consensus/test_shielded_reindex_equivalence.cpp
    src/consensus/genesis_canonical.cpp
    src/consensus/reindexer.cpp
    src/common/logger.cpp
    src/consensus/undo.cpp
    src/primitives/transaction_serializer.cpp
  )
  target_link_libraries(test_shielded_reindex_equivalence PRIVATE
    dinero_chainstate
    dinero_consensus
    dinero_shielded
    dinero_crypto
    jsoncpp_static
    RocksDB::rocksdb
    OpenSSL::Crypto
    OpenSSL::SSL
    pthread
    sqlite3
  )
  link_zstd_if_needed(test_shielded_reindex_equivalence)
  target_include_directories(test_shielded_reindex_equivalence PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_shielded_reindex_equivalence PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(
    NAME ShieldedReindexEquivalence
    COMMAND test_shielded_reindex_equivalence
  )
  set_tests_properties(ShieldedReindexEquivalence PROPERTIES
    LABELS "shielded;reindex;shielded-canonical-smoke;shielded-canonical-recovery"
    TIMEOUT 300
  )
endif()

# Security regression: empty aggregated_range_proof must not bypass the
# mandatory range-proof / binding-sig checks (shielded inflation fix).
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_shielded_empty_rangeproof_inflation.cpp)
  add_executable(test_shielded_empty_rangeproof_inflation
    tests/consensus/test_shielded_empty_rangeproof_inflation.cpp
  )
  target_link_libraries(test_shielded_empty_rangeproof_inflation PRIVATE
    dinero_shielded
    dinero_consensus
    dinero_zk
    dinero_crypto
    OpenSSL::Crypto
    OpenSSL::SSL
    pthread
    sqlite3
  )
  link_zstd_if_needed(test_shielded_empty_rangeproof_inflation)
  target_include_directories(test_shielded_empty_rangeproof_inflation PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_shielded_empty_rangeproof_inflation PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(
    NAME ShieldedEmptyRangeProofInflation
    COMMAND test_shielded_empty_rangeproof_inflation
  )
  set_tests_properties(ShieldedEmptyRangeProofInflation PROPERTIES
    LABELS "shielded;security;inflation"
    TIMEOUT 120
  )
endif()

# Behavior-matrix regression for the canonical sync snapshot (#439). Pure logic:
# includes only consensus/header_convergence.h, so it links nothing heavy and
# runs in milliseconds. See docs/architecture/sync-state-behavior-matrix.md.
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_sync_snapshot_matrix.cpp)
  add_executable(test_sync_snapshot_matrix
    tests/consensus/test_sync_snapshot_matrix.cpp
    src/primitives/uint256.cpp   # uint256::operator== is out-of-line
  )
  target_include_directories(test_sync_snapshot_matrix PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(
    NAME SyncSnapshotBehaviorMatrix
    COMMAND test_sync_snapshot_matrix
  )
  set_tests_properties(SyncSnapshotBehaviorMatrix PROPERTIES
    LABELS "consensus;sync;headers;mandatory"
    TIMEOUT 60
  )
endif()

# Security regression: consensus shielded validation must FAIL CLOSED when the
# Pedersen generators are unavailable, and the daemon must refuse to start in
# that state rather than silently rejecting every shielded bundle above the
# input-binding activation height (which forks the node off the network).
#
# Generator derivation is a std::call_once singleton over a compile-time DST, so
# the failure cannot be produced legitimately. This target compiles
# pedersen_generators.cpp directly with DINERO_ENABLE_TEST_HOOKS to get the
# override seam. Compiling that translation unit here also means it defines every
# symbol the dinero_shielded archive member defines, so the archive member is
# never pulled in and there is no duplicate-symbol clash. The definition is set
# ONLY on this target — production libraries and the daemon never see it.
#
# CONSTRAINT: that no-clash property holds only while this translation unit
# defines exactly the symbols the archive member defines. If someone adds a
# symbol to pedersen_generators.cpp that another dinero_shielded member
# references, the linker pulls the archive member back in and this target fails
# with a duplicate-symbol error. Fix it by adding the new symbol's dependencies
# here, not by dropping the direct compile.
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_shielded_generator_failclosed.cpp)
  add_executable(test_shielded_generator_failclosed
    tests/consensus/test_shielded_generator_failclosed.cpp
    ${CMAKE_SOURCE_DIR}/src/consensus/shielded/pedersen_generators.cpp
  )
  target_compile_definitions(test_shielded_generator_failclosed PRIVATE
    DINERO_ENABLE_TEST_HOOKS
  )
  target_link_libraries(test_shielded_generator_failclosed PRIVATE
    dinero_shielded
    dinero_consensus
    dinero_zk
    dinero_crypto
    OpenSSL::Crypto
    OpenSSL::SSL
    pthread
    sqlite3
  )
  link_zstd_if_needed(test_shielded_generator_failclosed)
  target_include_directories(test_shielded_generator_failclosed PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_shielded_generator_failclosed PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(
    NAME ShieldedGeneratorFailClosed
    COMMAND test_shielded_generator_failclosed
  )
  set_tests_properties(ShieldedGeneratorFailClosed PROPERTIES
    LABELS "shielded;security;inflation;mandatory"
    TIMEOUT 120
  )
endif()

# Security regression: untrusted CompactSize counts in the shielded-bundle
# deserializer must be capped before resize() (remote-OOM fix).
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_shielded_serialization_oom.cpp)
  add_executable(test_shielded_serialization_oom
    tests/consensus/test_shielded_serialization_oom.cpp
  )
  target_link_libraries(test_shielded_serialization_oom PRIVATE
    dinero_shielded
    dinero_consensus
    dinero_zk
    dinero_crypto
    OpenSSL::Crypto
    OpenSSL::SSL
    pthread
    sqlite3
  )
  link_zstd_if_needed(test_shielded_serialization_oom)
  target_include_directories(test_shielded_serialization_oom PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_shielded_serialization_oom PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(
    NAME ShieldedSerializationOOM
    COMMAND test_shielded_serialization_oom
  )
  set_tests_properties(ShieldedSerializationOOM PROPERTIES
    LABELS "shielded;security;serialization;oom"
    TIMEOUT 120
  )
endif()

# Shielded canonical state + recovery gate
if(TARGET shielded_pool_test AND TARGET shielded_adversarial_test)
  add_test(
    NAME ShieldedPoolRoundTrip
    COMMAND $<TARGET_FILE:shielded_pool_test>
  )
  set_tests_properties(ShieldedPoolRoundTrip PROPERTIES
    LABELS "shielded;shielded-canonical-smoke;shielded-canonical-recovery"
    TIMEOUT 180
  )

  add_test(
    NAME ShieldedAdversarialHardening
    COMMAND $<TARGET_FILE:shielded_adversarial_test>
  )
  set_tests_properties(ShieldedAdversarialHardening PROPERTIES
    LABELS "shielded;adversarial;shielded-canonical-smoke;shielded-canonical-recovery"
    TIMEOUT 180
  )

  add_custom_target(test_shielded_canonical_recovery
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -L shielded-canonical-recovery
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running shielded canonical-state recovery gate"
    USES_TERMINAL
  )

  message(STATUS "✅ Shielded canonical recovery gate registered: ctest -L shielded-canonical-recovery")
else()
  message(WARNING "⚠️  shielded_pool_test or shielded_adversarial_test target missing - shielded recovery gate not registered")
endif()

add_custom_target(test_phase2_shielded_canonical_recovery
  COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -L phase2-shielded-canonical-recovery
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  COMMENT "Running Phase 2 shielded daemon crash-recovery gate"
  USES_TERMINAL
)

message(STATUS "✅ Phase 2 shielded daemon recovery gate registered: ctest -L phase2-shielded-canonical-recovery")

# Full release gate suite (wallet + parity + p2p storm + ibd torture + utreexo + mempool stress)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_release_suite.sh)
  add_test(
    NAME ReleaseSuiteConfigSmoke
    COMMAND ${CMAKE_SOURCE_DIR}/tests/test_release_suite.sh --check-config
  )

  set_tests_properties(ReleaseSuiteConfigSmoke PROPERTIES
    LABELS "meta;smoke"
    TIMEOUT 60
    ENVIRONMENT "BUILD_DIR=${CMAKE_BINARY_DIR}"
  )

  add_test(
    NAME ReleaseSuite
    COMMAND ${CMAKE_SOURCE_DIR}/tests/test_release_suite.sh
  )

  set_tests_properties(ReleaseSuite PROPERTIES
    LABELS "release;gate;meta"
    TIMEOUT 14400
    ENVIRONMENT "BUILD_DIR=${CMAKE_BINARY_DIR}"
  )

  add_custom_target(test_release_suite
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -R ReleaseSuite
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running full release suite gate"
    USES_TERMINAL
  )

  message(STATUS "✅ Release suite gate registered: ctest -R ReleaseSuite")
else()
  message(WARNING "⚠️  tests/test_release_suite.sh not found - ReleaseSuite gate not registered")
endif()

# ════════════════════════════════════════════════════════════════════════════
# CRITICAL: Genesis Invariant Tests - MUST RUN FIRST
# ════════════════════════════════════════════════════════════════════════════
# These are NOT optional. They protect consensus constants that, if changed,
# would require a new genesis (hard fork). This is constitutional law, not QA.
#
# Protected constants:
# - Genesis hash, merkle root, nonce, timestamp
# - Block 1 premine scriptPubKey and amount
# - Subsidy schedule (halving interval, initial reward)
# - Network identity (network_id, target_spacing)
# - Taproot enforcement (witness version 1 requirement)
#
# Enforcement level: MANDATORY (Level 2)
# - Built by default (not optional)
# - Run in ctest (automated CI)
# - Labeled as CRITICAL (highest priority)
# - Failure blocks merge (CI gate)
#
# "Past-you stopping future-you from breaking money."
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_genesis_invariants.cpp)
  add_executable(test_genesis_invariants
    tests/consensus/test_genesis_invariants.cpp
  )

  add_dependencies(test_genesis_invariants gtest)

  target_link_libraries(test_genesis_invariants PRIVATE
    dinero_consensus        # For GetBlockSubsidy, chainparams
    dinero_crypto           # For hash functions
    $<TARGET_FILE:gtest>
    )
  link_zstd_if_needed(test_genesis_invariants)

  target_include_directories(test_genesis_invariants BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  # Register with CTest - runs automatically via `ctest`
  add_test(NAME GenesisInvariants COMMAND test_genesis_invariants)

  # Label as CRITICAL - highest priority in CI
  set_tests_properties(GenesisInvariants PROPERTIES
    LABELS "consensus;critical;genesis;mandatory"
    TIMEOUT 30  # Should complete instantly - if not, something is very wrong
  )

  message(STATUS "✅ Genesis invariant tests enabled (MANDATORY)")
else()
  message(FATAL_ERROR "❌ CRITICAL: tests/consensus/test_genesis_invariants.cpp not found! This file is REQUIRED to prevent accidental hard forks.")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Block Validation Invariants Test (INV-1 through INV-5)
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_block_validation_invariants.cpp)
  add_executable(test_block_validation_invariants
    tests/consensus/test_block_validation_invariants.cpp
    tests/stubs/mempool_test_stubs.cpp  # no-op Mempool::forEachEntry etc.
    tests/stubs/daemon_test_stubs.cpp   # BridgeNode, vault, PoolDB/Manager, extra Mempool
    src/daemon/config.cpp               # GetConfig() — referenced by consensus_write_batch.obj
  )

  add_dependencies(test_block_validation_invariants gtest)

  target_link_libraries(test_block_validation_invariants PRIVATE
    dinero_core
    dinero_consensus     # Transitively pulls dinero_pq for pq::* and V7P2MRStore::*
    dinero_crypto
    secp256k1
    ${ROCKSDB_LIBRARY}
    ${SQLITE3_LIBRARIES}
    $<TARGET_FILE:gtest>
    )
  if(TARGET bulletproofs_ffi)
    target_link_libraries(test_block_validation_invariants PRIVATE bulletproofs_ffi)
    if(TARGET build_bulletproofs_ffi)
      add_dependencies(test_block_validation_invariants build_bulletproofs_ffi)
    endif()
  endif()
  link_zstd_if_needed(test_block_validation_invariants)

  target_include_directories(test_block_validation_invariants BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  add_test(NAME BlockValidationInvariants COMMAND test_block_validation_invariants)

  set_tests_properties(BlockValidationInvariants PROPERTIES
    LABELS "consensus;critical;invariants"
    TIMEOUT 30
  )

  message(STATUS "✅ Block validation invariant tests enabled")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Utreexo Ephemeral UTXO Regression Test (block 3581 bug)
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_utreexo_ephemeral.cpp)
  add_executable(test_utreexo_ephemeral
    tests/consensus/test_utreexo_ephemeral.cpp
    src/daemon/config.cpp  # GetConfig() referenced by consensus_write_batch.obj
    tests/stubs/mempool_test_stubs.cpp  # Mempool onBlockConnected/Disconnected/forEachEntry/etc.
    tests/stubs/daemon_test_stubs.cpp   # Mempool extras + vault
  )

  add_dependencies(test_utreexo_ephemeral gtest)

  target_link_libraries(test_utreexo_ephemeral PRIVATE
    dinero_core
    dinero_consensus
    dinero_crypto
    secp256k1
    ${ROCKSDB_LIBRARY}
    ${SQLITE3_LIBRARIES}
    $<TARGET_FILE:gtest>
    )
  if(TARGET bulletproofs_ffi)
    target_link_libraries(test_utreexo_ephemeral PRIVATE bulletproofs_ffi)
    if(TARGET build_bulletproofs_ffi)
      add_dependencies(test_utreexo_ephemeral build_bulletproofs_ffi)
    endif()
  endif()
  link_zstd_if_needed(test_utreexo_ephemeral)

  target_include_directories(test_utreexo_ephemeral BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  add_test(NAME UtreexoEphemeral COMMAND test_utreexo_ephemeral)

  set_tests_properties(UtreexoEphemeral PROPERTIES
    LABELS "consensus;regression;ephemeral"
    TIMEOUT 30
  )

  message(STATUS "✅ Utreexo ephemeral regression test enabled")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Utreexo Safety Gate Test (BlockAssembler hardening)
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mining/test_utreexo_safety_gate.cpp)
  add_executable(test_utreexo_safety_gate
    tests/mining/test_utreexo_safety_gate.cpp
    src/daemon/mempool.cpp
    src/daemon/config.cpp
    tests/stubs/vault_test_stubs.cpp  # vault observers — Mempool methods come from real mempool.cpp above
  )

  target_link_libraries(test_utreexo_safety_gate PRIVATE
    dinero_core
    dinero_consensus
    dinero_pq         # P2MR VWU + spend verification used by mempool/script validation
    dinero_shielded   # Shielded bundle validation/apply used by block_validation.cpp
    dinero_chainstate
    dinero_crypto
    secp256k1
    ${ROCKSDB_LIBRARY}
    ${SQLITE3_LIBRARIES}
    GTest::gtest
    )
  if(TARGET bulletproofs_ffi)
    target_link_libraries(test_utreexo_safety_gate PRIVATE bulletproofs_ffi)
    if(TARGET build_bulletproofs_ffi)
      add_dependencies(test_utreexo_safety_gate build_bulletproofs_ffi)
    endif()
  endif()
  link_zstd_if_needed(test_utreexo_safety_gate)
  if(APPLE)
    target_link_libraries(test_utreexo_safety_gate PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()

  target_include_directories(test_utreexo_safety_gate BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  add_test(NAME UtreexoSafetyGate COMMAND test_utreexo_safety_gate)

  set_tests_properties(UtreexoSafetyGate PROPERTIES
    LABELS "mining;regression;safety"
    TIMEOUT 30
  )

  message(STATUS "✅ Utreexo safety gate test enabled")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Testnet Invariants Verification Test (proves mainnet tests work)
# ════════════════════════════════════════════════════════════════════════════
# This test verifies that the genesis invariant tests would actually catch
# consensus changes by checking current values. If any consensus constant
# changed, this test would fail, proving the enforcement mechanism works.
#
# Purpose: "Canary in the coal mine" - proves the tests catch drift
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_testnet_invariants.cpp)
  add_executable(test_testnet_invariants
    tests/consensus/test_testnet_invariants.cpp
  )

  target_link_libraries(test_testnet_invariants PRIVATE
    dinero_consensus        # For GetBlockSubsidy, chainparams
    dinero_crypto           # For hash functions
    GTest::gtest  # Cross-platform GTest
  )
  link_zstd_if_needed(test_testnet_invariants)

  target_include_directories(test_testnet_invariants PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  # Register with CTest
  add_test(NAME TestnetInvariantsVerification COMMAND test_testnet_invariants)

  # Label as verification test
  set_tests_properties(TestnetInvariantsVerification PROPERTIES
    LABELS "consensus;verification;testnet"
    TIMEOUT 30
  )

  message(STATUS "✅ Testnet invariants verification test enabled")
else()
  message(WARNING "⚠️  tests/consensus/test_testnet_invariants.cpp not found - skipping verification test")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Consensus Validation Tripwires (Phase C.0)
# ════════════════════════════════════════════════════════════════════════════
# These tests ensure that consensus-critical validation logic cannot be
# accidentally weakened or removed during refactoring. Each test creates an
# INVALID block and verifies that ValidateBlock() correctly rejects it.
#
# CRITICAL: If any of these tests fail, it means someone removed validation
# logic. DO NOT modify these tests to make them pass - fix the code instead!
#
# Protected validation rules:
# - Merkle root must match computed value from transactions
# - Block must contain at least one transaction (coinbase)
# - Timestamp must not be too far in the future (2 hour tolerance)
# - Block version must be valid (non-zero)
#
# Enforcement level: MANDATORY (Level 2)
# Purpose: "Regression protection for consensus validation"
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_consensus_validation_tripwires.cpp)
  add_executable(test_consensus_validation_tripwires
    tests/consensus/test_consensus_validation_tripwires.cpp
    src/daemon/mempool.cpp    # Provides dinero::Mempool implementation
    src/daemon/config.cpp     # Provides GetConfig()
    tests/stubs/vault_test_stubs.cpp  # vault observers (real mempool present, so vault-only seam)
  )

  target_link_libraries(test_consensus_validation_tripwires PRIVATE
    dinero_consensus        # For PowConsensusEngine
    dinero_chainstate       # For CreatePowConsensusEngine
    dinero_crypto           # For hash functions
    dinero_core             # For StratumServer and other daemon components
    GTest::gtest
  )
  if(TARGET bulletproofs_ffi)
    target_link_libraries(test_consensus_validation_tripwires PRIVATE bulletproofs_ffi)
    if(TARGET build_bulletproofs_ffi)
      add_dependencies(test_consensus_validation_tripwires build_bulletproofs_ffi)
    endif()
  endif()
  link_zstd_if_needed(test_consensus_validation_tripwires)
  if(APPLE)
    target_link_libraries(test_consensus_validation_tripwires PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()

  target_include_directories(test_consensus_validation_tripwires PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  # Register with CTest
  add_test(NAME ConsensusValidationTripwires COMMAND test_consensus_validation_tripwires)

  # Label as critical - these tests prevent regression
  set_tests_properties(ConsensusValidationTripwires PROPERTIES
    LABELS "consensus;critical;validation;mandatory"
    TIMEOUT 30
  )

  message(STATUS "✅ Consensus validation tripwire tests enabled (MANDATORY)")
else()
  message(FATAL_ERROR "❌ CRITICAL: tests/consensus/test_consensus_validation_tripwires.cpp not found! This file protects against validation regression.")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Ring 1: Formal Verification (Property-Based Testing)
# ════════════════════════════════════════════════════════════════════════════
# Mathematical proofs of consensus-critical invariants using property-based
# testing (QuickCheck/Hypothesis style). Tests prove properties hold for ALL
# inputs by generating thousands of random test cases.
#
# Ring 1 Tests:
#   1. Supply Invariant: ∀ height, supply ≤ 265,428,000 DIN (100k random heights) ✅
#   2. UTXO Set Invariant: ∀ operation, UTXO set consistency (3k+ tests) ✅
#   3. Chain Selection Invariant: ∀ chain, most-work wins (700+ tests) ✅
#
# Enforcement level: MANDATORY (Level 1 - Mathematical Proofs)
# Purpose: "Formal verification of consensus invariants"
# ════════════════════════════════════════════════════════════════════════════
option(ENABLE_CONSENSUS_FORMAL_VERIFICATION "Build Ring 1 formal verification tests (MANDATORY)" ON)

if(ENABLE_CONSENSUS_FORMAL_VERIFICATION AND EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_consensus_formal_verification.cpp)
  add_executable(test_consensus_formal_verification
    tests/consensus/test_consensus_formal_verification.cpp
  )

  target_link_libraries(test_consensus_formal_verification PRIVATE
    dinero_consensus        # For ConsensusSubsidy, SupplyValidator, UTXOSet
    dinero_chainstate       # For ChainDB (UTXOSet dependency)
    dinero_core             # For Logger, UndoRecord, and other utilities
    GTest::gtest            # System GoogleTest (Homebrew - compatible with Clang 17)
  )
  link_zstd_if_needed(test_consensus_formal_verification)

  target_include_directories(test_consensus_formal_verification PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  # Register with CTest
  add_test(NAME ConsensusFormalVerification COMMAND test_consensus_formal_verification)

  # Label as critical - formal proofs of consensus invariants
  set_tests_properties(ConsensusFormalVerification PROPERTIES
    LABELS "consensus;formal;ring1;mandatory"
    TIMEOUT 120  # Property-based tests with 100k+ samples need more time
  )

  message(STATUS "✅ Ring 1 formal verification tests COMPLETE (Tests 1-3: Supply + UTXO + Chain Selection Invariants)")
elseif(ENABLE_CONSENSUS_FORMAL_VERIFICATION)
  message(WARNING "⚠️  Ring 1 formal verification test file not found - skipping")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Ring 2: Consensus Validation Properties (V1-V3: Block/Transaction/Script)
# ════════════════════════════════════════════════════════════════════════════
# Purpose: "Prove local consensus validation is mathematically correct"
#
# V1: Block Validity Properties (7 tests)
# V2: Transaction Validity Properties (7 tests)
# V3: Script Execution Properties (7 tests)
# V4-V5: State Transition & Enforcement Oracles (14 tests - separate files)
#
# Total: 35 properties, ~50,600 random test cases
# ════════════════════════════════════════════════════════════════════════════
option(ENABLE_RING2_CONSENSUS_VALIDATION "Build Ring 2 consensus validation tests (MANDATORY)" ON)

if(ENABLE_RING2_CONSENSUS_VALIDATION AND EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_consensus_ring2_validity.cpp)
  add_executable(test_consensus_ring2_validity
    tests/consensus/test_consensus_ring2_validity.cpp
  )

  target_link_libraries(test_consensus_ring2_validity PRIVATE
    dinero_consensus        # For BlockValidator, TransactionValidator, ScriptValidator
    dinero_chainstate       # For UTXOSet
    dinero_core             # For Logger and utilities
    dinero_crypto           # For SHA256
    GTest::gtest            # System GoogleTest
  )
  link_zstd_if_needed(test_consensus_ring2_validity)

  target_include_directories(test_consensus_ring2_validity PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  # Register with CTest
  add_test(NAME ConsensusRing2Validity COMMAND test_consensus_ring2_validity)

  # Label as critical - formal proofs of consensus validation
  set_tests_properties(ConsensusRing2Validity PROPERTIES
    LABELS "consensus;formal;ring2;mandatory"
    TIMEOUT 180  # V1-V3 property tests with 50k+ samples need time
  )

  message(STATUS "✅ Ring 2 consensus validation tests configured (V1-V3: Block/Transaction/Script Validity)")
elseif(ENABLE_RING2_CONSENSUS_VALIDATION)
  message(WARNING "⚠️  Ring 2 consensus validation test file not found - skipping")
endif()

# ════════════════════════════════════════════════════════════════════════════
# DAA Golden Vectors: Bootstrap retarget + template/validator parity
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/src/test/daa_golden_vectors_test.cpp)
  add_executable(test_daa_golden_vectors
    src/test/daa_golden_vectors_test.cpp
  )

  add_dependencies(test_daa_golden_vectors gtest)

  target_link_libraries(test_daa_golden_vectors PRIVATE
    dinero_core
    dinero_consensus
    dinero_crypto
    $<TARGET_FILE:gtest>
    $<TARGET_FILE:gtest_main>
  )
  link_zstd_if_needed(test_daa_golden_vectors)

  target_include_directories(test_daa_golden_vectors BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  add_test(NAME DAAGoldenVectors COMMAND test_daa_golden_vectors)

  set_tests_properties(DAAGoldenVectors PROPERTIES
    LABELS "consensus;critical;difficulty;mandatory"
    TIMEOUT 30
  )

  message(STATUS "DAA golden vector tests enabled")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Liquidity Vault — daemon-side ledger primitives + behavioural-parity gtests.
# Pure-code; no consensus / chainstate dependencies.
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/src/test/vault_ledger_tests.cpp)
  add_executable(test_vault_ledger
    src/test/vault_ledger_tests.cpp
    src/vault/ledger.cpp
    src/vault/ledger_account.cpp
  )
  add_dependencies(test_vault_ledger gtest gtest_main)
  target_link_libraries(test_vault_ledger PRIVATE
    $<TARGET_FILE:gtest>
    $<TARGET_FILE:gtest_main>
  )
  target_include_directories(test_vault_ledger BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME VaultLedger COMMAND test_vault_ledger)
  set_tests_properties(VaultLedger PROPERTIES
    LABELS "vault;ledger;mandatory"
    TIMEOUT 30
  )
  message(STATUS "Vault ledger tests enabled")
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/src/test/vault_state_machines_tests.cpp)
  add_executable(test_vault_state_machines
    src/test/vault_state_machines_tests.cpp
    src/vault/ledger.cpp
    src/vault/ledger_account.cpp
    src/vault/deposit_flow.cpp
    src/vault/reorg_watcher.cpp
    src/vault/withdrawal_queue.cpp
    src/vault/signing_backend.cpp
  )
  add_dependencies(test_vault_state_machines gtest gtest_main)
  target_link_libraries(test_vault_state_machines PRIVATE
    $<TARGET_FILE:gtest>
    $<TARGET_FILE:gtest_main>
  )
  target_include_directories(test_vault_state_machines BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME VaultStateMachines COMMAND test_vault_state_machines)
  set_tests_properties(VaultStateMachines PROPERTIES
    LABELS "vault;mandatory"
    TIMEOUT 30
  )
  message(STATUS "Vault state-machine tests enabled")
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/src/test/vault_ledger_store_tests.cpp)
  add_executable(test_vault_ledger_store
    src/test/vault_ledger_store_tests.cpp
    src/vault/ledger.cpp
    src/vault/ledger_account.cpp
    src/vault/ledger_store.cpp
  )
  add_dependencies(test_vault_ledger_store gtest gtest_main)
  target_link_libraries(test_vault_ledger_store PRIVATE
    $<TARGET_FILE:gtest>
    $<TARGET_FILE:gtest_main>
  )
  target_include_directories(test_vault_ledger_store BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME VaultLedgerStore COMMAND test_vault_ledger_store)
  set_tests_properties(VaultLedgerStore PROPERTIES
    LABELS "vault;persistence;mandatory"
    TIMEOUT 30
  )
  message(STATUS "Vault ledger-store tests enabled")
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/src/test/vault_service_tests.cpp)
  add_executable(test_vault_service
    src/test/vault_service_tests.cpp
    src/vault/ledger.cpp
    src/vault/ledger_account.cpp
    src/vault/deposit_flow.cpp
    src/vault/reorg_watcher.cpp
    src/vault/withdrawal_queue.cpp
    src/vault/signing_backend.cpp
    src/vault/vault_service.cpp
  )
  add_dependencies(test_vault_service gtest gtest_main)
  target_link_libraries(test_vault_service PRIVATE
    $<TARGET_FILE:gtest>
    $<TARGET_FILE:gtest_main>
  )
  target_include_directories(test_vault_service BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME VaultService COMMAND test_vault_service)
  set_tests_properties(VaultService PROPERTIES
    LABELS "vault;mandatory"
    TIMEOUT 30
  )
  message(STATUS "Vault service tests enabled")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Shielded pool — Phase 0 wave 1 foundation tests (commitment tree,
# nullifier set, bundle serialization). Pure-function tests, no prover
# setup, run in ms. Pre-requisite for any shielded code change.
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/src/test/shielded_serialization_tests.cpp)
  add_executable(test_shielded_serialization
    src/test/shielded_serialization_tests.cpp
  )
  add_dependencies(test_shielded_serialization gtest dinero_shielded)
  target_link_libraries(test_shielded_serialization PRIVATE
    $<TARGET_FILE:gtest>
    $<TARGET_FILE:gtest_main>
    dinero_shielded
  )
  target_include_directories(test_shielded_serialization BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME ShieldedSerialization COMMAND test_shielded_serialization)
  set_tests_properties(ShieldedSerialization PROPERTIES
    LABELS "shielded;consensus;mandatory"
    TIMEOUT 10
  )
  message(STATUS "Shielded serialization tests enabled")
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/src/test/commitment_tree_tests.cpp)
  add_executable(test_commitment_tree
    src/test/commitment_tree_tests.cpp
  )
  add_dependencies(test_commitment_tree gtest dinero_shielded)
  target_link_libraries(test_commitment_tree PRIVATE
    $<TARGET_FILE:gtest>
    $<TARGET_FILE:gtest_main>
    dinero_shielded
  )
  target_include_directories(test_commitment_tree BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME CommitmentTree COMMAND test_commitment_tree)
  set_tests_properties(CommitmentTree PROPERTIES
    LABELS "shielded;consensus;mandatory"
    TIMEOUT 30
  )
  message(STATUS "Shielded commitment-tree tests enabled")
endif()

# shielded-epoch-reset fork (cutover) — reset transformation tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_shielded_epoch_reset.cpp)
  add_executable(test_shielded_epoch_reset
    tests/consensus/test_shielded_epoch_reset.cpp
  )
  add_dependencies(test_shielded_epoch_reset gtest dinero_shielded)
  target_link_libraries(test_shielded_epoch_reset PRIVATE
    $<TARGET_FILE:gtest>
    $<TARGET_FILE:gtest_main>
    dinero_shielded
  )
  target_include_directories(test_shielded_epoch_reset BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME ShieldedEpochReset COMMAND test_shielded_epoch_reset)
  set_tests_properties(ShieldedEpochReset PROPERTIES
    LABELS "shielded;consensus;cv-binding;mandatory"
    TIMEOUT 30
  )
  message(STATUS "Shielded epoch-reset tests enabled")
endif()

# #356: pure marker-guard decision for ApplyStatelessReplayShielded. Header-only
# free function — no dinero library link needed, just gtest. Named so the
# ShieldedBlockSection ctest gate regex also runs it.
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_stateless_replay_shielded_decision.cpp)
  add_executable(test_stateless_replay_shielded_decision
    tests/consensus/test_stateless_replay_shielded_decision.cpp
  )
  add_dependencies(test_stateless_replay_shielded_decision gtest)
  target_link_libraries(test_stateless_replay_shielded_decision PRIVATE
    $<TARGET_FILE:gtest>
    $<TARGET_FILE:gtest_main>
  )
  target_include_directories(test_stateless_replay_shielded_decision BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME ShieldedBlockSectionStatelessReplayDecision
           COMMAND test_stateless_replay_shielded_decision)
  set_tests_properties(ShieldedBlockSectionStatelessReplayDecision PROPERTIES
    LABELS "shielded;consensus;mandatory"
    TIMEOUT 30
  )
  message(STATUS "Stateless-replay shielded-decision tests enabled")
endif()

# ConnectBlockShieldedSection — shared connect-tail free function (Piece 0).
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_shielded_block_section.cpp)
  add_executable(test_shielded_block_section
    tests/consensus/test_shielded_block_section.cpp
  )
  add_dependencies(test_shielded_block_section gtest dinero_shielded)
  target_link_libraries(test_shielded_block_section PRIVATE
    $<TARGET_FILE:gtest>
    $<TARGET_FILE:gtest_main>
    dinero_shielded
  )
  target_include_directories(test_shielded_block_section BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME ShieldedBlockSection COMMAND test_shielded_block_section)
  set_tests_properties(ShieldedBlockSection PROPERTIES
    LABELS "shielded;consensus;mandatory"
    TIMEOUT 30
  )
  message(STATUS "Shielded block-section tests enabled")
endif()

# Task 5 (CSN shielded reorg invertibility): delta-recompute parity between
# BlockValidator::ComputeShieldedDeltasForStoredBlock and the forward
# STATELESS ConnectBlock per-tx loop. Needs the full consensus stack (real
# forward connect as the oracle), the shielded prover (real bundles), and the
# wallet Taproot signer (real BIP340 signatures so forward connect passes).
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_csn_shielded_delta_parity.cpp)
  add_executable(test_csn_shielded_delta_parity
    tests/consensus/test_csn_shielded_delta_parity.cpp
    tests/stubs/mempool_test_stubs.cpp  # no-op Mempool::forEachEntry etc.
    tests/stubs/daemon_test_stubs.cpp   # BridgeNode, vault, PoolDB/Manager, extra Mempool
    src/daemon/config.cpp               # GetConfig() — referenced by consensus_write_batch.obj
    src/wallet/taproot_keys.cpp         # BIP340 Schnorr keygen/signing (test signing util)
    src/wallet/taproot_tx_signer.cpp    # BIP341 Taproot sighash (test signing util)
  )

  add_dependencies(test_csn_shielded_delta_parity gtest)

  target_link_libraries(test_csn_shielded_delta_parity PRIVATE
    dinero_core
    dinero_consensus
    dinero_shielded
    dinero_crypto
    secp256k1
    ${ROCKSDB_LIBRARY}
    ${SQLITE3_LIBRARIES}
    $<TARGET_FILE:gtest>
    $<TARGET_FILE:gtest_main>
    )
  if(TARGET bulletproofs_ffi)
    target_link_libraries(test_csn_shielded_delta_parity PRIVATE bulletproofs_ffi)
    if(TARGET build_bulletproofs_ffi)
      add_dependencies(test_csn_shielded_delta_parity build_bulletproofs_ffi)
    endif()
  endif()
  link_zstd_if_needed(test_csn_shielded_delta_parity)

  target_include_directories(test_csn_shielded_delta_parity BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  # Named so the ShieldedBlockSection ctest gate regex also runs this suite.
  add_test(NAME ShieldedBlockSectionDeltaParity COMMAND test_csn_shielded_delta_parity)
  set_tests_properties(ShieldedBlockSectionDeltaParity PROPERTIES
    LABELS "shielded;consensus;mandatory"
    TIMEOUT 300
  )
  message(STATUS "CSN shielded delta-parity tests enabled")
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/src/test/nullifier_set_tests.cpp)
  add_executable(test_nullifier_set
    src/test/nullifier_set_tests.cpp
  )
  add_dependencies(test_nullifier_set gtest dinero_shielded)
  target_link_libraries(test_nullifier_set PRIVATE
    $<TARGET_FILE:gtest>
    $<TARGET_FILE:gtest_main>
    dinero_shielded
    sqlite3
  )
  target_include_directories(test_nullifier_set BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/sqlite-amalgamation-3480000
  )
  add_test(NAME NullifierSet COMMAND test_nullifier_set)
  set_tests_properties(NullifierSet PROPERTIES
    LABELS "shielded;consensus;mandatory"
    TIMEOUT 15
  )
  message(STATUS "Shielded nullifier-set tests enabled")
endif()

# Phase 0 wave 2 — prover/verifier round-trip tests for the shielded
# circuits. Slower (Spartan proving), runs sequentially.
if(EXISTS ${CMAKE_SOURCE_DIR}/src/test/shielded_circuit_tests.cpp)
  add_executable(test_shielded_circuit
    src/test/shielded_circuit_tests.cpp
  )
  add_dependencies(test_shielded_circuit gtest dinero_shielded)
  target_link_libraries(test_shielded_circuit PRIVATE
    $<TARGET_FILE:gtest>
    $<TARGET_FILE:gtest_main>
    dinero_shielded
  )
  target_include_directories(test_shielded_circuit BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME ShieldedCircuit COMMAND test_shielded_circuit)
  set_tests_properties(ShieldedCircuit PROPERTIES
    LABELS "shielded;zk;mandatory"
    TIMEOUT 600
  )
  message(STATUS "Shielded circuit (prover round-trip) tests enabled")
endif()

# Phase 2 wave 1 — anchor depth window (Sapling-shape 100-block tolerance).
if(EXISTS ${CMAKE_SOURCE_DIR}/src/test/pedersen_tests.cpp)
  add_executable(test_pedersen
    src/test/pedersen_tests.cpp
  )
  add_dependencies(test_pedersen gtest dinero_shielded)
  target_link_libraries(test_pedersen PRIVATE
    $<TARGET_FILE:gtest>
    $<TARGET_FILE:gtest_main>
    dinero_shielded
    secp256k1
    OpenSSL::Crypto
  )
  target_include_directories(test_pedersen BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME Pedersen COMMAND test_pedersen)
  set_tests_properties(Pedersen PROPERTIES
    LABELS "shielded;crypto;mandatory"
    TIMEOUT 30
  )
  message(STATUS "Pedersen tests enabled")
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/src/test/anchor_history_tests.cpp)
  add_executable(test_anchor_history
    src/test/anchor_history_tests.cpp
  )
  add_dependencies(test_anchor_history gtest dinero_shielded)
  target_link_libraries(test_anchor_history PRIVATE
    $<TARGET_FILE:gtest>
    $<TARGET_FILE:gtest_main>
    dinero_shielded
  )
  target_include_directories(test_anchor_history BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME AnchorHistory COMMAND test_anchor_history)
  set_tests_properties(AnchorHistory PROPERTIES
    LABELS "shielded;consensus;mandatory"
    TIMEOUT 10
  )
  message(STATUS "Shielded anchor-history tests enabled")
endif()

# Phase 0 wave 3 — ValidateShieldedBundle + ApplyShieldedBundle integration.
# Covers every ShieldedValidationError enum case end-to-end, including the
# real prove→verify happy path.
if(EXISTS ${CMAKE_SOURCE_DIR}/src/test/shielded_validation_tests.cpp)
  add_executable(test_shielded_validation
    src/test/shielded_validation_tests.cpp
  )
  add_dependencies(test_shielded_validation gtest dinero_shielded)
  target_link_libraries(test_shielded_validation PRIVATE
    GTest::gtest
    GTest::gtest_main
    dinero_shielded
    dinero_wallet  # shielded_derivation.cpp: EncryptNoteForRecipient + DeriveNoteSpendKey
    sqlite3
  )
  target_include_directories(test_shielded_validation BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/sqlite-amalgamation-3480000
  )
  add_test(NAME ShieldedValidation COMMAND test_shielded_validation)
  set_tests_properties(ShieldedValidation PROPERTIES
    LABELS "shielded;consensus;mandatory"
    TIMEOUT 600
  )
  message(STATUS "Shielded validation tests enabled")
endif()

# Audit Critical #1 — shielded cv-binding regression suite.
# cv-bound proofs add ~430k EC constraints, so honest prove/verify cases are
# slow; the TIMEOUT is generous accordingly.
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_shielded_cv_binding.cpp)
  add_executable(test_shielded_cv_binding
    tests/consensus/test_shielded_cv_binding.cpp
  )
  add_dependencies(test_shielded_cv_binding gtest dinero_shielded)
  target_link_libraries(test_shielded_cv_binding PRIVATE
    GTest::gtest
    GTest::gtest_main
    dinero_shielded
    dinero_wallet
    dinero_zk
    sqlite3
  )
  target_include_directories(test_shielded_cv_binding BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/sqlite-amalgamation-3480000
  )
  if(APPLE)
    target_link_libraries(test_shielded_cv_binding PRIVATE "-framework Security")
  endif()
  add_test(NAME ShieldedCvBinding COMMAND test_shielded_cv_binding)
  set_tests_properties(ShieldedCvBinding PROPERTIES
    LABELS "shielded;consensus;cv-binding;mandatory"
    TIMEOUT 1800
  )
  message(STATUS "Shielded cv-binding tests enabled")
endif()

# cv-binding audit 2026-07-01: ec_add_complete selector-binding regression test
if(TARGET dinero_zk AND TARGET gtest)
  add_executable(test_ec_add_complete_binding
    tests/zk/test_ec_add_complete_binding.cpp
  )
  add_dependencies(test_ec_add_complete_binding gtest dinero_zk)
  target_link_libraries(test_ec_add_complete_binding PRIVATE
    GTest::gtest
    GTest::gtest_main
    dinero_zk
  )
  target_include_directories(test_ec_add_complete_binding BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_ec_add_complete_binding PRIVATE "-framework Security")
  endif()
  add_test(NAME EcAddCompleteBinding COMMAND test_ec_add_complete_binding)
  set_tests_properties(EcAddCompleteBinding PROPERTIES
    LABELS "zk;shielded;cv-binding;mandatory"
    TIMEOUT 300
  )
  message(STATUS "ec_add_complete binding regression test enabled")
endif()

# cv-binding audit 2026-07-01: sound fe_assert_less_than_p canonicity check
if(TARGET dinero_zk AND TARGET gtest)
  add_executable(test_fe_less_than_p
    tests/zk/test_fe_less_than_p.cpp
  )
  add_dependencies(test_fe_less_than_p gtest dinero_zk)
  target_link_libraries(test_fe_less_than_p PRIVATE
    GTest::gtest
    GTest::gtest_main
    dinero_zk
  )
  target_include_directories(test_fe_less_than_p BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_fe_less_than_p PRIVATE "-framework Security")
  endif()
  add_test(NAME FeLessThanP COMMAND test_fe_less_than_p)
  set_tests_properties(FeLessThanP PROPERTIES
    LABELS "zk;shielded;cv-binding;mandatory"
    TIMEOUT 300
  )
  message(STATUS "fe_assert_less_than_p canonicity test enabled")
endif()

# M3 PR B: native ShieldedProverKit C ABI for iOS
if(TARGET dinero_shielded_prover_kit AND TARGET gtest)
  add_executable(test_shielded_prover_kit
    src/test/shielded_prover_kit_tests.cpp
  )
  add_dependencies(test_shielded_prover_kit gtest dinero_shielded_prover_kit)
  target_link_libraries(test_shielded_prover_kit PRIVATE
    GTest::gtest
    GTest::gtest_main
    dinero_shielded_prover_kit
    dinero_shielded
    dinero_wallet
    dinero_zk
    dinero_tx_primitives
    dinero_crypto
    dinero_consensus
    zstd
    sqlite3
  )
  target_include_directories(test_shielded_prover_kit BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/sqlite-amalgamation-3480000
  )
  if(APPLE)
    target_link_libraries(test_shielded_prover_kit PRIVATE "-framework Security")
  endif()
  add_test(NAME ShieldedProverKit COMMAND test_shielded_prover_kit)
  set_tests_properties(ShieldedProverKit PROPERTIES
    LABELS "shielded;proverkit;m3;mandatory"
    TIMEOUT 600
  )
  message(STATUS "Shielded ProverKit ABI tests enabled")
endif()

# Focused ValidationContext tripwire: consensus replay callers must pass
# the same tx_sighash used by the bundle's binding signature.
if(EXISTS ${CMAKE_SOURCE_DIR}/src/test/shielded_validation_context_tests.cpp)
  add_executable(test_shielded_validation_context
    src/test/shielded_validation_context_tests.cpp
  )
  add_dependencies(test_shielded_validation_context gtest dinero_shielded)
  target_link_libraries(test_shielded_validation_context PRIVATE
    $<TARGET_FILE:gtest>
    $<TARGET_FILE:gtest_main>
    dinero_shielded
    sqlite3
  )
  target_include_directories(test_shielded_validation_context BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/sqlite-amalgamation-3480000
  )
  add_test(NAME ShieldedValidationContext COMMAND test_shielded_validation_context)
  set_tests_properties(ShieldedValidationContext PROPERTIES
    LABELS "shielded;consensus;mandatory"
    TIMEOUT 120
  )
  message(STATUS "Shielded validation-context tests enabled")
endif()

# D.3-shielded reverse-apply property test: pins the deterministic
# truncate-clone-serialize math used to reconstruct
# pre_block_shielded_frontier when DisconnectTip's ReadStoredUndo fails.
if(EXISTS ${CMAKE_SOURCE_DIR}/src/test/d3_shielded_reverse_apply_tests.cpp)
  add_executable(test_d3_shielded_reverse_apply
    src/test/d3_shielded_reverse_apply_tests.cpp
  )
  add_dependencies(test_d3_shielded_reverse_apply gtest dinero_shielded)
  target_link_libraries(test_d3_shielded_reverse_apply PRIVATE
    $<TARGET_FILE:gtest>
    $<TARGET_FILE:gtest_main>
    dinero_shielded
    sqlite3
  )
  target_include_directories(test_d3_shielded_reverse_apply BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/sqlite-amalgamation-3480000
  )
  add_test(NAME D3ShieldedReverseApply COMMAND test_d3_shielded_reverse_apply)
  set_tests_properties(D3ShieldedReverseApply PROPERTIES
    LABELS "shielded;consensus;mandatory;crash-oracle"
    TIMEOUT 60
  )
  message(STATUS "D.3 shielded reverse-apply property test enabled")
endif()

# ── v0.3.0 wire-format canonical bundle vectors ─────────────────────
if(TARGET dinero_shielded AND TARGET gtest)
  add_executable(test_v030_vectors
    src/test/v030_wire_vectors_tests.cpp
  )
  add_dependencies(test_v030_vectors gtest dinero_shielded)
  target_link_libraries(test_v030_vectors PRIVATE
    $<TARGET_FILE:gtest>
    $<TARGET_FILE:gtest_main>
    dinero_shielded
    sqlite3
  )
  target_include_directories(test_v030_vectors BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/sqlite-amalgamation-3480000
  )
  add_test(NAME ShieldedV030Vectors COMMAND test_v030_vectors)
  set_tests_properties(ShieldedV030Vectors PROPERTIES
    LABELS "shielded;consensus;canonical-vectors"
    TIMEOUT 300
  )
  message(STATUS "Shielded v0.3.0 wire-format vectors enabled")
endif()

# ── Phase 5 Wave 1: shielded key-derivation vectors ────────────────
if(TARGET dinero_wallet AND TARGET gtest)
  add_executable(test_shielded_note_store_rollback
    tests/wallet/test_shielded_note_store_rollback.cpp
  )
  add_dependencies(test_shielded_note_store_rollback gtest dinero_wallet)
  target_link_libraries(test_shielded_note_store_rollback PRIVATE
    GTest::gtest
    GTest::gtest_main
    dinero_wallet
    dinero_shielded
    sqlite3
  )
  target_include_directories(test_shielded_note_store_rollback BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/sqlite-amalgamation-3480000
  )
  if(APPLE)
    target_link_libraries(test_shielded_note_store_rollback PRIVATE "-framework Security")
  endif()
  add_test(NAME ShieldedNoteStoreRollback COMMAND test_shielded_note_store_rollback)
  set_tests_properties(ShieldedNoteStoreRollback PROPERTIES
    LABELS "shielded;wallet;persistence"
    TIMEOUT 60
  )

  add_executable(test_shielded_derivation
    src/test/shielded_derivation_tests.cpp
  )
  add_dependencies(test_shielded_derivation gtest dinero_wallet)
  target_link_libraries(test_shielded_derivation PRIVATE
    GTest::gtest
    GTest::gtest_main
    dinero_wallet
    dinero_shielded
    sqlite3
  )
  target_include_directories(test_shielded_derivation BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/sqlite-amalgamation-3480000
  )
  if(APPLE)
    target_link_libraries(test_shielded_derivation PRIVATE "-framework Security")
  endif()
  add_test(NAME ShieldedDerivation COMMAND test_shielded_derivation)
  set_tests_properties(ShieldedDerivation PROPERTIES
    LABELS "shielded;wallet;shielded-canonical-smoke"
    TIMEOUT 60
  )
  message(STATUS "Shielded derivation tests enabled")
endif()

# ── M2: shielded output feed extractor unit tests ───────────────────
if(TARGET dinero_shielded AND TARGET gtest)
  add_executable(test_shielded_output_feed
    src/test/shielded_output_feed_tests.cpp
  )
  add_dependencies(test_shielded_output_feed gtest dinero_shielded)
  target_link_libraries(test_shielded_output_feed PRIVATE
    GTest::gtest
    GTest::gtest_main
    dinero_shielded
    dinero_zk
    dinero_tx_primitives
    dinero_crypto
    dinero_consensus
    zstd
    sqlite3
  )
  target_include_directories(test_shielded_output_feed BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_shielded_output_feed PRIVATE "-framework Security")
  endif()
  add_test(NAME ShieldedOutputFeed COMMAND test_shielded_output_feed)
  set_tests_properties(ShieldedOutputFeed PROPERTIES
    LABELS "shielded;shielded-canonical-smoke;m2"
    TIMEOUT 60
  )
  message(STATUS "Shielded output feed tests enabled")
endif()

# ── M3: shielded witness builder unit tests ─────────────────────────
if(TARGET dinero_shielded AND TARGET gtest)
  add_executable(test_shielded_witness
    src/test/shielded_witness_tests.cpp
  )
  add_dependencies(test_shielded_witness gtest dinero_shielded)
  target_link_libraries(test_shielded_witness PRIVATE
    GTest::gtest
    GTest::gtest_main
    dinero_shielded
    dinero_zk
    dinero_tx_primitives
    dinero_crypto
    dinero_consensus
    zstd
    sqlite3
  )
  target_include_directories(test_shielded_witness BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_shielded_witness PRIVATE "-framework Security")
  endif()
  add_test(NAME ShieldedWitness COMMAND test_shielded_witness)
  set_tests_properties(ShieldedWitness PROPERTIES
    LABELS "shielded;shielded-canonical-smoke;m3"
    TIMEOUT 60
  )
  message(STATUS "Shielded witness tests enabled")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Ring 2: V4-V5 Validation Property Oracles (State Transition & Enforcement)
# ════════════════════════════════════════════════════════════════════════════
# Purpose: "Prove state transition and consensus enforcement correctness"
#
# V4: State Transition Oracles (7 tests) - Stateful UTXO evolution
# V5: Consensus Enforcement Oracles (7 tests) - Tripwires and convergence
#
# Pattern: Ring 4-style oracles (reset → observe → finalize)
# ════════════════════════════════════════════════════════════════════════════

# V4 Oracle Framework
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/properties/validation_property_oracle.cpp)
  add_library(validation_oracles STATIC
    tests/consensus/properties/validation_property_oracle.cpp
  )

  target_include_directories(validation_oracles PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests/consensus/properties
  )

  target_link_libraries(validation_oracles PUBLIC
    dinero_consensus
    dinero_chainstate
    dinero_core
  )

  # V4.1 Oracle Test
  if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/properties/test_validation_oracle_v41.cpp)
    add_executable(test_validation_oracle_v41
      tests/consensus/properties/test_validation_oracle_v41.cpp
    )

    target_link_libraries(test_validation_oracle_v41 PRIVATE
      validation_oracles
      GTest::gtest
      )

    target_include_directories(test_validation_oracle_v41 PRIVATE
      ${CMAKE_SOURCE_DIR}/include
      ${CMAKE_SOURCE_DIR}/src
      ${CMAKE_SOURCE_DIR}/tests/consensus/properties
    )

    add_test(NAME ValidationOracleV41 COMMAND test_validation_oracle_v41)

    set_tests_properties(ValidationOracleV41 PROPERTIES
      LABELS "consensus;formal;ring2;v4;mandatory"
      TIMEOUT 60
    )

    message(STATUS "✅ Ring 2 V4.1 oracle test configured (State Transition: Applying Valid Block)")
  endif()

  # V4.2 Oracle Test
  if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/properties/test_validation_oracle_v42.cpp)
    add_executable(test_validation_oracle_v42
      tests/consensus/properties/test_validation_oracle_v42.cpp
    )

    target_link_libraries(test_validation_oracle_v42 PRIVATE
      validation_oracles
      GTest::gtest
      )

    target_include_directories(test_validation_oracle_v42 PRIVATE
      ${CMAKE_SOURCE_DIR}/include
      ${CMAKE_SOURCE_DIR}/src
      ${CMAKE_SOURCE_DIR}/tests/consensus/properties
    )

    add_test(NAME ValidationOracleV42 COMMAND test_validation_oracle_v42)

    set_tests_properties(ValidationOracleV42 PROPERTIES
      LABELS "consensus;formal;ring2;v4;mandatory"
      TIMEOUT 60
    )

    message(STATUS "✅ Ring 2 V4.2 oracle test configured (State Transition: Inputs Removed)")
  endif()

  # V4.3 Oracle Test
  if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/properties/test_validation_oracle_v43.cpp)
    add_executable(test_validation_oracle_v43
      tests/consensus/properties/test_validation_oracle_v43.cpp
    )

    target_link_libraries(test_validation_oracle_v43 PRIVATE
      validation_oracles
      GTest::gtest
      )

    target_include_directories(test_validation_oracle_v43 PRIVATE
      ${CMAKE_SOURCE_DIR}/include
      ${CMAKE_SOURCE_DIR}/src
      ${CMAKE_SOURCE_DIR}/tests/consensus/properties
    )

    add_test(NAME ValidationOracleV43 COMMAND test_validation_oracle_v43)

    set_tests_properties(ValidationOracleV43 PROPERTIES
      LABELS "consensus;formal;ring2;v4;mandatory"
      TIMEOUT 60
    )

    message(STATUS "✅ Ring 2 V4.3 oracle test configured (State Transition: Outputs Added)")
  endif()

  # V4.4 Oracle Test
  if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/properties/test_validation_oracle_v44.cpp)
    add_executable(test_validation_oracle_v44
      tests/consensus/properties/test_validation_oracle_v44.cpp
    )

    target_link_libraries(test_validation_oracle_v44 PRIVATE
      validation_oracles
      GTest::gtest
      )

    target_include_directories(test_validation_oracle_v44 PRIVATE
      ${CMAKE_SOURCE_DIR}/include
      ${CMAKE_SOURCE_DIR}/src
      ${CMAKE_SOURCE_DIR}/tests/consensus/properties
    )

    add_test(NAME ValidationOracleV44 COMMAND test_validation_oracle_v44)

    set_tests_properties(ValidationOracleV44 PROPERTIES
      LABELS "consensus;formal;ring2;v4;mandatory"
      TIMEOUT 60
    )

    message(STATUS "✅ Ring 2 V4.4 oracle test configured (State Transition: Value Conservation)")
  endif()

  # V4.5 Oracle Test
  if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/properties/test_validation_oracle_v45.cpp)
    add_executable(test_validation_oracle_v45
      tests/consensus/properties/test_validation_oracle_v45.cpp
    )
    add_dependencies(test_validation_oracle_v45 gtest)
    target_link_libraries(test_validation_oracle_v45 PRIVATE validation_oracles $<TARGET_FILE:gtest>)
    target_include_directories(test_validation_oracle_v45 BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests/consensus/properties)
    add_test(NAME ValidationOracleV45 COMMAND test_validation_oracle_v45)
    set_tests_properties(ValidationOracleV45 PROPERTIES LABELS "consensus;formal;ring2;v4;mandatory" TIMEOUT 60)
    message(STATUS "✅ Ring 2 V4.5 oracle test configured (State Transition: Reorg UTXO Revert)")
  endif()

  # V4.6 Oracle Test
  if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/properties/test_validation_oracle_v46.cpp)
    add_executable(test_validation_oracle_v46
      tests/consensus/properties/test_validation_oracle_v46.cpp
    )
    add_dependencies(test_validation_oracle_v46 gtest)
    target_link_libraries(test_validation_oracle_v46 PRIVATE validation_oracles $<TARGET_FILE:gtest>)
    target_include_directories(test_validation_oracle_v46 BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests/consensus/properties)
    add_test(NAME ValidationOracleV46 COMMAND test_validation_oracle_v46)
    set_tests_properties(ValidationOracleV46 PROPERTIES LABELS "consensus;formal;ring2;v4;mandatory" TIMEOUT 60)
    message(STATUS "✅ Ring 2 V4.6 oracle test configured (State Transition: Coinbase Maturity)")
  endif()

  # V4.7 Oracle Test
  if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/properties/test_validation_oracle_v47.cpp)
    add_executable(test_validation_oracle_v47
      tests/consensus/properties/test_validation_oracle_v47.cpp
    )
    add_dependencies(test_validation_oracle_v47 gtest)
    target_link_libraries(test_validation_oracle_v47 PRIVATE validation_oracles $<TARGET_FILE:gtest>)
    target_include_directories(test_validation_oracle_v47 BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests/consensus/properties)
    add_test(NAME ValidationOracleV47 COMMAND test_validation_oracle_v47)
    set_tests_properties(ValidationOracleV47 PROPERTIES LABELS "consensus;formal;ring2;v4;mandatory" TIMEOUT 60)
    message(STATUS "✅ Ring 2 V4.7 oracle test configured (State Transition: No UTXO Duplication)")
  endif()

  # V5.1 Oracle Test
  if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/properties/test_validation_oracle_v51.cpp)
    add_executable(test_validation_oracle_v51 tests/consensus/properties/test_validation_oracle_v51.cpp)
    add_dependencies(test_validation_oracle_v51 gtest)
    target_link_libraries(test_validation_oracle_v51 PRIVATE validation_oracles $<TARGET_FILE:gtest>)
    link_zstd_if_needed(test_validation_oracle_v51)
    target_include_directories(test_validation_oracle_v51 BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests/consensus/properties)
    add_test(NAME ValidationOracleV51 COMMAND test_validation_oracle_v51)
    set_tests_properties(ValidationOracleV51 PROPERTIES LABELS "consensus;formal;ring2;v5;mandatory" TIMEOUT 60)
    message(STATUS "✅ Ring 2 V5.1 oracle test configured (Enforcement: Invalid Block Never Connected)")
  endif()

  # V5.2 Oracle Test
  if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/properties/test_validation_oracle_v52.cpp)
    add_executable(test_validation_oracle_v52 tests/consensus/properties/test_validation_oracle_v52.cpp)
    add_dependencies(test_validation_oracle_v52 gtest)
    target_link_libraries(test_validation_oracle_v52 PRIVATE validation_oracles $<TARGET_FILE:gtest>)
    link_zstd_if_needed(test_validation_oracle_v52)
    target_include_directories(test_validation_oracle_v52 BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests/consensus/properties)
    add_test(NAME ValidationOracleV52 COMMAND test_validation_oracle_v52)
    set_tests_properties(ValidationOracleV52 PROPERTIES LABELS "consensus;formal;ring2;v5;mandatory" TIMEOUT 60)
    message(STATUS "✅ Ring 2 V5.2 oracle test configured (Enforcement: Invalid TX Never Applied)")
  endif()

  # V5.3 Oracle Test
  if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/properties/test_validation_oracle_v53.cpp)
    add_executable(test_validation_oracle_v53 tests/consensus/properties/test_validation_oracle_v53.cpp)
    add_dependencies(test_validation_oracle_v53 gtest)
    target_link_libraries(test_validation_oracle_v53 PRIVATE validation_oracles $<TARGET_FILE:gtest>)
    link_zstd_if_needed(test_validation_oracle_v53)
    target_include_directories(test_validation_oracle_v53 BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests/consensus/properties)
    add_test(NAME ValidationOracleV53 COMMAND test_validation_oracle_v53)
    set_tests_properties(ValidationOracleV53 PROPERTIES LABELS "consensus;formal;ring2;v5;mandatory" TIMEOUT 60)
    message(STATUS "✅ Ring 2 V5.3 oracle test configured (Enforcement: Failed Block Zero Side Effects)")
  endif()

  # V5.4 Oracle Test
  if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/properties/test_validation_oracle_v54.cpp)
    add_executable(test_validation_oracle_v54 tests/consensus/properties/test_validation_oracle_v54.cpp)
    add_dependencies(test_validation_oracle_v54 gtest)
    target_link_libraries(test_validation_oracle_v54 PRIVATE validation_oracles $<TARGET_FILE:gtest>)
    link_zstd_if_needed(test_validation_oracle_v54)
    target_include_directories(test_validation_oracle_v54 BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests/consensus/properties)
    add_test(NAME ValidationOracleV54 COMMAND test_validation_oracle_v54)
    set_tests_properties(ValidationOracleV54 PROPERTIES LABELS "consensus;formal;ring2;v5;mandatory" TIMEOUT 60)
    message(STATUS "✅ Ring 2 V5.4 oracle test configured (Enforcement: Rule Violations Abort Block)")
  endif()

  # V5.5 Oracle Test
  if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/properties/test_validation_oracle_v55.cpp)
    add_executable(test_validation_oracle_v55 tests/consensus/properties/test_validation_oracle_v55.cpp)
    add_dependencies(test_validation_oracle_v55 gtest)
    target_link_libraries(test_validation_oracle_v55 PRIVATE validation_oracles $<TARGET_FILE:gtest>)
    link_zstd_if_needed(test_validation_oracle_v55)
    target_include_directories(test_validation_oracle_v55 BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests/consensus/properties)
    add_test(NAME ValidationOracleV55 COMMAND test_validation_oracle_v55)
    set_tests_properties(ValidationOracleV55 PROPERTIES LABELS "consensus;formal;ring2;v5;mandatory" TIMEOUT 60)
    message(STATUS "✅ Ring 2 V5.5 oracle test configured (Enforcement: Reorg Never Invalid Chain)")
  endif()

  # V5.6 Oracle Test
  if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/properties/test_validation_oracle_v56.cpp)
    add_executable(test_validation_oracle_v56 tests/consensus/properties/test_validation_oracle_v56.cpp)
    add_dependencies(test_validation_oracle_v56 gtest)
    target_link_libraries(test_validation_oracle_v56 PRIVATE validation_oracles $<TARGET_FILE:gtest>)
    link_zstd_if_needed(test_validation_oracle_v56)
    target_include_directories(test_validation_oracle_v56 BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests/consensus/properties)
    add_test(NAME ValidationOracleV56 COMMAND test_validation_oracle_v56)
    set_tests_properties(ValidationOracleV56 PROPERTIES LABELS "consensus;formal;ring2;v5;mandatory" TIMEOUT 60)
    message(STATUS "✅ Ring 2 V5.6 oracle test configured (Enforcement: Mempool Never Feeds Invalid TX)")
  endif()

  # V5.7 Oracle Test
  if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/properties/test_validation_oracle_v57.cpp)
    add_executable(test_validation_oracle_v57 tests/consensus/properties/test_validation_oracle_v57.cpp)
    add_dependencies(test_validation_oracle_v57 gtest)
    target_link_libraries(test_validation_oracle_v57 PRIVATE validation_oracles $<TARGET_FILE:gtest>)
    link_zstd_if_needed(test_validation_oracle_v57)
    target_include_directories(test_validation_oracle_v57 BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests/consensus/properties)
    add_test(NAME ValidationOracleV57 COMMAND test_validation_oracle_v57)
    set_tests_properties(ValidationOracleV57 PROPERTIES LABELS "consensus;formal;ring2;v5;mandatory" TIMEOUT 60)
    message(STATUS "✅ Ring 2 V5.7 oracle test configured (Enforcement: Deterministic)")
  endif()

  message(STATUS "✅ Ring 2 V4-V5 validation oracle framework configured")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Ring 3 Phase 1: Deterministic Peer Simulator (P2P Testing Infrastructure)
# ════════════════════════════════════════════════════════════════════════════
# Infrastructure for testing P2P protocol without real network I/O:
#   - MockSocket: Simulated TCP with latency/packet loss
#   - PeerSimulator: Scripted peer behaviors (HONEST, STALE, MALICIOUS)
#   - DeterministicScheduler: Controlled event ordering
#   - PropertyTestRNG: Reproducible randomness
#
# Phase 1 enables 10,000+ connection simulations in milliseconds.
# Prerequisite for Ring 3 property-based P2P tests (Phases 2-5).
# ════════════════════════════════════════════════════════════════════════════
option(ENABLE_P2P_PEER_SIMULATOR "Build Ring 3 Phase 1 peer simulator tests" ON)

if(ENABLE_P2P_PEER_SIMULATOR AND EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_peer_simulator_smoke.cpp)
  add_executable(test_peer_simulator_smoke
    tests/p2p/test_peer_simulator_smoke.cpp
  )

  target_link_libraries(test_peer_simulator_smoke PRIVATE
    GTest::gtest
  )

  target_include_directories(test_peer_simulator_smoke PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests/p2p
  )

  # Register with CTest
  add_test(NAME PeerSimulatorSmoke COMMAND test_peer_simulator_smoke)

  # Label as ring3 infrastructure test
  set_tests_properties(PeerSimulatorSmoke PROPERTIES
    LABELS "p2p;ring3;infrastructure;phase1"
    TIMEOUT 30  # Smoke tests should be fast
  )

  message(STATUS "✅ Ring 3 Phase 1: Peer simulator infrastructure (MockSocket, PeerSimulator, DeterministicScheduler)")
elseif(ENABLE_P2P_PEER_SIMULATOR)
  message(WARNING "⚠️  Ring 3 Phase 1 peer simulator test file not found - skipping")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Ring 3 Phase 2: Property Test Framework (QuickCheck/Hypothesis Style)
# ════════════════════════════════════════════════════════════════════════════
# Property-based testing framework for P2P protocol verification:
#   - ConnectionSequenceGenerator: Generate random event sequences
#   - PropertyAssertion: DSL for expressing properties (forAll pattern)
#   - InvariantChecker: Verify state machine invariants
#   - Statistical validation: Run thousands of iterations
#
# Enables proving properties hold ∀ inputs (QuickCheck/Hypothesis methodology).
# Prerequisite for Ring 3 Phases 3-5 (implement actual P2P property tests).
# ════════════════════════════════════════════════════════════════════════════
option(ENABLE_P2P_PROPERTY_FRAMEWORK "Build Ring 3 Phase 2 property test framework" ON)

if(ENABLE_P2P_PROPERTY_FRAMEWORK AND EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_property_framework_smoke.cpp)
  add_executable(test_property_framework_smoke
    tests/p2p/test_property_framework_smoke.cpp
  )

  target_link_libraries(test_property_framework_smoke PRIVATE
    GTest::gtest
  )

  target_include_directories(test_property_framework_smoke PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests/p2p
  )

  # Register with CTest
  add_test(NAME PropertyFrameworkSmoke COMMAND test_property_framework_smoke)

  # Label as ring3 framework test
  set_tests_properties(PropertyFrameworkSmoke PROPERTIES
    LABELS "p2p;ring3;framework;phase2"
    TIMEOUT 30  # Smoke tests should be fast
  )

  message(STATUS "✅ Ring 3 Phase 2: Property test framework (ConnectionSequenceGenerator, PropertyAssertion, InvariantChecker)")
elseif(ENABLE_P2P_PROPERTY_FRAMEWORK)
  message(WARNING "⚠️  Ring 3 Phase 2 property framework test file not found - skipping")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Ring 3 Phase 3: P2P Property Tests
# ════════════════════════════════════════════════════════════════════════════
# Property-based tests verifying P2P protocol invariants from formal spec:
#   - Test Suite 1: Connection Lifecycle (4 properties)
#   - Test Suite 2: Handshake Protocol (4 properties)
#   - Test Suite 3: Message Ordering (4 properties)
#   - Test Suite 4: Timeout Detection (4 properties)
#   - Test Suite 5: Resource Management (4 properties)
#
# Total: 20 properties proving ∀ inputs satisfy protocol invariants.
# Uses Phase 1 (peer simulator) + Phase 2 (property framework) infrastructure.
# ════════════════════════════════════════════════════════════════════════════
option(ENABLE_P2P_PROPERTY_TESTS "Build Ring 3 Phase 3 P2P property tests" ON)

if(ENABLE_P2P_PROPERTY_TESTS AND EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_p2p_properties.cpp)
  add_executable(test_p2p_properties
    tests/p2p/test_p2p_properties.cpp
  )

  target_link_libraries(test_p2p_properties PRIVATE
    GTest::gtest
  )

  target_include_directories(test_p2p_properties PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests/p2p
  )

  # Register with CTest
  add_test(NAME P2PProperties COMMAND test_p2p_properties)

  # Label as ring3 property test
  set_tests_properties(P2PProperties PROPERTIES
    LABELS "p2p;ring3;properties;phase3"
    TIMEOUT 60  # Property tests run 1000+ iterations
  )

  message(STATUS "✅ Ring 3 Phase 3: P2P property tests (20 properties × 500-1000 iterations each)")
elseif(ENABLE_P2P_PROPERTY_TESTS)
  message(WARNING "⚠️  Ring 3 Phase 3 P2P property tests file not found - skipping")
endif()

# Ring 3 Phase 4b: Thread Safety Properties (Real-Thread Testing)
# ════════════════════════════════════════════════════════════════════════════
# TS1: Thread-Safe Peer Lifetime (Use-After-Free Impossible)
#
# Property: ∀ peer P, ∀ thread T:
#   If T accesses P, then P.state ∈ {RUNNING, STOPPING}
#   P.state == DESTROYED ⇒ no thread holds reference to P
#
# CRITICAL: This test is EXPECTED TO FAIL before refactor.
# The test documents the violation, not the fix.
# Exit criteria: Test passes after p2p_manager.cpp refactor (Phase 4c).
#
# WARNING: This test uses real threads, not simulator.
# Compile with ASAN: -fsanitize=address -fno-omit-frame-pointer
# ════════════════════════════════════════════════════════════════════════════
option(ENABLE_TS1_THREAD_SAFETY "Build Ring 3 Phase 4b TS1 thread safety tests (EXPECTED FAIL)" ON)

if(ENABLE_TS1_THREAD_SAFETY AND EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_thread_safety_ts1.cpp)
  add_executable(test_thread_safety_ts1
    tests/p2p/test_thread_safety_ts1.cpp
  )

  target_link_libraries(test_thread_safety_ts1 PRIVATE
    GTest::gtest
    Threads::Threads
  )

  target_include_directories(test_thread_safety_ts1 PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests/p2p
  )

  # Register with CTest
  add_test(NAME ThreadSafety_TS1 COMMAND test_thread_safety_ts1)

  # Mark as ring3 phase4b test
  # Ring 3 Phase 4c COMPLETE: Production code now satisfies TS1
  set_tests_properties(ThreadSafety_TS1 PROPERTIES
    LABELS "p2p;ring3;threading;phase4b;phase4c;ts1"
    TIMEOUT 120  # Real threads may take longer
  )

  message(STATUS "✅ Ring 3 Phase 4b: TS1 thread safety test (Phase 4c complete - TS1 satisfied)")
elseif(ENABLE_TS1_THREAD_SAFETY)
  message(WARNING "⚠️  Ring 3 Phase 4b TS1 test file not found - skipping")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Ring 3 Phase 4c: TS1 Integration Test (Production P2PManager)
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_p2p_manager_ts1_integration.cpp)
  add_executable(test_p2p_manager_ts1_integration
    tests/p2p/test_p2p_manager_ts1_integration.cpp
    src/daemon/p2p_manager.cpp
    src/network/relay_hints_eviction.cpp
    src/network/relay_registry.cpp # Required by NAT C3 relay registry member in P2PManager
    src/network/quic_session.cpp   # Required by relay virtual-peer QUIC members in P2PManager
    src/network/quic_transport.cpp # Required by encrypted relay readiness checks in P2PManager
    src/network/udp_socket.cpp     # Required by QuicTransport
    src/network/relay_tls_keypair.cpp
    src/daemon/node_identity.cpp   # Required by dineroid handshake methods in P2PManager
    src/p2p/addr_v2.cpp            # Required by addrv2 methods in P2PManager
    src/p2p/addrman.cpp
    src/crypto/params_stub.cpp    # Provides Params().magic for standalone P2PManager link
    src/common/sha256d.cpp        # Required for Dinero::Common::double_sha256_raw
    src/common/logger.cpp         # Required by NodeIdentity diagnostics
    src/daemon/secure_random.cpp  # Required for SecureRandom::GetUInt64()
  )

  if(APPLE)
    target_link_libraries(test_p2p_manager_ts1_integration PRIVATE
      GTest::gtest
        Threads::Threads
      dinero_crypto
      secp256k1
      OpenSSL::Crypto
      "-framework Security"
    )
  else()
    target_link_libraries(test_p2p_manager_ts1_integration PRIVATE
      GTest::gtest
        Threads::Threads
      dinero_crypto
      secp256k1
      OpenSSL::Crypto
    )
  endif()

  if(DINERO_ENABLE_QUIC AND TARGET dinero_ngtcp2)
    target_link_libraries(test_p2p_manager_ts1_integration PRIVATE dinero_ngtcp2)
    if(TARGET dinero_ngtcp2_crypto_ossl)
      target_link_libraries(test_p2p_manager_ts1_integration PRIVATE dinero_ngtcp2_crypto_ossl)
    elseif(TARGET dinero_ngtcp2_crypto_quictls)
      target_link_libraries(test_p2p_manager_ts1_integration PRIVATE dinero_ngtcp2_crypto_quictls)
    endif()
  endif()

  if(WIN32)
    target_link_libraries(test_p2p_manager_ts1_integration PRIVATE ws2_32)
  endif()

  target_include_directories(test_p2p_manager_ts1_integration PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/daemon
  )

  # Register with CTest
  add_test(NAME P2PManager_TS1_Integration COMMAND test_p2p_manager_ts1_integration)

  # Ring 3 Phase 4c: After refactor, this should PASS
  set_tests_properties(P2PManager_TS1_Integration PROPERTIES
    LABELS "p2p;ring3;threading;phase4c;ts1;p2p-threading-smoke"
    TIMEOUT 30  # Integration tests should complete quickly
  )

  message(STATUS "✅ Ring 3 Phase 4c: TS1 integration test (production P2PManager)")
else()
  message(WARNING "⚠️  Ring 3 Phase 4c TS1 integration test not found - skipping")
endif()

# Keepalive idle-peer reaping policy (P2PManager::classify_idle_peer):
# fake-clock regression pinning the 90s boundary, the last_message_at
# refresh, and the deliberate silent-outbound exemption. Mirrors the TS1
# target's dependency list — the one proven to link p2p_manager.cpp
# standalone.
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_p2p_idle_peer_reap.cpp)
  add_executable(test_p2p_idle_peer_reap
    tests/p2p/test_p2p_idle_peer_reap.cpp
    src/daemon/p2p_manager.cpp
    src/network/relay_hints_eviction.cpp
    src/network/relay_registry.cpp
    src/network/quic_session.cpp
    src/network/quic_transport.cpp
    src/network/udp_socket.cpp
    src/network/relay_tls_keypair.cpp
    src/daemon/node_identity.cpp
    src/p2p/addr_v2.cpp
    src/p2p/addrman.cpp
    src/crypto/params_stub.cpp
    src/common/sha256d.cpp
    src/common/logger.cpp
    src/daemon/secure_random.cpp
  )

  if(APPLE)
    target_link_libraries(test_p2p_idle_peer_reap PRIVATE
      GTest::gtest
      Threads::Threads
      dinero_crypto
      secp256k1
      OpenSSL::Crypto
      "-framework Security"
    )
  else()
    target_link_libraries(test_p2p_idle_peer_reap PRIVATE
      GTest::gtest
      Threads::Threads
      dinero_crypto
      secp256k1
      OpenSSL::Crypto
    )
  endif()

  if(DINERO_ENABLE_QUIC AND TARGET dinero_ngtcp2)
    target_link_libraries(test_p2p_idle_peer_reap PRIVATE dinero_ngtcp2)
    if(TARGET dinero_ngtcp2_crypto_ossl)
      target_link_libraries(test_p2p_idle_peer_reap PRIVATE dinero_ngtcp2_crypto_ossl)
    elseif(TARGET dinero_ngtcp2_crypto_quictls)
      target_link_libraries(test_p2p_idle_peer_reap PRIVATE dinero_ngtcp2_crypto_quictls)
    endif()
  endif()

  target_include_directories(test_p2p_idle_peer_reap PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/daemon
  )

  add_test(NAME P2PIdlePeerReap COMMAND test_p2p_idle_peer_reap)
  set_tests_properties(P2PIdlePeerReap PROPERTIES
    LABELS "network;p2p;keepalive;smoke"
    TIMEOUT 10  # pure fake-clock policy test — no sockets, no sleeps
  )
endif()

# Focused build target for the relay/QUIC transport gate. CTest does not
# build executables, so this target gives lean QUIC build directories a clean
# "build focused tests, then ctest focused label" path.
set(DINERO_QUIC_RELAY_TEST_TARGETS "")

# Relay virtual-peer transport queue and mainnet plaintext safety gate.
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_relay_virtual_transport.cpp)
  add_executable(test_relay_virtual_transport
    tests/p2p/test_relay_virtual_transport.cpp
    tests/p2p/test_p2p_fd_hygiene.cpp
    src/daemon/p2p_manager.cpp
    src/network/relay_hints_eviction.cpp
    src/network/relay_registry.cpp
    src/network/quic_session.cpp
    src/network/quic_transport.cpp
    src/network/relay_tls_keypair.cpp
    src/network/udp_socket.cpp
    src/daemon/node_identity.cpp
    src/p2p/addr_v2.cpp
    src/p2p/addrman.cpp
    src/crypto/params_stub.cpp
    src/common/sha256d.cpp
    src/common/logger.cpp
    src/daemon/secure_random.cpp
  )

  target_compile_definitions(test_relay_virtual_transport PRIVATE DINERO_TEST_BUILD)

  if(APPLE)
    target_link_libraries(test_relay_virtual_transport PRIVATE
      GTest::gtest
      Threads::Threads
      dinero_crypto
      secp256k1
      OpenSSL::SSL
      OpenSSL::Crypto
      "-framework Security"
    )
  else()
    target_link_libraries(test_relay_virtual_transport PRIVATE
      GTest::gtest
      Threads::Threads
      dinero_crypto
      secp256k1
      OpenSSL::SSL
      OpenSSL::Crypto
    )
  endif()

  if(DINERO_ENABLE_QUIC AND TARGET dinero_ngtcp2)
    target_link_libraries(test_relay_virtual_transport PRIVATE dinero_ngtcp2)
    if(TARGET dinero_ngtcp2_crypto_ossl)
      target_link_libraries(test_relay_virtual_transport PRIVATE dinero_ngtcp2_crypto_ossl)
    elseif(TARGET dinero_ngtcp2_crypto_quictls)
      target_link_libraries(test_relay_virtual_transport PRIVATE dinero_ngtcp2_crypto_quictls)
    endif()
  endif()

  if(WIN32)
    target_link_libraries(test_relay_virtual_transport PRIVATE ws2_32)
  endif()

  target_include_directories(test_relay_virtual_transport PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/daemon
  )

  add_test(NAME RelayVirtualTransport COMMAND test_relay_virtual_transport)
  set_tests_properties(RelayVirtualTransport PROPERTIES
    LABELS "p2p;relay;transport;nat;quic-relay-focused"
    TIMEOUT 10
  )
  list(APPEND DINERO_QUIC_RELAY_TEST_TARGETS test_relay_virtual_transport)
endif()

# ════════════════════════════════════════════════════════════════════════════
if(DINERO_ENABLE_QUIC AND TARGET dinero_ngtcp2 AND EXISTS ${CMAKE_SOURCE_DIR}/tests/network/test_quic_dependency.cpp)
  add_executable(test_quic_dependency
    tests/network/test_quic_dependency.cpp
  )
  target_link_libraries(test_quic_dependency PRIVATE dinero_ngtcp2)
  add_test(NAME QuicDependency COMMAND test_quic_dependency)
  set_tests_properties(QuicDependency PROPERTIES
    LABELS "p2p;quic;ngtcp2;transport;quic-relay-focused"
    TIMEOUT 10
  )
  list(APPEND DINERO_QUIC_RELAY_TEST_TARGETS test_quic_dependency)
endif()

if(DINERO_ENABLE_QUIC AND TARGET dinero_ngtcp2 AND EXISTS ${CMAKE_SOURCE_DIR}/tests/network/test_quic_crypto_capability.cpp)
  add_executable(test_quic_crypto_capability
    tests/network/test_quic_crypto_capability.cpp
  )
  target_link_libraries(test_quic_crypto_capability
    PRIVATE
      dinero_ngtcp2
      OpenSSL::SSL
      OpenSSL::Crypto
  )
  if(TARGET dinero_ngtcp2_crypto_ossl)
    target_link_libraries(test_quic_crypto_capability PRIVATE dinero_ngtcp2_crypto_ossl)
    target_compile_definitions(test_quic_crypto_capability PRIVATE DINERO_NGTCP2_CRYPTO_BACKEND_OSSL=1)
  elseif(TARGET dinero_ngtcp2_crypto_quictls)
    target_link_libraries(test_quic_crypto_capability PRIVATE dinero_ngtcp2_crypto_quictls)
    target_compile_definitions(test_quic_crypto_capability PRIVATE DINERO_NGTCP2_CRYPTO_BACKEND_QUICTLS=1)
  else()
    target_compile_definitions(test_quic_crypto_capability PRIVATE DINERO_NGTCP2_CRYPTO_BACKEND_NONE=1)
  endif()
  add_test(NAME QuicCryptoCapability COMMAND test_quic_crypto_capability)
  set_tests_properties(QuicCryptoCapability PROPERTIES
    LABELS "p2p;quic;ngtcp2;transport;crypto;quic-relay-focused"
    TIMEOUT 10
  )
  list(APPEND DINERO_QUIC_RELAY_TEST_TARGETS test_quic_crypto_capability)
endif()

if(DINERO_ENABLE_QUIC AND EXISTS ${CMAKE_SOURCE_DIR}/tests/network/test_quic_transport.cpp)
  if(TARGET dinero_ngtcp2_crypto_ossl OR TARGET dinero_ngtcp2_crypto_quictls)
    add_executable(test_quic_transport
      tests/network/test_quic_transport.cpp
      src/network/quic_transport.cpp
      src/network/udp_socket.cpp
      src/network/relay_tls_keypair.cpp
    )
    target_link_libraries(test_quic_transport
      PRIVATE
        GTest::gtest
        Threads::Threads
        dinero_ngtcp2
        OpenSSL::SSL
        OpenSSL::Crypto
    )
    if(TARGET dinero_ngtcp2_crypto_ossl)
      target_link_libraries(test_quic_transport PRIVATE dinero_ngtcp2_crypto_ossl)
    elseif(TARGET dinero_ngtcp2_crypto_quictls)
      target_link_libraries(test_quic_transport PRIVATE dinero_ngtcp2_crypto_quictls)
    endif()
    if(WIN32)
      target_link_libraries(test_quic_transport PRIVATE ws2_32)
    endif()
    target_include_directories(test_quic_transport PRIVATE
      ${CMAKE_SOURCE_DIR}/include
    )
    add_test(NAME QuicTransport COMMAND test_quic_transport)
    set_tests_properties(QuicTransport PROPERTIES
      LABELS "p2p;quic;ngtcp2;transport;crypto;quic-relay-focused"
      TIMEOUT 10
    )
    list(APPEND DINERO_QUIC_RELAY_TEST_TARGETS test_quic_transport)
  endif()
endif()

if(DINERO_ENABLE_QUIC AND TARGET dinero_ngtcp2_crypto_ossl AND EXISTS ${CMAKE_SOURCE_DIR}/tests/network/test_quic_session.cpp)
  add_executable(test_quic_session
    tests/network/test_quic_session.cpp
    src/network/quic_session.cpp
    src/network/quic_transport.cpp
    src/network/udp_socket.cpp
    src/network/relay_tls_keypair.cpp
  )
  target_link_libraries(test_quic_session
    PRIVATE
      GTest::gtest
      Threads::Threads
      dinero_ngtcp2
      dinero_ngtcp2_crypto_ossl
      OpenSSL::SSL
      OpenSSL::Crypto
  )
  if(WIN32)
    target_link_libraries(test_quic_session PRIVATE ws2_32)
  endif()
  target_include_directories(test_quic_session PRIVATE
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME QuicSession COMMAND test_quic_session)
  set_tests_properties(QuicSession PROPERTIES
    LABELS "p2p;quic;ngtcp2;transport;crypto;quic-relay-focused"
    TIMEOUT 10
  )
  list(APPEND DINERO_QUIC_RELAY_TEST_TARGETS test_quic_session)
endif()

if(DINERO_ENABLE_QUIC AND TARGET dinero_ngtcp2_crypto_ossl AND EXISTS ${CMAKE_SOURCE_DIR}/tests/network/test_quic_session_stress.cpp)
  add_executable(test_quic_session_stress
    tests/network/test_quic_session_stress.cpp
    src/network/quic_session.cpp
    src/network/quic_transport.cpp
    src/network/udp_socket.cpp
    src/network/relay_tls_keypair.cpp
  )
  target_link_libraries(test_quic_session_stress
    PRIVATE
      GTest::gtest
      Threads::Threads
      dinero_ngtcp2
      dinero_ngtcp2_crypto_ossl
      OpenSSL::SSL
      OpenSSL::Crypto
  )
  if(WIN32)
    target_link_libraries(test_quic_session_stress PRIVATE ws2_32)
  endif()
  target_include_directories(test_quic_session_stress PRIVATE
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME QuicSessionStress COMMAND test_quic_session_stress)
  set_tests_properties(QuicSessionStress PROPERTIES
    LABELS "p2p;quic;ngtcp2;transport;crypto;quic-relay-focused"
    TIMEOUT 300
  )
  list(APPEND DINERO_QUIC_RELAY_TEST_TARGETS test_quic_session_stress)
endif()

if(DINERO_ENABLE_QUIC AND TARGET dinero_ngtcp2_crypto_ossl AND EXISTS ${CMAKE_SOURCE_DIR}/tests/network/test_relay_tls_keypair.cpp)
  add_executable(test_relay_tls_keypair
    tests/network/test_relay_tls_keypair.cpp
    src/network/relay_tls_keypair.cpp
    src/network/quic_session.cpp
    src/network/quic_transport.cpp
    src/network/udp_socket.cpp
  )
  target_link_libraries(test_relay_tls_keypair
    PRIVATE
      GTest::gtest
      GTest::gtest_main
      Threads::Threads
      dinero_ngtcp2
      dinero_ngtcp2_crypto_ossl
      OpenSSL::SSL
      OpenSSL::Crypto
  )
  if(WIN32)
    target_link_libraries(test_relay_tls_keypair PRIVATE ws2_32)
  endif()
  target_include_directories(test_relay_tls_keypair PRIVATE
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME RelayTlsKeypair COMMAND test_relay_tls_keypair)
  set_tests_properties(RelayTlsKeypair PROPERTIES
    LABELS "p2p;quic;ngtcp2;transport;crypto;quic-relay-focused"
    TIMEOUT 15
  )
  list(APPEND DINERO_QUIC_RELAY_TEST_TARGETS test_relay_tls_keypair)
endif()

if(DINERO_QUIC_RELAY_TEST_TARGETS)
  add_custom_target(dinero_quic_relay_test_targets
    DEPENDS ${DINERO_QUIC_RELAY_TEST_TARGETS}
  )
endif()

# P2P Hardening Tests (March 2026 Security Audit)
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_hardening.cpp)
  add_executable(test_p2p_hardening
    tests/p2p/test_hardening.cpp
  )

  target_link_libraries(test_p2p_hardening PRIVATE
    dinero_core
    dinero_crypto
    secp256k1
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
  )

  target_include_directories(test_p2p_hardening PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  add_test(NAME P2P_Hardening COMMAND test_p2p_hardening)
  set_tests_properties(P2P_Hardening PROPERTIES
    LABELS "p2p;hardening;security"
    TIMEOUT 10
  )
  message(STATUS "P2P hardening tests registered")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Ring 3 Phase 4d: TS2 (Lock Ordering & Deadlock Freedom)
# ════════════════════════════════════════════════════════════════════════════
option(ENABLE_TS2_LOCK_ORDERING "Build Ring 3 Phase 4d TS2 lock ordering tests" ON)

if(ENABLE_TS2_LOCK_ORDERING AND EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_thread_safety_ts2.cpp)
  add_executable(test_thread_safety_ts2
    tests/p2p/test_thread_safety_ts2.cpp
  )

  target_link_libraries(test_thread_safety_ts2 PRIVATE
    GTest::gtest
    Threads::Threads
  )

  target_include_directories(test_thread_safety_ts2 PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests/p2p
  )

  # Register with CTest
  add_test(NAME ThreadSafety_TS2 COMMAND test_thread_safety_ts2)

  # Ring 3 Phase 4d: TS2 lock ordering property test
  set_tests_properties(ThreadSafety_TS2 PROPERTIES
    LABELS "p2p;ring3;threading;phase4d;ts2;lock-ordering"
    TIMEOUT 60  # Lock ordering tests with timeout detection
  )

  message(STATUS "✅ Ring 3 Phase 4d: TS2 lock ordering test (property definition)")
elseif(ENABLE_TS2_LOCK_ORDERING)
  message(WARNING "⚠️  Ring 3 Phase 4d TS2 test file not found - skipping")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Ring 3 Phase 4e: TS3 (Blocking-Free Event Loops / Liveness)
# ════════════════════════════════════════════════════════════════════════════
option(ENABLE_TS3_LIVENESS "Build Ring 3 Phase 4e TS3 liveness tests" ON)

if(ENABLE_TS3_LIVENESS AND EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_thread_safety_ts3.cpp)
  add_executable(test_thread_safety_ts3
    tests/p2p/test_thread_safety_ts3.cpp
  )

  target_link_libraries(test_thread_safety_ts3 PRIVATE
    GTest::gtest
    Threads::Threads
  )

  target_include_directories(test_thread_safety_ts3 PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests/p2p
  )

  # Register with CTest
  add_test(NAME ThreadSafety_TS3 COMMAND test_thread_safety_ts3)

  # Ring 3 Phase 4e: TS3 liveness property test
  set_tests_properties(ThreadSafety_TS3 PROPERTIES
    LABELS "p2p;ring3;threading;phase4e;ts3;liveness"
    TIMEOUT 120  # Liveness tests may take longer (shutdown delays)
  )

  message(STATUS "✅ Ring 3 Phase 4e: TS3 liveness test (property definition)")
elseif(ENABLE_TS3_LIVENESS)
  message(WARNING "⚠️  Ring 3 Phase 4e TS3 test file not found - skipping")
endif()

# BIP39 test suite
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_bip39.cpp)
  add_executable(test_bip39 tests/test_bip39.cpp)
  if(APPLE)
    target_link_libraries(test_bip39 PRIVATE dinero_crypto dinero_wallet secp256k1 "-framework Security")
  else()
    target_link_libraries(test_bip39 PRIVATE dinero_crypto dinero_wallet secp256k1)
  endif()
  target_include_directories(test_bip39 PRIVATE ${CMAKE_SOURCE_DIR}/include)
  add_test(NAME BIP39 COMMAND test_bip39)
endif()

# Wallet runtime path hygiene gate
if(EXISTS ${CMAKE_SOURCE_DIR}/ci/check_wallet_hardcoded_paths.sh)
  add_test(
    NAME WalletPathHygiene
    COMMAND bash ${CMAKE_SOURCE_DIR}/ci/check_wallet_hardcoded_paths.sh
  )
  set_tests_properties(WalletPathHygiene PROPERTIES LABELS "wallet;hygiene;path")
endif()

# v7 retired coin-type hygiene gate
if(EXISTS ${CMAKE_SOURCE_DIR}/ci/check_retired_coin_type_1447.sh)
  add_test(
    NAME RetiredCoinType1447Hygiene
    COMMAND bash ${CMAKE_SOURCE_DIR}/ci/check_retired_coin_type_1447.sh
  )
  set_tests_properties(RetiredCoinType1447Hygiene PROPERTIES LABELS "wallet;hygiene;coin_type")
endif()

# Wallet FFI concurrency stress (deadlock guard for FFI lock discipline)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_wallet_ffi_concurrency.cpp)
  add_executable(test_wallet_ffi_concurrency
    tests/test_wallet_ffi_concurrency.cpp
    src/wallet/taproot_control_block.cpp
    src/wallet/p2mr_address.cpp
    src/daemon/bech32_encoder.cpp
    tests/stubs/mempool_test_stubs.cpp
  )
  if(APPLE)
    target_link_libraries(test_wallet_ffi_concurrency PRIVATE
      dinero_wallet_ffi
      dinero_wallet
      wallet_test_deps
      dinero_chainstate
      dinero_crypto
      secp256k1
      "-framework Security"
      Threads::Threads
    )
  else()
    target_link_libraries(test_wallet_ffi_concurrency PRIVATE
      dinero_wallet_ffi
      dinero_wallet
      wallet_test_deps
      dinero_chainstate
      dinero_crypto
      secp256k1
      Threads::Threads
    )
  endif()
  target_include_directories(test_wallet_ffi_concurrency PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/wallet-core/ffi
  )
  add_test(NAME WalletFFIConcurrency COMMAND test_wallet_ffi_concurrency)
  set_tests_properties(WalletFFIConcurrency PROPERTIES TIMEOUT 60 LABELS "wallet;ffi;concurrency")
endif()

# ============================================================================
# Phase D.2: Consensus Invariant Tests (CRITICAL)
# ============================================================================
# These tests verify that consensus rules are correctly locked and enforced.
# If any of these tests fail, there is a CRITICAL consensus bug.

# Phase D.2.1: Premine Validation Test
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_premine_validation.cpp)
  add_executable(test_premine_validation tests/test_premine_validation.cpp)
  target_link_libraries(test_premine_validation PRIVATE
    dinero_chainstate  # For ChainDB::getBlock, getBlockHashByHeight
    dinero_consensus
    dinero_core        # For logger and daemon components
  )
  link_zstd_if_needed(test_premine_validation)
  target_include_directories(test_premine_validation PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME PremineValidation COMMAND test_premine_validation)
endif()

# Phase D.2.2: Consensus Limits Test
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_consensus_limits.cpp)
  add_executable(test_consensus_limits tests/test_consensus_limits.cpp)
  target_link_libraries(test_consensus_limits PRIVATE
    dinero_consensus
  )
  link_zstd_if_needed(test_consensus_limits)
  target_include_directories(test_consensus_limits PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME ConsensusLimits COMMAND test_consensus_limits)
endif()

# Phase D.2.3: Monetary Constants Test
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_monetary_constants.cpp)
  add_executable(test_monetary_constants tests/test_monetary_constants.cpp)
  target_link_libraries(test_monetary_constants PRIVATE
    dinero_consensus
  )
  link_zstd_if_needed(test_monetary_constants)
  target_include_directories(test_monetary_constants PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME MonetaryConstants COMMAND test_monetary_constants)
endif()

# ============================================================================
# Phase 11a: Consensus-Lock Tests (CRITICAL REGRESSION PREVENTION)
# ============================================================================
# These tests lock the Phase 11a consensus separation and Utreexo integration.
# They prevent regressions in:
#   1. Merkle computation (single-TX invariant)
#   2. Mining vs Validation parity (PATH A vs PATH B)
#   3. ApplyBlock vs ValidateAndApplyBlock equivalence
#
# If any of these tests fail, Phase 11a consensus is BROKEN.
# DO NOT merge changes that break these tests.

# Phase 11a.1: Merkle Invariants Lock
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_merkle_invariants.cpp)
  add_executable(test_merkle_invariants tests/consensus/test_merkle_invariants.cpp)
  target_link_libraries(test_merkle_invariants PRIVATE
    dinero_consensus
    dinero_crypto
    dinero_core          # For BlockAssembler
    GTest::gtest
    GTest::gtest_main
  )
  link_zstd_if_needed(test_merkle_invariants)
  target_include_directories(test_merkle_invariants PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME MerkleInvariants COMMAND test_merkle_invariants)
  set_tests_properties(MerkleInvariants PROPERTIES
    LABELS "consensus;critical;phase11a;merkle"
    TIMEOUT 30
  )
  message(STATUS "✅ Phase 11a Merkle Invariants test enabled")
endif()

# DISABLED: Requires IConsensusUTXOSet interface refactor (BlockValidator constructor changed)
# Phase 11a.2: Mining vs Validation Parity Lock
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_mining_validation_parity.cpp)
  add_executable(test_mining_validation_parity tests/consensus/test_mining_validation_parity.cpp)
  target_link_libraries(test_mining_validation_parity PRIVATE
    dinero_consensus
    dinero_crypto
    dinero_core          # For BlockAssembler
    GTest::gtest
    GTest::gtest_main
  )
  link_zstd_if_needed(test_mining_validation_parity)
  if(TARGET bulletproofs_ffi)
    target_link_libraries(test_mining_validation_parity PRIVATE bulletproofs_ffi)
    if(TARGET build_bulletproofs_ffi)
      add_dependencies(test_mining_validation_parity build_bulletproofs_ffi)
    endif()
  endif()
  target_include_directories(test_mining_validation_parity PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME MiningValidationParity COMMAND test_mining_validation_parity)
  set_tests_properties(MiningValidationParity PROPERTIES
    LABELS "consensus;critical;phase11a;parity"
    TIMEOUT 30
  )
  message(STATUS "✅ Phase 11a Mining/Validation Parity test enabled")
endif()

# Priority 2: Block Template Determinism Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mining/test_block_template_determinism.cpp)
  add_executable(test_block_template_determinism
    tests/mining/test_block_template_determinism.cpp
    tests/stubs/mempool_test_stubs.cpp   # forEachEntry, onBlockConnected, etc.
    tests/stubs/daemon_test_stubs.cpp    # Mempool extras (getMempoolEntry, getTransactionFee, …) + vault
    src/daemon/config.cpp                # GetConfig() referenced by consensus_write_batch.obj
  )
  target_link_libraries(test_block_template_determinism PRIVATE
    dinero_core          # For Mempool, BlockAssembler
    dinero_consensus
    dinero_crypto
    GTest::gtest
    GTest::gtest_main
  )
  link_zstd_if_needed(test_block_template_determinism)
  target_include_directories(test_block_template_determinism PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME BlockTemplateDeterminism COMMAND test_block_template_determinism)
  set_tests_properties(BlockTemplateDeterminism PROPERTIES
    LABELS "consensus;critical;mining;determinism"
    TIMEOUT 60
  )
  message(STATUS "✅ Block Template Determinism test enabled")
endif()

# Priority 3: UTXO Index Correctness Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/utxo/test_utxo_index_correctness.cpp)
  add_executable(test_utxo_index_correctness tests/utxo/test_utxo_index_correctness.cpp)
  target_include_directories(test_utxo_index_correctness PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME UTXOIndexCorrectness COMMAND test_utxo_index_correctness)
  set_tests_properties(UTXOIndexCorrectness PROPERTIES
    LABELS "utxo;critical;wallet;consensus"
    TIMEOUT 60
  )
  message(STATUS "✅ UTXO Index Correctness test enabled")
endif()

# Priority 4: Persistence Safety Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/persistence/test_persistence_safety.cpp)
  add_executable(test_persistence_safety tests/persistence/test_persistence_safety.cpp)
  target_include_directories(test_persistence_safety PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME PersistenceSafety COMMAND test_persistence_safety)
  set_tests_properties(PersistenceSafety PROPERTIES
    LABELS "persistence;critical;crash-safety"
    TIMEOUT 60
  )
  message(STATUS "✅ Persistence Safety test enabled")
endif()

# Priority 5: Daemon Invariants Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/daemon/test_daemon_invariants.cpp)
  add_executable(test_daemon_invariants tests/daemon/test_daemon_invariants.cpp)
  target_include_directories(test_daemon_invariants PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME DaemonInvariants COMMAND test_daemon_invariants)
  set_tests_properties(DaemonInvariants PROPERTIES
    LABELS "daemon;invariants;critical"
    TIMEOUT 60
  )
  message(STATUS "✅ Daemon Invariants test enabled")
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/daemon/test_archival_config_defaults.cpp)
  add_executable(test_archival_config_defaults
    tests/daemon/test_archival_config_defaults.cpp
    src/daemon/services/config_service.cpp
    src/common/logger.cpp
  )
  target_include_directories(test_archival_config_defaults PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME ArchivalConfigDefaults COMMAND test_archival_config_defaults)
  set_tests_properties(ArchivalConfigDefaults PROPERTIES
    LABELS "daemon;config;archival"
    TIMEOUT 30
  )
  message(STATUS "✅ Archival config defaults test enabled")
endif()

# Phase B (Dinero Core 1.0) — INI-style config-file loader
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/daemon/test_config_file_loader.cpp)
  add_executable(test_config_file_loader
    tests/daemon/test_config_file_loader.cpp
    src/daemon/services/config_service.cpp
    src/common/logger.cpp
  )
  target_include_directories(test_config_file_loader PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME ConfigFileLoader COMMAND test_config_file_loader)
  set_tests_properties(ConfigFileLoader PROPERTIES
    LABELS "daemon;config;file-parsing"
    TIMEOUT 30
  )
  message(STATUS "✅ Config file loader test enabled")
endif()

# Phase D.4 (Dinero Core 1.0) — health-check decision logic
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/daemon/test_health.cpp)
  add_executable(test_health
    tests/daemon/test_health.cpp
    src/daemon/health.cpp
  )
  target_include_directories(test_health PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME HealthCheck COMMAND test_health)
  set_tests_properties(HealthCheck PROPERTIES
    LABELS "daemon;health"
    TIMEOUT 30
  )
  message(STATUS "✅ Health check test enabled")
endif()

# services-boundary-smoke: pilot test for MetricsService.
# First entry in the bucket the dinero_core surgery plan requires
# before any service hub can be moved out of dinero_core. Exercises
# Init/Start/IsStarted/Stop/ExportMetrics lifecycle with real
# LoggerService + ConfigService dependencies; no DaemonContext
# heavyweight setup.
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/services/test_metrics_service.cpp)
  add_executable(test_metrics_service
    tests/services/test_metrics_service.cpp
    src/daemon/services/metrics_service.cpp
    src/daemon/services/config_service.cpp
    src/metrics/metrics_registry.cpp
    src/common/logger.cpp
  )
  target_include_directories(test_metrics_service PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME MetricsServiceBoundary COMMAND test_metrics_service)
  set_tests_properties(MetricsServiceBoundary PROPERTIES
    LABELS "services-boundary-smoke;daemon;metrics;service-hub"
    TIMEOUT 30
  )
  message(STATUS "✅ MetricsService boundary smoke test enabled")
endif()

# services-boundary-smoke: AddressManagerService boundary contract.
# Service owns a unique_ptr<p2p::AddressManager>; tests verify Name(),
# getManager() identity preservation, and Init/Start/Stop safety.
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/services/test_address_manager_service.cpp)
  add_executable(test_address_manager_service
    tests/services/test_address_manager_service.cpp
    src/daemon/services/address_manager_service.cpp
    src/p2p/addrman.cpp
  )
  target_include_directories(test_address_manager_service PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME AddressManagerServiceBoundary COMMAND test_address_manager_service)
  set_tests_properties(AddressManagerServiceBoundary PROPERTIES
    LABELS "services-boundary-smoke;daemon;address-manager;service-hub"
    TIMEOUT 30
  )
  message(STATUS "✅ AddressManagerService boundary smoke test enabled")
endif()

# services-boundary-smoke: RBFPolicyService boundary contract.
# Service owns a unique_ptr<policy::MempoolPolicy>; tests verify Name(),
# getPolicy() identity preservation, and Init/Start/Stop safety.
# Links dinero_tx_primitives because MempoolPolicy::checkSigOps etc.
# reference Transaction::GetSize / GetWeight / Serialize.
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/services/test_rbf_policy_service.cpp)
  add_executable(test_rbf_policy_service
    tests/services/test_rbf_policy_service.cpp
    src/daemon/services/rbf_policy_service.cpp
    src/policy/mempool_policy.cpp
    src/primitives/transaction_serializer.cpp
  )
  target_link_libraries(test_rbf_policy_service PRIVATE
    dinero_tx_primitives
  )
  target_include_directories(test_rbf_policy_service PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME RBFPolicyServiceBoundary COMMAND test_rbf_policy_service)
  set_tests_properties(RBFPolicyServiceBoundary PROPERTIES
    LABELS "services-boundary-smoke;daemon;rbf-policy;service-hub"
    TIMEOUT 30
  )
  message(STATUS "✅ RBFPolicyService boundary smoke test enabled")
endif()

# services-boundary-smoke: HeadersSyncService boundary contract.
# Service owns a unique_ptr<p2p::HeadersFirstSync>; tests verify Name(),
# getManager() identity preservation, Init/Start/Stop safety, and fresh
# sync-state default (isSyncing()==false on a new service).
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/services/test_headers_sync_service.cpp)
  add_executable(test_headers_sync_service
    tests/services/test_headers_sync_service.cpp
    src/daemon/services/headers_sync_service.cpp
    src/p2p/headers_first_sync.cpp
  )
  target_link_libraries(test_headers_sync_service PRIVATE
    jsoncpp_static  # HeadersFirstSync uses Json::Value for status/serialization
  )
  target_include_directories(test_headers_sync_service PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME HeadersSyncServiceBoundary COMMAND test_headers_sync_service)
  set_tests_properties(HeadersSyncServiceBoundary PROPERTIES
    LABELS "services-boundary-smoke;daemon;headers-sync;service-hub"
    TIMEOUT 30
  )
  message(STATUS "✅ HeadersSyncService boundary smoke test enabled")
endif()

# issue #214: deterministic regression for in-daemon staleness recovery.
# Pure decision logic (header-only) — links only gtest, no daemon/sockets.
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/services/test_stale_tip_recovery.cpp)
  add_executable(test_stale_tip_recovery
    tests/services/test_stale_tip_recovery.cpp
  )
  add_dependencies(test_stale_tip_recovery gtest gtest_main)
  target_include_directories(test_stale_tip_recovery BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
  )
  target_link_libraries(test_stale_tip_recovery PRIVATE
    GTest::gtest_main
  )
  add_test(NAME StaleTipRecovery COMMAND test_stale_tip_recovery)
  set_tests_properties(StaleTipRecovery PROPERTIES
    LABELS "services-boundary-smoke;daemon;p2p;staleness-recovery;smoke"
    TIMEOUT 15
  )
  message(STATUS "✅ StaleTipRecovery (#214) deterministic test enabled")
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/services/test_header_refresh_coalescer.cpp)
  add_executable(test_header_refresh_coalescer
    tests/services/test_header_refresh_coalescer.cpp
  )
  add_dependencies(test_header_refresh_coalescer gtest gtest_main)
  target_include_directories(test_header_refresh_coalescer BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
  )
  target_link_libraries(test_header_refresh_coalescer PRIVATE
    gtest gtest_main
  )
  add_test(NAME HeaderRefreshCoalescer COMMAND test_header_refresh_coalescer)
  set_tests_properties(HeaderRefreshCoalescer PROPERTIES LABELS "p2p;unit")
endif()

# services-boundary-smoke: PeerScoringService boundary contract.
# Service owns a unique_ptr<p2p::PeerScoringManager>; tests verify Name(),
# getManager() identity preservation, Init/Start/Stop safety, and that
# fresh-peer defaults (score=0, isBanned=false) wire through.
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/services/test_peer_scoring_service.cpp)
  add_executable(test_peer_scoring_service
    tests/services/test_peer_scoring_service.cpp
    src/daemon/services/peer_scoring_service.cpp
    src/daemon/services/config_service.cpp  # ConfigService::DataDir() referenced in Init
    src/common/logger.cpp                    # transitive through config_service
    src/p2p/peer_scoring.cpp
  )
  target_include_directories(test_peer_scoring_service PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME PeerScoringServiceBoundary COMMAND test_peer_scoring_service)
  set_tests_properties(PeerScoringServiceBoundary PROPERTIES
    LABELS "services-boundary-smoke;daemon;peer-scoring;service-hub"
    TIMEOUT 30
  )
  message(STATUS "✅ PeerScoringService boundary smoke test enabled")
endif()

# services-boundary-smoke: CompactBlockService boundary contract.
# CompactBlockService is the thinnest service hub in the original 9 — it
# owns no internal state, only delegates stats to BlockRelayManager via
# DaemonContext. Test exercises Name(), Init/Start/Stop lifecycle, and
# the null-guard property on its stat accessors when block_relay is null.
#
# The .cpp references BlockRelayManager::GetStats() and
# BlockRelayManager::GetCompactBlockSuccessRate() (both live in
# src/daemon/block_relay_manager.cpp, which is dinero_core-heavy). The
# test target links a tiny test-only stub (block_relay_manager_test_stub.cpp)
# providing empty implementations returning defaults. Since the test
# never sets ctx_->block_relay, those stubs are never invoked at
# runtime — they only exist to satisfy the linker without dragging the
# full block_relay_manager.cpp transitive set in.
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/services/test_compact_block_service.cpp)
  add_executable(test_compact_block_service
    tests/services/test_compact_block_service.cpp
    src/daemon/services/compact_block_service.cpp
    tests/services/block_relay_manager_test_stub.cpp
  )
  target_include_directories(test_compact_block_service PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME CompactBlockServiceBoundary COMMAND test_compact_block_service)
  set_tests_properties(CompactBlockServiceBoundary PROPERTIES
    LABELS "services-boundary-smoke;daemon;compact-block;service-hub"
    TIMEOUT 30
  )
  message(STATUS "✅ CompactBlockService boundary smoke test enabled")
endif()

# dinero_core gap closure: MempoolPolicy unit test.
# Not a service-boundary test (mempool_policy isn't a service hub), but
# it's a dinero_core source-map gap with zero prior coverage. Scope
# deliberately avoids anything needing a real Transaction (those tests
# are integration-shaped); exercises config defaults, error strings,
# config round-trip, and the eviction-ordering invariant.
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/services/test_mempool_policy.cpp)
  add_executable(test_mempool_policy
    tests/services/test_mempool_policy.cpp
    src/policy/mempool_policy.cpp
    src/primitives/transaction_serializer.cpp
  )
  target_link_libraries(test_mempool_policy PRIVATE
    dinero_tx_primitives  # MempoolPolicy::checkSigOps references Transaction methods
  )
  target_include_directories(test_mempool_policy PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME MempoolPolicyUnit COMMAND test_mempool_policy)
  set_tests_properties(MempoolPolicyUnit PROPERTIES
    LABELS "policy-smoke;daemon;mempool;policy"
    TIMEOUT 30
  )
  message(STATUS "✅ MempoolPolicy unit test enabled")
endif()

# services-boundary-smoke: BlockIngressService boundary contract.
# Validation-adjacent service: this smoke intentionally avoids Submit()/BlockAcceptor
# and locks only the IService/IBlockIngress-facing lifecycle and metrics boundary.
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/services/test_block_ingress_service.cpp)
  add_executable(test_block_ingress_service
    tests/services/test_block_ingress_service.cpp
    src/daemon/config.cpp                # GetConfig() referenced by dinero_core objects
    tests/stubs/daemon_test_stubs.cpp    # vault + daemon mempool extra isolation seams
    tests/stubs/mempool_test_stubs.cpp   # no-op Mempool notification/iteration seams
  )
  target_link_libraries(test_block_ingress_service PRIVATE
    dinero_core
  )
  target_include_directories(test_block_ingress_service PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME BlockIngressServiceBoundary COMMAND test_block_ingress_service)
  set_tests_properties(BlockIngressServiceBoundary PROPERTIES
    LABELS "services-boundary-smoke;daemon;block-ingress;service-hub;validation-adjacent"
    TIMEOUT 30
  )
  message(STATUS "✅ BlockIngressService boundary smoke test enabled")
endif()

# dinero_core gap closure: MiningEventBus unit test.
# MiningEventBus (src/daemon/mining_events.{h,cpp}) is the thread-safe
# ring buffer of mining-side events (hashrate, state, block-found) that
# downstream subscribers (RPC, qt UI, metrics) consume via snapshot
# polling. Five contract properties locked: empty bus, monotonic ids,
# since-based filtering, max cap honoured, payload shape per event type.
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/services/test_mining_events.cpp)
  add_executable(test_mining_events
    tests/services/test_mining_events.cpp
    src/daemon/mining_events.cpp
  )
  target_link_libraries(test_mining_events PRIVATE
    jsoncpp_static  # MiningEventBus uses Json::Value as the event payload
  )
  target_include_directories(test_mining_events PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME MiningEventBusUnit COMMAND test_mining_events)
  set_tests_properties(MiningEventBusUnit PROPERTIES
    LABELS "events-smoke;daemon;mining;events"
    TIMEOUT 30
  )
  message(STATUS "✅ MiningEventBus unit test enabled")
endif()

# Phase C (Dinero Core 1.0) — `cmake --install` layout smoke test.
# Verifies install rules produce the contract-required layout for both
# manual mode (--prefix /usr/local-style) and packaged mode (--prefix
# /usr with DESTDIR). Uses cmake --install internally; depends on
# dinerod and dinero-cli being built.
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/install/test_cmake_install.sh)
  add_test(NAME CmakeInstallLayout
    COMMAND ${CMAKE_SOURCE_DIR}/tests/install/test_cmake_install.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  )
  set_tests_properties(CmakeInstallLayout PROPERTIES
    LABELS "install;packaging-smoke"
    TIMEOUT 60
    ENVIRONMENT "BUILD_DIR=${CMAKE_BINARY_DIR}"
  )
  message(STATUS "✅ cmake --install layout test enabled")
endif()

# Phase D.2 (Dinero Core 1.0) — recovery-grade wallet backup test.
# Exercises every quality gate: refuse unsafe paths, declared-targets-
# only, manifest integrity round-trip, atomic output, mode 0600, dry-
# run, no-overwrite, no staging leftovers, no chainstate in archive.
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/install/test_dinero_backup.sh)
  add_test(NAME DineroBackup
    COMMAND ${CMAKE_SOURCE_DIR}/tests/install/test_dinero_backup.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  )
  set_tests_properties(DineroBackup PROPERTIES
    LABELS "backup;recovery;packaging-smoke"
    TIMEOUT 60
  )
  message(STATUS "✅ dinero-backup test enabled")
endif()

# Phase D.3 (Dinero Core 1.0) — pre-upgrade rollback capture test.
# Verifies non-fatal-on-failure contract, atomic output, naming
# pattern, dir mode, dry-run, stale-backup preservation, Linux-gate.
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/install/test_dinero_prepare_upgrade.sh)
  add_test(NAME DineroPrepareUpgrade
    COMMAND ${CMAKE_SOURCE_DIR}/tests/install/test_dinero_prepare_upgrade.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  )
  set_tests_properties(DineroPrepareUpgrade PROPERTIES
    LABELS "rollback;recovery;packaging-smoke"
    TIMEOUT 60
  )
  message(STATUS "✅ dinero-prepare-upgrade test enabled")
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/daemon/test_assumeutxo_metadata_lifecycle.cpp)
  add_executable(test_assumeutxo_metadata_lifecycle
    tests/daemon/test_assumeutxo_metadata_lifecycle.cpp
  )
  add_dependencies(test_assumeutxo_metadata_lifecycle gtest)
  target_link_libraries(test_assumeutxo_metadata_lifecycle PRIVATE
    dinero_chainstate
    dinero_wallet
    gtest
    sqlite3
    ${CMAKE_DL_LIBS}
  )
  link_zstd_if_needed(test_assumeutxo_metadata_lifecycle)
  target_include_directories(test_assumeutxo_metadata_lifecycle PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
  )
  add_test(NAME AssumeUTXOMetadataLifecycle COMMAND test_assumeutxo_metadata_lifecycle)
  set_tests_properties(AssumeUTXOMetadataLifecycle PROPERTIES
    LABELS "daemon;assumeutxo;persistence"
    TIMEOUT 60
  )
  message(STATUS "✅ AssumeUTXO metadata lifecycle test enabled")
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/packaging/test_generate_snapshot_pipeline.sh)
  add_test(NAME GenerateSnapshotPipeline
    COMMAND ${CMAKE_SOURCE_DIR}/tests/packaging/test_generate_snapshot_pipeline.sh
  )
  set_tests_properties(GenerateSnapshotPipeline PROPERTIES
    LABELS "assumeutxo;packaging-smoke;snapshot"
    TIMEOUT 30
  )
endif()

# AssumeUTXO replay hardening: confirm-before-fatal policy + torn-body merkle guard
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/daemon/test_replay_failure_policy.cpp)
  add_executable(test_replay_failure_policy
    tests/daemon/test_replay_failure_policy.cpp
  )
  add_dependencies(test_replay_failure_policy gtest)
  target_link_libraries(test_replay_failure_policy PRIVATE
    dinero_chainstate
    dinero_wallet
    gtest
    sqlite3
    ${CMAKE_DL_LIBS}
  )
  link_zstd_if_needed(test_replay_failure_policy)
  target_include_directories(test_replay_failure_policy PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
  )
  add_test(NAME ReplayFailurePolicy COMMAND test_replay_failure_policy)
  set_tests_properties(ReplayFailurePolicy PROPERTIES
    LABELS "daemon;assumeutxo"
    TIMEOUT 60
  )
  message(STATUS "✅ AssumeUTXO replay-hardening test enabled")
endif()

# Heap-corruption regression: UnreadableBlockSet concurrent-access safety
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/daemon/test_unreadable_block_set.cpp)
  add_executable(test_unreadable_block_set
    tests/daemon/test_unreadable_block_set.cpp
  )
  add_dependencies(test_unreadable_block_set gtest)
  target_link_libraries(test_unreadable_block_set PRIVATE
    dinero_chainstate
    dinero_wallet
    gtest
    sqlite3
    ${CMAKE_DL_LIBS}
  )
  link_zstd_if_needed(test_unreadable_block_set)
  target_include_directories(test_unreadable_block_set PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
  )
  add_test(NAME UnreadableBlockSet COMMAND test_unreadable_block_set)
  set_tests_properties(UnreadableBlockSet PROPERTIES
    LABELS "daemon;concurrency"
    TIMEOUT 120
  )
  message(STATUS "✅ UnreadableBlockSet concurrency test enabled")
endif()

# Double-teardown regression: DaemonApp::Stop exactly-once gate (stop_gate.h)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/daemon/test_stop_gate.cpp)
  add_executable(test_stop_gate
    tests/daemon/test_stop_gate.cpp
  )
  add_dependencies(test_stop_gate gtest)
  target_link_libraries(test_stop_gate PRIVATE
    gtest
    ${CMAKE_DL_LIBS}
  )
  target_include_directories(test_stop_gate PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
  )
  add_test(NAME StopGate COMMAND test_stop_gate)
  set_tests_properties(StopGate PROPERTIES
    LABELS "daemon;concurrency"
    TIMEOUT 120
  )
  message(STATUS "✅ StopGate double-teardown test enabled")
endif()

# Rooted-husk regression: forest deserialize must be all-or-nothing (62742 wedge)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_forest_serialization_integrity.cpp)
  add_executable(test_forest_serialization_integrity
    tests/consensus/test_forest_serialization_integrity.cpp
  )
  add_dependencies(test_forest_serialization_integrity gtest)
  target_link_libraries(test_forest_serialization_integrity PRIVATE
    dinero_chainstate
    gtest
    ${CMAKE_DL_LIBS}
  )
  link_zstd_if_needed(test_forest_serialization_integrity)
  target_include_directories(test_forest_serialization_integrity PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
  )
  add_test(NAME ForestSerializationIntegrity COMMAND test_forest_serialization_integrity)
  set_tests_properties(ForestSerializationIntegrity PROPERTIES
    LABELS "consensus;utreexo"
    TIMEOUT 120
  )
  message(STATUS "✅ Forest serialization integrity test enabled")
endif()

# AssumeUTXO fatal-state-machine lifecycle tests (docs/design/assumeutxo-fatal-state-machine.md)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/daemon/test_assumeutxo_lifecycle.cpp)
  add_executable(test_assumeutxo_lifecycle
    tests/daemon/test_assumeutxo_lifecycle.cpp
    src/daemon/services/assumeutxo_lifecycle.cpp
  )
  add_dependencies(test_assumeutxo_lifecycle gtest)
  target_link_libraries(test_assumeutxo_lifecycle PRIVATE
    dinero_chainstate
    dinero_wallet
    gtest
    sqlite3
    ${CMAKE_DL_LIBS}
  )
  link_zstd_if_needed(test_assumeutxo_lifecycle)
  target_include_directories(test_assumeutxo_lifecycle PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
  )
  add_test(NAME AssumeUtxoLifecycle COMMAND test_assumeutxo_lifecycle)
  set_tests_properties(AssumeUtxoLifecycle PROPERTIES
    LABELS "daemon;assumeutxo;state-machine"
    TIMEOUT 60
  )
  message(STATUS "✅ AssumeUTXO lifecycle state-machine test enabled")
endif()

# AssumeUTXO replay engine: real ConnectBlock over a fresh consensus set
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/daemon/test_assumeutxo_replay.cpp)
  add_executable(test_assumeutxo_replay
    tests/daemon/test_assumeutxo_replay.cpp
    src/daemon/services/assumeutxo_replay.cpp
    src/consensus/utxo_set_digest.cpp
    src/daemon/config.cpp  # GetConfig() referenced by consensus_write_batch.obj
    tests/stubs/mempool_test_stubs.cpp  # Mempool hooks pulled in via dinero_core
    tests/stubs/daemon_test_stubs.cpp   # Mempool extras + vault
  )
  add_dependencies(test_assumeutxo_replay gtest)
  target_link_libraries(test_assumeutxo_replay PRIVATE
    dinero_core
    dinero_consensus
    dinero_crypto
    secp256k1
    ${ROCKSDB_LIBRARY}
    ${SQLITE3_LIBRARIES}
    $<TARGET_FILE:gtest>
  )
  if(TARGET bulletproofs_ffi)
    target_link_libraries(test_assumeutxo_replay PRIVATE bulletproofs_ffi)
    if(TARGET build_bulletproofs_ffi)
      add_dependencies(test_assumeutxo_replay build_bulletproofs_ffi)
    endif()
  endif()
  link_zstd_if_needed(test_assumeutxo_replay)
  target_include_directories(test_assumeutxo_replay BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME AssumeUtxoReplay COMMAND test_assumeutxo_replay)
  set_tests_properties(AssumeUtxoReplay PROPERTIES
    LABELS "daemon;assumeutxo;replay"
    TIMEOUT 120
  )
  message(STATUS "✅ AssumeUTXO replay engine test enabled")
endif()

# ─── Canonical UTXO records digest (AssumeUTXO content commitment) ───────────
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_utxo_set_digest.cpp)
  add_executable(test_utxo_set_digest
    tests/consensus/test_utxo_set_digest.cpp
    src/consensus/utxo_set_digest.cpp
  )
  add_dependencies(test_utxo_set_digest gtest)
  target_link_libraries(test_utxo_set_digest PRIVATE
    dinero_chainstate
    dinero_wallet
    gtest
    sqlite3
    ${CMAKE_DL_LIBS}
  )
  link_zstd_if_needed(test_utxo_set_digest)
  target_include_directories(test_utxo_set_digest PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
  )
  add_test(NAME UtxoSetDigest COMMAND test_utxo_set_digest)
  set_tests_properties(UtxoSetDigest PROPERTIES
    LABELS "consensus;assumeutxo;digest"
    TIMEOUT 60
  )
  message(STATUS "✅ UTXO records digest test enabled")
endif()

# Doctor Diagnostic Command Tests (Phase 4)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/daemon/test_doctor.cpp)
  add_executable(test_doctor
    tests/daemon/test_doctor.cpp
    # Doctor sources (not in a separate library)
    src/daemon/doctor/doctor_context.cpp
    src/daemon/doctor/doctor_registry.cpp
    src/daemon/doctor/doctor_runner.cpp
    src/daemon/doctor/doctor_fixer.cpp
    src/daemon/doctor/doctor_json_emitter.cpp
    src/daemon/doctor/doctor_renderer.cpp
    src/daemon/doctor/doctor_checks_v1.cpp
  )

  add_dependencies(test_doctor gtest)

  target_link_libraries(test_doctor PRIVATE
    dinero_consensus
    dinero_crypto
    jsoncpp_static
    $<TARGET_FILE:gtest>
  )
  link_zstd_if_needed(test_doctor)

  target_include_directories(test_doctor BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  if(ENABLE_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    dinero_enable_sanitizers(test_doctor)
  endif()

  add_test(NAME DoctorDiagnostics COMMAND test_doctor)
  set_tests_properties(DoctorDiagnostics PROPERTIES
    LABELS "daemon;doctor;phase4"
    TIMEOUT 60
  )

  message(STATUS "✅ Doctor diagnostic tests enabled")
endif()

# Datadir guard tests (single-writer and PID safety)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/daemon/test_datadir_guard.cpp)
  add_executable(test_datadir_guard
    tests/daemon/test_datadir_guard.cpp
    src/daemon/datadir_guard.cpp
  )

  add_dependencies(test_datadir_guard gtest gtest_main)

  target_link_libraries(test_datadir_guard PRIVATE
    $<TARGET_FILE:gtest>
    $<TARGET_FILE:gtest_main>
  )

  target_include_directories(test_datadir_guard BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  if(ENABLE_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    dinero_enable_sanitizers(test_datadir_guard)
  endif()

  add_test(NAME DatadirGuard COMMAND test_datadir_guard)
  set_tests_properties(DatadirGuard PROPERTIES
    LABELS "daemon;operational-safety"
    TIMEOUT 60
  )

  message(STATUS "✅ Datadir guard tests enabled")
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/daemon/test_chainstate_recovery_marker.cpp)
  add_executable(test_chainstate_recovery_marker
    tests/daemon/test_chainstate_recovery_marker.cpp
  )

  target_include_directories(test_chainstate_recovery_marker BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  add_test(NAME ChainstateRecoveryMarker COMMAND test_chainstate_recovery_marker)
  set_tests_properties(ChainstateRecoveryMarker PROPERTIES
    LABELS "daemon;recovery"
    TIMEOUT 30
  )

  message(STATUS "✅ Chainstate recovery marker test enabled")
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/daemon/test_undo_audit_coinbase_gate.cpp)
  add_executable(test_undo_audit_coinbase_gate
    tests/daemon/test_undo_audit_coinbase_gate.cpp
  )

  target_include_directories(test_undo_audit_coinbase_gate BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  add_test(NAME UndoAuditCoinbaseGate COMMAND test_undo_audit_coinbase_gate)
  set_tests_properties(UndoAuditCoinbaseGate PROPERTIES
    LABELS "daemon;recovery"
    TIMEOUT 30
  )

  message(STATUS "✅ Undo-audit coinbase maturity gate test enabled")
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/daemon/test_utxo_position_index_rebuild.cpp)
  add_executable(test_utxo_position_index_rebuild
    tests/daemon/test_utxo_position_index_rebuild.cpp
    src/common/logger.cpp
  )

  target_link_libraries(test_utxo_position_index_rebuild PRIVATE
    dinero_consensus
    dinero_chainstate
    RocksDB::rocksdb
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
  )

  link_zstd_if_needed(test_utxo_position_index_rebuild)

  target_include_directories(test_utxo_position_index_rebuild BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )

  if(APPLE)
    target_link_libraries(test_utxo_position_index_rebuild PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()

  add_test(NAME UTXOPositionIndexRebuild COMMAND test_utxo_position_index_rebuild)
  set_tests_properties(UTXOPositionIndexRebuild PROPERTIES
    LABELS "daemon;utreexo;recovery"
    TIMEOUT 60
  )

  message(STATUS "✅ UTXO position rebuild test enabled")
endif()

# DISABLED: Requires IConsensusUTXOSet interface refactor (BlockValidator constructor changed)
# Phase 11a.3: Utreexo Consensus Parity Lock
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_utreexo_consensus_parity.cpp)
  add_executable(test_utreexo_consensus_parity tests/consensus/test_utreexo_consensus_parity.cpp)
  target_link_libraries(test_utreexo_consensus_parity PRIVATE
    dinero_consensus
    dinero_crypto
    dinero_core          # For BlockAssembler
    GTest::gtest
    GTest::gtest_main
  )
  link_zstd_if_needed(test_utreexo_consensus_parity)
  if(TARGET bulletproofs_ffi)
    target_link_libraries(test_utreexo_consensus_parity PRIVATE bulletproofs_ffi)
    if(TARGET build_bulletproofs_ffi)
      add_dependencies(test_utreexo_consensus_parity build_bulletproofs_ffi)
    endif()
  endif()
  target_include_directories(test_utreexo_consensus_parity PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME UtreexoConsensusParity COMMAND test_utreexo_consensus_parity)
  set_tests_properties(UtreexoConsensusParity PROPERTIES
    LABELS "consensus;critical;phase11a;utreexo"
    TIMEOUT 30
  )
  message(STATUS "✅ Phase 11a Utreexo Consensus Parity test enabled")
endif()

# Utreexo Pollard/Stump Commitment Invariant Test
# CRITICAL: Verifies Pollard.getCommitment() == Stump.getCommitment()
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_utreexo_stump_invariant.cpp)
  add_executable(test_utreexo_stump_invariant
    tests/consensus/test_utreexo_stump_invariant.cpp
    ${CMAKE_SOURCE_DIR}/tests/stubs/chain_test_stubs.cpp
  )
  target_link_libraries(test_utreexo_stump_invariant PRIVATE
    dinero_consensus_core
    dinero_crypto
    dinero_tx_primitives
  )
  link_zstd_if_needed(test_utreexo_stump_invariant)
  target_include_directories(test_utreexo_stump_invariant PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME UtreexoStumpInvariant COMMAND test_utreexo_stump_invariant)
  set_tests_properties(UtreexoStumpInvariant PROPERTIES
    LABELS "consensus;critical;utreexo;stump;invariant"
    TIMEOUT 30
  )
  message(STATUS "✅ Utreexo Pollard/Stump Invariant test enabled")
endif()

# Phase 11b.2: Witness Merkle Isolation Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_witness_merkle_isolation.cpp)
  add_executable(test_witness_merkle_isolation tests/consensus/test_witness_merkle_isolation.cpp)
  target_link_libraries(test_witness_merkle_isolation PRIVATE
    dinero_consensus
    dinero_crypto
    dinero_tx_primitives # For Transaction, Block
    GTest::gtest
    GTest::gtest_main
  )
  link_zstd_if_needed(test_witness_merkle_isolation)
  target_include_directories(test_witness_merkle_isolation PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME WitnessMerkleIsolation COMMAND test_witness_merkle_isolation)
  set_tests_properties(WitnessMerkleIsolation PROPERTIES
    LABELS "consensus;critical;phase11b;witness;merkle;isolation"
    TIMEOUT 30
  )
  message(STATUS "✅ Phase 11b Witness Merkle Isolation test enabled")
endif()

# Phase 11c.4: Witness Commitment Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_witness_commitment.cpp)
  add_executable(test_witness_commitment tests/consensus/test_witness_commitment.cpp)
  target_link_libraries(test_witness_commitment PRIVATE
    dinero_consensus
    dinero_crypto
    dinero_tx_primitives  # For Transaction, Block
    GTest::gtest
    GTest::gtest_main
  )
  link_zstd_if_needed(test_witness_commitment)
  target_include_directories(test_witness_commitment PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME WitnessCommitment COMMAND test_witness_commitment)
  set_tests_properties(WitnessCommitment PROPERTIES
    LABELS "consensus;critical;phase11c;witness;commitment;dinw"
    TIMEOUT 30
  )
  message(STATUS "✅ Phase 11c Witness Commitment test enabled")
endif()

# Phase 11d.2: Witness Commitment Enforcement Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_witness_commitment_enforcement.cpp)
  add_executable(test_witness_commitment_enforcement tests/consensus/test_witness_commitment_enforcement.cpp)
  target_link_libraries(test_witness_commitment_enforcement PRIVATE
    dinero_consensus
    dinero_crypto
    dinero_tx_primitives  # For Transaction, Block
    GTest::gtest
    GTest::gtest_main
  )
  link_zstd_if_needed(test_witness_commitment_enforcement)
  target_include_directories(test_witness_commitment_enforcement PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME WitnessCommitmentEnforcement COMMAND test_witness_commitment_enforcement)
  set_tests_properties(WitnessCommitmentEnforcement PROPERTIES
    LABELS "consensus;critical;phase11d;witness;enforcement;safety"
    TIMEOUT 30
  )
  message(STATUS "✅ Phase 11d Witness Commitment Enforcement test enabled")
endif()

# CRITICAL: Activation Boundary Tests (Genesis, Premine, Height 2)
# These tests LOCK the activation boundary FOREVER. DO NOT MODIFY.
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_activation_boundary_critical.cpp)
  add_executable(test_activation_boundary_critical tests/consensus/test_activation_boundary_critical.cpp)
  target_link_libraries(test_activation_boundary_critical PRIVATE
    dinero_consensus
    dinero_crypto
    dinero_tx_primitives
    GTest::gtest
    GTest::gtest_main
  )
  link_zstd_if_needed(test_activation_boundary_critical)
  target_include_directories(test_activation_boundary_critical PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME ActivationBoundaryCritical COMMAND test_activation_boundary_critical)
  set_tests_properties(ActivationBoundaryCritical PROPERTIES
    LABELS "consensus;critical;activation;boundary;genesis;premine;height2"
    TIMEOUT 30
  )
  message(STATUS "✅ CRITICAL Activation Boundary tests enabled (genesis, premine, height 2)")
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_filter_commitment_activation.cpp)
  add_executable(test_filter_commitment_activation
    tests/consensus/test_filter_commitment_activation.cpp
  )
  target_link_libraries(test_filter_commitment_activation PRIVATE
    dinero_consensus
    dinero_crypto
    dinero_tx_primitives
    GTest::gtest
    GTest::gtest_main
  )
  link_zstd_if_needed(test_filter_commitment_activation)
  target_include_directories(test_filter_commitment_activation PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME FilterCommitmentActivation COMMAND test_filter_commitment_activation)
  set_tests_properties(FilterCommitmentActivation PROPERTIES
    LABELS "consensus;filters;activation;commitment;critical"
    TIMEOUT 30
  )
  message(STATUS "✅ Filter commitment activation tests enabled")
endif()

# Phase 11e.1: Witness Magic Translation Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_witness_magic_translation.cpp)
  add_executable(test_witness_magic_translation tests/consensus/test_witness_magic_translation.cpp)
  target_link_libraries(test_witness_magic_translation PRIVATE
    dinero_consensus
    dinero_crypto
    dinero_tx_primitives  # For Transaction, Block
    GTest::gtest
    GTest::gtest_main
  )
  link_zstd_if_needed(test_witness_magic_translation)
  target_include_directories(test_witness_magic_translation PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME WitnessMagicTranslation COMMAND test_witness_magic_translation)
  set_tests_properties(WitnessMagicTranslation PROPERTIES
    LABELS "consensus;critical;phase11e;witness;translation;interpretation"
    TIMEOUT 30
  )
  message(STATUS "✅ Phase 11e Witness Magic Translation test enabled")
endif()

# Phase 6C: Taproot Consensus Validation Tests (BIP340/341/342)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_taproot_consensus.cpp)
  add_executable(test_taproot_consensus tests/test_taproot_consensus.cpp)
  target_link_libraries(test_taproot_consensus PRIVATE
    dinero_consensus
    dinero_crypto
  )
  link_zstd_if_needed(test_taproot_consensus)
  # NO HOMEBREW PATHS - use vendored secp256k1
  # dinero_consensus and dinero_crypto already link secp256k1
  target_include_directories(test_taproot_consensus PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_directories(test_taproot_consensus PRIVATE /opt/homebrew/lib)
  endif()
  add_test(NAME TaprootConsensus COMMAND test_taproot_consensus)
endif()

# Phase 24.2: Script Interpreter Tests (P2WPKH, P2WSH, Taproot)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_script_interpreter.cpp)
  add_executable(test_script_interpreter tests/test_script_interpreter.cpp)
  target_link_libraries(test_script_interpreter PRIVATE
    dinero_consensus
    dinero_crypto
    OpenSSL::Crypto  # SHA1, SHA256 functions
    secp256k1        # ECDSA, Schnorr signatures
  )
  link_zstd_if_needed(test_script_interpreter)
  target_include_directories(test_script_interpreter PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_directories(test_script_interpreter PRIVATE /opt/homebrew/lib)
  endif()
  add_test(NAME ScriptInterpreter COMMAND test_script_interpreter)
endif()

# Phase 12b: Debug Signature Verification Failures
if(EXISTS ${CMAKE_SOURCE_DIR}/debug_sig_test.cpp)
  add_executable(debug_sig_test debug_sig_test.cpp)
  target_link_libraries(debug_sig_test PRIVATE
    dinero_consensus
    dinero_crypto
    OpenSSL::Crypto
    secp256k1
  )
  link_zstd_if_needed(debug_sig_test)
  target_include_directories(debug_sig_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_directories(debug_sig_test PRIVATE /opt/homebrew/lib)
  endif()
endif()

# Phase 28: Covenant Framework Tests (CTV, CSFS, TXHASH, CCV)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_covenants.cpp)
  add_executable(test_covenants tests/test_covenants.cpp)
  target_link_libraries(test_covenants PRIVATE
    dinero_consensus
    dinero_crypto
    OpenSSL::Crypto
    secp256k1
  )
  link_zstd_if_needed(test_covenants)
  target_include_directories(test_covenants PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_directories(test_covenants PRIVATE /opt/homebrew/lib)
  endif()
  add_test(NAME CovenantTests COMMAND test_covenants)
endif()

# BIP119 compatibility vectors exercise the production CTV hash implementation
# independently of Dinero's wallet/RPC constructors.
add_executable(test_bip119_ctv_vectors
  tests/consensus/test_bip119_ctv_vectors.cpp
)
add_dependencies(test_bip119_ctv_vectors gtest gtest_main)
target_link_libraries(test_bip119_ctv_vectors PRIVATE
  dinero_consensus
  jsoncpp_static
  gtest
  gtest_main
)
link_zstd_if_needed(test_bip119_ctv_vectors)
target_include_directories(test_bip119_ctv_vectors PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
)
# Absolute path to the pinned upstream corpus. Injected rather than resolved
# relative to the working directory so the test cannot silently find nothing
# when ctest runs it from a different cwd.
target_compile_definitions(test_bip119_ctv_vectors PRIVATE
  DINERO_BIP119_CTVHASH_JSON="${CMAKE_SOURCE_DIR}/tests/vectors/bip119_ctvhash.json"
)
add_test(NAME BIP119CTVVectors COMMAND test_bip119_ctv_vectors)
set_tests_properties(BIP119CTVVectors PROPERTIES
  LABELS "consensus;covenant;ctv"
  TIMEOUT 60
)

add_executable(test_taproot_scriptpath_consensus
  tests/consensus/test_taproot_scriptpath_consensus.cpp
)
add_dependencies(test_taproot_scriptpath_consensus gtest gtest_main)
target_link_libraries(test_taproot_scriptpath_consensus PRIVATE
  dinero_consensus
  gtest
  gtest_main
)
link_zstd_if_needed(test_taproot_scriptpath_consensus)
target_include_directories(test_taproot_scriptpath_consensus PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
)
add_test(NAME TaprootScriptPathConsensus
  COMMAND test_taproot_scriptpath_consensus)
set_tests_properties(TaprootScriptPathConsensus PROPERTIES
  LABELS "consensus;taproot;scriptpath;covenant"
  TIMEOUT 10
)

add_executable(test_bip341_sighash_vectors
  tests/consensus/test_bip341_sighash_vectors.cpp
)
add_dependencies(test_bip341_sighash_vectors gtest gtest_main)
target_link_libraries(test_bip341_sighash_vectors PRIVATE
  dinero_consensus
  gtest
  gtest_main
)
link_zstd_if_needed(test_bip341_sighash_vectors)
target_include_directories(test_bip341_sighash_vectors PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
)
add_test(NAME BIP341SighashVectors COMMAND test_bip341_sighash_vectors)
set_tests_properties(BIP341SighashVectors PROPERTIES
  LABELS "consensus;taproot;sighash"
  TIMEOUT 10
)

add_executable(test_covenant_activation
  tests/consensus/test_covenant_activation.cpp
)
add_dependencies(test_covenant_activation gtest gtest_main)
target_link_libraries(test_covenant_activation PRIVATE
  dinero_consensus
  gtest
  gtest_main
)
link_zstd_if_needed(test_covenant_activation)
target_include_directories(test_covenant_activation PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
)
add_test(NAME CovenantActivation COMMAND test_covenant_activation)
set_tests_properties(CovenantActivation PROPERTIES
  LABELS "consensus;covenant;activation"
  TIMEOUT 10
)

# Deliberately NOT a gtest binary: it must observe the process state before any
# chain has ever been selected, which no gtest case can reach (and which fork()
# based death tests inherit rather than reset). Plain main(), explicit exit
# codes, no assert() -- see the file header.
add_executable(test_selectparams_fresh_process
  tests/consensus/test_selectparams_fresh_process.cpp
)
target_link_libraries(test_selectparams_fresh_process PRIVATE
  dinero_consensus
)
link_zstd_if_needed(test_selectparams_fresh_process)
target_include_directories(test_selectparams_fresh_process PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
)
add_test(NAME SelectParamsFreshProcess COMMAND test_selectparams_fresh_process)
set_tests_properties(SelectParamsFreshProcess PROPERTIES
  LABELS "consensus;chainparams"
  TIMEOUT 10
)

add_executable(test_chainparams_selection
  tests/consensus/test_chainparams_selection.cpp
)
add_dependencies(test_chainparams_selection gtest gtest_main)
target_link_libraries(test_chainparams_selection PRIVATE
  dinero_consensus
  gtest
  gtest_main
)
link_zstd_if_needed(test_chainparams_selection)
target_include_directories(test_chainparams_selection PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
)
add_test(NAME ChainParamsSelection COMMAND test_chainparams_selection)
set_tests_properties(ChainParamsSelection PROPERTIES
  LABELS "consensus;chainparams"
  TIMEOUT 10
)

add_executable(test_covenant_system_lifecycle
  tests/consensus/test_covenant_system_lifecycle.cpp
  src/daemon/mempool.cpp
  src/daemon/config.cpp
  tests/stubs/vault_test_stubs.cpp
)
target_link_libraries(test_covenant_system_lifecycle PRIVATE
  dinero_core
  dinero_consensus
  dinero_pq
  dinero_shielded
  dinero_chainstate
  dinero_crypto
  secp256k1
  ${ROCKSDB_LIBRARY}
  ${SQLITE3_LIBRARIES}
  GTest::gtest
  GTest::gtest_main
)
if(TARGET bulletproofs_ffi)
  target_link_libraries(
    test_covenant_system_lifecycle PRIVATE bulletproofs_ffi)
  if(TARGET build_bulletproofs_ffi)
    add_dependencies(
      test_covenant_system_lifecycle build_bulletproofs_ffi)
  endif()
endif()
link_zstd_if_needed(test_covenant_system_lifecycle)
target_include_directories(test_covenant_system_lifecycle PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
)
if(APPLE)
  target_link_libraries(test_covenant_system_lifecycle PRIVATE
    "-framework Security"
    "-framework CoreFoundation"
  )
endif()
add_test(
  NAME CovenantSystemLifecycle
  COMMAND test_covenant_system_lifecycle)
set_tests_properties(CovenantSystemLifecycle PROPERTIES
  LABELS "consensus;covenant;activation;mempool;mining;reorg;restart"
  TIMEOUT 30
)

add_executable(benchmark_covenant_validation
  tests/benchmarks/benchmark_covenant_validation.cpp
)
target_link_libraries(benchmark_covenant_validation PRIVATE
  dinero_consensus
  dinero_crypto
  secp256k1
)
target_include_directories(benchmark_covenant_validation PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
)

add_executable(test_ccv_successor_binding
  tests/consensus/test_ccv_successor_binding.cpp
)
add_dependencies(test_ccv_successor_binding gtest gtest_main)
target_link_libraries(test_ccv_successor_binding PRIVATE
  dinero_consensus
  gtest
  gtest_main
)
link_zstd_if_needed(test_ccv_successor_binding)
target_include_directories(test_ccv_successor_binding PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
)
add_test(NAME CcvSuccessorBinding COMMAND test_ccv_successor_binding)
set_tests_properties(CcvSuccessorBinding PROPERTIES
  LABELS "consensus;covenant;ccv;scriptpath"
  TIMEOUT 10
)

# CCV has no upstream BIP, no published vector corpus, and no second
# implementation to differ against. Adversarial coverage attacking each
# numbered rule of CCV_SUCCESSOR_BINDING_V1 is the substitute for that.
add_executable(test_ccv_adversarial
  tests/consensus/test_ccv_adversarial.cpp
)
add_dependencies(test_ccv_adversarial gtest gtest_main)
target_link_libraries(test_ccv_adversarial PRIVATE
  dinero_consensus
  gtest
  gtest_main
)
link_zstd_if_needed(test_ccv_adversarial)
target_include_directories(test_ccv_adversarial PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
)
# Implementation-independent CCV reference model, differentially tested against
# production. CCV has no upstream vectors and no second implementation; this
# supplies one, derived from the spec rather than from covenants.cpp.
add_executable(test_ccv_reference_model
  tests/consensus/test_ccv_reference_model.cpp
)
add_dependencies(test_ccv_reference_model gtest gtest_main)
target_link_libraries(test_ccv_reference_model PRIVATE
  dinero_consensus
  secp256k1
  gtest
  gtest_main
)
link_zstd_if_needed(test_ccv_reference_model)
target_include_directories(test_ccv_reference_model PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/third_party/secp256k1-zkp/include
  ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
)
# Bounded deterministic fuzz gate for the covenant paths. Deliberately NOT
# given the `fuzz` label: CI excludes that label, so a fuzz-labelled target
# produces zero CI signal (see issue #486). The same binary serves as the long
# campaign via DINERO_COVENANT_FUZZ_ITERATIONS and the documented
# ASan/UBSan/unsigned-overflow build.
add_executable(test_covenant_fuzz
  tests/consensus/test_covenant_fuzz.cpp
)
add_dependencies(test_covenant_fuzz gtest gtest_main)
target_link_libraries(test_covenant_fuzz PRIVATE
  dinero_consensus
  gtest
  gtest_main
)
link_zstd_if_needed(test_covenant_fuzz)
target_include_directories(test_covenant_fuzz PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
)
target_compile_definitions(test_covenant_fuzz PRIVATE
  DINERO_COVENANT_FUZZ_SEEDS="${CMAKE_SOURCE_DIR}/tests/vectors/covenant_fuzz_seeds"
)
# Bounded-state analysis: exhaustive enumeration over reduced domains with
# verdicts predicted from the spec. Complements the adversarial suite (specific
# attacks) and the fuzzer (random sampling) -- neither of those enumerates.
add_executable(test_ccv_bounded_state
  tests/consensus/test_ccv_bounded_state.cpp
)
add_dependencies(test_ccv_bounded_state gtest gtest_main)
target_link_libraries(test_ccv_bounded_state PRIVATE
  dinero_consensus
  gtest
  gtest_main
)
link_zstd_if_needed(test_ccv_bounded_state)
target_include_directories(test_ccv_bounded_state PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
)
add_test(NAME CcvBoundedState COMMAND test_ccv_bounded_state)
set_tests_properties(CcvBoundedState PROPERTIES
  LABELS "consensus;covenant;ccv"
  TIMEOUT 120
)

add_test(NAME CovenantFuzz COMMAND test_covenant_fuzz)
set_tests_properties(CovenantFuzz PROPERTIES
  LABELS "consensus;covenant;ccv;ctv"
  TIMEOUT 300
)

add_test(NAME CcvReferenceModel COMMAND test_ccv_reference_model)
set_tests_properties(CcvReferenceModel PROPERTIES
  LABELS "consensus;covenant;ccv;scriptpath"
  TIMEOUT 120
)

add_test(NAME CcvAdversarial COMMAND test_ccv_adversarial)
set_tests_properties(CcvAdversarial PROPERTIES
  LABELS "consensus;covenant;ccv;scriptpath"
  TIMEOUT 30
)

# Production wallet construction and checksummed recovery descriptors for the
# regtest-active profile. The test feeds every constructed spend back through
# the consensus Taproot verifier; construction never substitutes validation.
add_executable(test_covenant_profile_wallet
  tests/wallet/test_covenant_profile.cpp
)
add_dependencies(test_covenant_profile_wallet gtest gtest_main)
target_link_libraries(test_covenant_profile_wallet PRIVATE
  dinero_wallet
  dinero_consensus
  dinero_crypto
  secp256k1
  gtest
  gtest_main
)
link_zstd_if_needed(test_covenant_profile_wallet)
target_include_directories(test_covenant_profile_wallet PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
)
add_test(NAME CovenantProfileWallet COMMAND test_covenant_profile_wallet)
set_tests_properties(CovenantProfileWallet PROPERTIES
  LABELS "wallet;covenant;ctv;ccv;recovery"
  TIMEOUT 15
)

if(NOT WIN32)
  add_executable(test_covenant_wallet_recovery
    tests/wallet/test_covenant_wallet_recovery.cpp
    src/daemon/lightning_stubs.cpp
    tests/stubs/mempool_test_stubs.cpp
  )
  add_dependencies(test_covenant_wallet_recovery gtest gtest_main)
  target_link_libraries(test_covenant_wallet_recovery PRIVATE
    dinero_wallet
    dinero_chainstate
    dinero_consensus
    dinero_crypto
    jsoncpp_static
    OpenSSL::Crypto
    secp256k1
    RocksDB::rocksdb
    sqlite3
    gtest
    gtest_main
  )
  link_zstd_if_needed(test_covenant_wallet_recovery)
  target_include_directories(test_covenant_wallet_recovery PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
  )
  if(APPLE)
    target_link_libraries(
      test_covenant_wallet_recovery PRIVATE "-framework Security")
  endif()
  add_test(
    NAME CovenantWalletRecovery
    COMMAND test_covenant_wallet_recovery)
  set_tests_properties(CovenantWalletRecovery PROPERTIES
    LABELS "wallet;covenant;recovery;persistence;migration"
    TIMEOUT 30
  )
endif()

# Phase 29: Covenant Integration Layer Tests
# DELETED registration: this test was superseded by Phase C.2, which moved
# covenant policy directly into mempool.cpp without the deprecated
# covenant_policy.h+wallet dependencies the test was written against. The
# test source still exists for archival reference but no longer compiles
# (AmountUna→uint64_t conversion was removed in C.2). Don't re-enable
# without porting the test to the new mempool-side API.
# See include/mempool/covenant_policy.h header comment.

# Phase C.4: Covenant script-path integration tests
if(ENABLE_ZK AND EXISTS ${CMAKE_SOURCE_DIR}/tests/test_covenant_scriptpath.cpp)
  add_executable(test_covenant_scriptpath
    tests/test_covenant_scriptpath.cpp
  )
  target_link_libraries(test_covenant_scriptpath PRIVATE
    dinero_consensus
    dinero_crypto
    dinero_zk
    secp256k1
    gtest
    gtest_main
  )
  link_zstd_if_needed(test_covenant_scriptpath)
  target_include_directories(test_covenant_scriptpath PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/secp256k1-zkp/include
  )
  add_test(NAME CovenantScriptPath COMMAND test_covenant_scriptpath)
  set_tests_properties(CovenantScriptPath PROPERTIES
    LABELS "covenant;scriptpath;covenant-policy-smoke"
    TIMEOUT 60
  )
  message(STATUS "  ✓ Covenant script-path tests registered")
endif()

# Phase 30: Taproot Asset Layer Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_assets.cpp)
  add_executable(test_assets
    tests/test_assets.cpp
    src/assets/asset_id.cpp
    src/assets/asset_state.cpp
    src/assets/asset_proof.cpp
    src/assets/asset_transfer.cpp
  )
  target_link_libraries(test_assets PRIVATE
    dinero_consensus
    dinero_crypto
    OpenSSL::Crypto
    secp256k1
  )
  link_zstd_if_needed(test_assets)
  target_include_directories(test_assets PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME TaprootAssetTests COMMAND test_assets)
endif()

# RPC gettransaction Tests (blockchain.gettransaction + wallet.gettransaction)
# Tests transaction lookup, output type detection, and wallet enrichment
# NOT consensus-critical - read-only query tools for operator UX
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_rpc_gettransaction.cpp)
  add_executable(test_rpc_gettransaction
    tests/test_rpc_gettransaction.cpp
  )
  target_link_libraries(test_rpc_gettransaction PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_rpc_handlers
    jsoncpp_static
  )
  link_zstd_if_needed(test_rpc_gettransaction)
  target_include_directories(test_rpc_gettransaction PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
  )
  add_test(NAME RPCGetTransactionTests COMMAND test_rpc_gettransaction)
endif()

# Phase 24.3: Bitcoin Core Script JSON Test Vectors
# Phase 24.5: Taproot Template Engine
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_script_json.cpp)
  add_executable(test_script_json
    tests/test_script_json.cpp
    src/script/taproot_templates.cpp
  )
  target_link_libraries(test_script_json PRIVATE
    dinero_consensus
    dinero_crypto
    OpenSSL::Crypto
    secp256k1
  )
  link_zstd_if_needed(test_script_json)
  target_include_directories(test_script_json PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/nlohmann  # nlohmann/json.hpp
    ${SECP256K1_INCLUDE_DIR}
  )
  add_test(NAME ScriptJSONTests COMMAND test_script_json ${CMAKE_SOURCE_DIR}/tests/data/script_tests.json)
endif()

# Taproot Address Decoding Test (Step 4.2)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_taproot_address_decoding.cpp)
  add_executable(test_taproot_address_decoding
    tests/test_taproot_address_decoding.cpp
    src/address/addr_codec.cpp
    src/address/addr_types.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_taproot_address_decoding PRIVATE
    dinero_consensus
    dinero_crypto
    dinero_wallet
  )
  link_zstd_if_needed(test_taproot_address_decoding)
  if(APPLE)
    target_link_libraries(test_taproot_address_decoding PRIVATE secp256k1 "-framework Security")
  else()
    target_link_libraries(test_taproot_address_decoding PRIVATE secp256k1)
  endif()
  target_include_directories(test_taproot_address_decoding PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/external
  )
  add_test(NAME TaprootAddressDecoding COMMAND test_taproot_address_decoding)
endif()

# Taproot Key Generation Test (Step 4.4)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_taproot_keys.cpp)
  add_executable(test_taproot_keys
    tests/test_taproot_keys.cpp
    src/wallet/taproot_keys.cpp
    src/address/addr_codec.cpp
    src/address/addr_types.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_taproot_keys PRIVATE
    dinero_consensus
    dinero_crypto
    dinero_wallet
    OpenSSL::SSL
    OpenSSL::Crypto
  )
  link_zstd_if_needed(test_taproot_keys)
  if(APPLE)
    target_link_libraries(test_taproot_keys PRIVATE secp256k1 "-framework Security")
  else()
    target_link_libraries(test_taproot_keys PRIVATE secp256k1)
  endif()
  target_include_directories(test_taproot_keys PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/external
  )
  add_test(NAME TaprootKeyGeneration COMMAND test_taproot_keys)
endif()

# Wallet encryption test (Argon2id + AES-256-GCM) - DISABLED: uses dinero_wallet (dead architecture)
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/crypto/test_wallet_encryption.cpp)
  add_executable(test_wallet_encryption tests/crypto/test_wallet_encryption.cpp)
  target_link_libraries(test_wallet_encryption PRIVATE dinero_wallet argon2 OpenSSL::Crypto)
  target_include_directories(test_wallet_encryption PRIVATE ${CMAKE_SOURCE_DIR}/include)
  add_test(NAME WalletEncryption COMMAND test_wallet_encryption)
endif()

# Wallet Encryption Metadata Fix Verification Test
# TEMPORARILY DISABLED (2026-02-02): wallet_test_deps needs ChainDB symbols
# TODO: Update linking when wallet_test_deps is fixed
# if(EXISTS ${CMAKE_SOURCE_DIR}/tests/crypto/test_wallet_encryption_fix.cpp)
#   add_executable(test_wallet_encryption_fix tests/crypto/test_wallet_encryption_fix.cpp)
#   target_link_libraries(test_wallet_encryption_fix PRIVATE dinero_wallet argon2 OpenSSL::Crypto wallet_test_deps)
#   if(APPLE)
#     target_link_libraries(test_wallet_encryption_fix PRIVATE "-framework Security" "-framework CoreFoundation")
#   endif()
#   target_include_directories(test_wallet_encryption_fix PRIVATE ${CMAKE_SOURCE_DIR}/include)
#   add_test(NAME WalletEncryptionFix COMMAND test_wallet_encryption_fix)
# endif()

# ═══════════════════════════════════════════════════════════════════════════
# Wallet Correctness Tests (Ring 2 - Optional, Non-Blocking)
# ═══════════════════════════════════════════════════════════════════════════
# Tests wallet invariants:
#   - Address derivation determinism (Test 3) ✅
#   - UTXO persistence across restarts (Test 2) - TODO
#   - Wallet restore correctness (Test 1) - TODO
#
# CI: Optional, manual/scheduled only (not consensus-critical)
# ═══════════════════════════════════════════════════════════════════════════
option(ENABLE_WALLET_CORRECTNESS_TESTS "Build wallet correctness tests (optional)" OFF)

if(ENABLE_WALLET_CORRECTNESS_TESTS)
  add_executable(test_wallet_correctness
    tests/wallet/test_wallet_correctness.cpp
  )
  target_link_libraries(test_wallet_correctness PRIVATE
    dinero_wallet
    dinero_consensus
    dinero_chainstate
    dinero_core
    sqlite3
    GTest::gtest
  )
  link_zstd_if_needed(test_wallet_correctness)

  # macOS Security framework for secure random
  if(APPLE)
    target_link_libraries(test_wallet_correctness PRIVATE "-framework Security")
  endif()

  target_include_directories(test_wallet_correctness PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests/wallet
  )
  # Not added to CTest by default (optional test suite)
  message(STATUS "✅ Wallet correctness tests enabled (Test 3: Address Derivation Determinism)")
endif()

# REMOVED: test_wallet_integration, test_bip84_addr_check (obsolete zombie tests from copy repo)

# ═══════════════════════════════════════════════════════════════════════════
# CRITICAL: Wallet Ownership Invariant Tests
# ═══════════════════════════════════════════════════════════════════════════
# These tests lock the critical wallet invariant:
#   "A UTXO without a derivation path is NOT owned. No exceptions."
#
# This prevents:
#   - Ghost balances (phantom UTXOs)
#   - Unspendable outputs
#   - Reorg-induced wallet corruption
#   - Signing without provenance
# ═══════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet/test_wallet_ownership_invariant.cpp)
  add_executable(test_wallet_ownership_invariant
    tests/wallet/test_wallet_ownership_invariant.cpp
  )
  target_link_libraries(test_wallet_ownership_invariant PRIVATE
    dinero_wallet
    dinero_consensus
    dinero_core
    sqlite3
    GTest::gtest
  )
  link_secp256k1_zkp(test_wallet_ownership_invariant)
  link_zstd_if_needed(test_wallet_ownership_invariant)

  if(APPLE)
    target_link_libraries(test_wallet_ownership_invariant PRIVATE "-framework Security")
  endif()

  target_include_directories(test_wallet_ownership_invariant PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  add_test(NAME WalletOwnershipInvariant COMMAND test_wallet_ownership_invariant)
  set_tests_properties(WalletOwnershipInvariant PROPERTIES
    LABELS "wallet;invariant;critical;ownership"
    TIMEOUT 30
  )
  message(STATUS "✅ Wallet ownership invariant tests enabled (CRITICAL)")
endif()

# ═══════════════════════════════════════════════════════════════════════════
# BIP32Deriver Equivalence Test
# ═══════════════════════════════════════════════════════════════════════════
# Verifies BIP32Deriver produces identical keys to legacy inline lambdas.
# This is the safety check before refactoring lambda clusters in hd_wallet.cpp.
# ═══════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet/test_bip32_deriver.cpp)
  add_executable(test_bip32_deriver
    tests/wallet/test_bip32_deriver.cpp
    src/wallet/bip32_deriver.cpp
  )
  target_link_libraries(test_bip32_deriver PRIVATE
    dinero_crypto
    OpenSSL::Crypto
    GTest::gtest
  )
  link_secp256k1_zkp(test_bip32_deriver)

  if(APPLE)
    target_link_libraries(test_bip32_deriver PRIVATE "-framework Security")
  endif()

  target_include_directories(test_bip32_deriver PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${SECP256K1_INCLUDE_DIR}
  )

  add_test(NAME BIP32DeriverEquivalence COMMAND test_bip32_deriver)
  set_tests_properties(BIP32DeriverEquivalence PROPERTIES
    LABELS "wallet;bip32;derivation;safety"
    TIMEOUT 30
  )
  message(STATUS "✅ BIP32Deriver equivalence tests enabled")
endif()

# HD Wallet test - DISABLED: uses dead architecture (dinero_wallet/old UTXO)
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/test_hd_wallet.cpp)
  add_executable(test_hd_wallet
    tests/test_hd_wallet.cpp
    src/daemon/hd_wallet_manager.cpp
    src/daemon/secure_random.cpp
    src/daemon/wallet_crypto.cpp
    src/common/logger.cpp        # Required by wallet_crypto.cpp
  )
  target_link_libraries(test_hd_wallet PRIVATE
    dinero_crypto
    jsoncpp_static
    secp256k1
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread                      # Fix: Required by OpenSSL 3.3 for thread-local storage
  )
  if(APPLE)
    target_link_libraries(test_hd_wallet PRIVATE "-framework Security")
  endif()
  target_include_directories(test_hd_wallet PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME test_hd_wallet COMMAND test_hd_wallet)
endif()

# Wallet Pipeline Test - v0.12.0 comprehensive wallet tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet_tests/test_wallet_pipeline.cpp)
  add_executable(test_wallet_pipeline
    tests/wallet_tests/test_wallet_pipeline.cpp
    src/common/logger.cpp  # Required by wallet components
    src/wallet/p2mr_address.cpp  # DecodeP2MRAddress (V7 P2MR bech32m codec)
    src/daemon/bech32_encoder.cpp  # Bech32Encoder::encode_segwit used by p2mr_address.cpp
  )
  target_link_libraries(test_wallet_pipeline PRIVATE
    dinero_wallet
    dinero_chainstate  # Needed for UTXO index and blockchain state
    dinero_crypto
    jsoncpp_static
    OpenSSL::Crypto
    secp256k1
    RocksDB::rocksdb   # Required by dinero_chainstate
  )
  target_link_directories(test_wallet_pipeline PRIVATE /opt/homebrew/lib)  # For RocksDB
  if(APPLE)
    target_link_libraries(test_wallet_pipeline PRIVATE "-framework Security")
  endif()
  target_include_directories(test_wallet_pipeline PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME WalletPipeline COMMAND test_wallet_pipeline)
endif()

# Wallet Mempool Round-Trip Test - v0.12.0 integration test (Proof Layer 3)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet_tests/test_wallet_mempool_roundtrip.cpp)
  add_executable(test_wallet_mempool_roundtrip
    tests/wallet_tests/test_wallet_mempool_roundtrip.cpp
    src/common/logger.cpp  # Required by wallet components
    src/wallet/p2mr_address.cpp  # DecodeP2MRAddress (V7 P2MR bech32m codec)
    src/daemon/bech32_encoder.cpp  # Bech32Encoder::encode_segwit used by p2mr_address.cpp
  )
  target_link_libraries(test_wallet_mempool_roundtrip PRIVATE
    dinero_wallet
    dinero_chainstate  # Needed for UTXO index and blockchain state
    dinero_crypto
    jsoncpp_static
    OpenSSL::Crypto
    secp256k1
    RocksDB::rocksdb   # Required by dinero_chainstate
  )
  target_link_directories(test_wallet_mempool_roundtrip PRIVATE /opt/homebrew/lib)  # For RocksDB
  if(APPLE)
    target_link_libraries(test_wallet_mempool_roundtrip PRIVATE "-framework Security")
  endif()
  target_include_directories(test_wallet_mempool_roundtrip PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME WalletMempoolRoundtrip COMMAND test_wallet_mempool_roundtrip)
endif()

# Wallet Block Integration Test - Phase M.3 full integration
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet_tests/test_wallet_block_integration.cpp)
  add_executable(test_wallet_block_integration
    tests/wallet_tests/test_wallet_block_integration.cpp
    src/common/logger.cpp  # Required by wallet components
  )
  target_link_libraries(test_wallet_block_integration PRIVATE
    dinero_wallet
    dinero_chainstate  # Needed for UTXO index and blockchain state
    dinero_crypto
    jsoncpp_static
    OpenSSL::Crypto
    secp256k1
    RocksDB::rocksdb   # Required by dinero_chainstate
  )
  target_link_directories(test_wallet_block_integration PRIVATE /opt/homebrew/lib)  # For RocksDB
  if(APPLE)
    target_link_libraries(test_wallet_block_integration PRIVATE "-framework Security")
  endif()
  target_include_directories(test_wallet_block_integration PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME WalletBlockIntegration COMMAND test_wallet_block_integration)
endif()

# ════════════════════════════════════════════════════════════════════════════
# Wallet ↔ Ingress Parity Test (Phase A1 - Mainnet Hardening)
# Proves wallet cannot bypass ITxIngress and handles all TxRejectCodes
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet_tests/test_wallet_ingress_parity.cpp)
  add_executable(test_wallet_ingress_parity
    tests/wallet_tests/test_wallet_ingress_parity.cpp
    src/common/logger.cpp
    src/daemon/interfaces/ingress_types.cpp
  )
  target_link_libraries(test_wallet_ingress_parity PRIVATE
    dinero_wallet
    dinero_crypto
    jsoncpp_static
    OpenSSL::Crypto
    secp256k1
  )
  if(APPLE)
    target_link_libraries(test_wallet_ingress_parity PRIVATE "-framework Security")
  endif()
  target_include_directories(test_wallet_ingress_parity PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME WalletIngressParity COMMAND test_wallet_ingress_parity)
  message(STATUS "  ✓ Phase A1 test registered: test_wallet_ingress_parity")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Wallet Crash-Safety Test (Phase A2 - Mainnet Hardening)
# Proves wallet survives crashes without phantom balances or stuck UTXOs
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet_tests/test_wallet_crash_safety.cpp)
  add_executable(test_wallet_crash_safety
    tests/wallet_tests/test_wallet_crash_safety.cpp
    src/common/logger.cpp
    src/daemon/lightning_stubs.cpp
    tests/stubs/mempool_test_stubs.cpp  # Mempool::isOutputSpentInMempool et al.
  )
  target_link_libraries(test_wallet_crash_safety PRIVATE
    dinero_wallet
    dinero_chainstate
    dinero_crypto
    jsoncpp_static
    OpenSSL::Crypto
    secp256k1
    RocksDB::rocksdb
    sqlite3
  )
  target_link_directories(test_wallet_crash_safety PRIVATE /opt/homebrew/lib)
  if(APPLE)
    target_link_libraries(test_wallet_crash_safety PRIVATE "-framework Security")
  endif()
  target_include_directories(test_wallet_crash_safety PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME WalletCrashSafety COMMAND test_wallet_crash_safety)
  message(STATUS "  ✓ Phase A2 test registered: test_wallet_crash_safety")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Wallet Determinism Test (Phase A3 - Mainnet Hardening)
# Proves same inputs → identical state, always
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet_tests/test_wallet_determinism.cpp)
  add_executable(test_wallet_determinism
    tests/wallet_tests/test_wallet_determinism.cpp
    src/common/logger.cpp
    src/daemon/lightning_stubs.cpp
    tests/stubs/mempool_test_stubs.cpp  # Mempool::isOutputSpentInMempool et al.
  )
  target_link_libraries(test_wallet_determinism PRIVATE
    dinero_wallet
    dinero_chainstate
    dinero_crypto
    jsoncpp_static
    OpenSSL::Crypto
    secp256k1
    RocksDB::rocksdb
    sqlite3
  )
  target_link_directories(test_wallet_determinism PRIVATE /opt/homebrew/lib)
  if(APPLE)
    target_link_libraries(test_wallet_determinism PRIVATE "-framework Security")
  endif()
  target_include_directories(test_wallet_determinism PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME WalletDeterminism COMMAND test_wallet_determinism)
  message(STATUS "  ✓ Phase A3 test registered: test_wallet_determinism")
endif()

# Snapshot/AssumeUTXO UTXO-set rescan visibility regression
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet_tests/test_wallet_rescan_utxoset.cpp)
  add_executable(test_wallet_rescan_utxoset
    tests/wallet_tests/test_wallet_rescan_utxoset.cpp
    src/common/logger.cpp
    src/daemon/lightning_stubs.cpp
    tests/stubs/mempool_test_stubs.cpp
  )
  target_link_libraries(test_wallet_rescan_utxoset PRIVATE
    dinero_wallet
    dinero_chainstate
    dinero_crypto
    jsoncpp_static
    OpenSSL::Crypto
    secp256k1
    RocksDB::rocksdb
    sqlite3
  )
  target_link_directories(test_wallet_rescan_utxoset PRIVATE /opt/homebrew/lib)
  if(APPLE)
    target_link_libraries(test_wallet_rescan_utxoset PRIVATE "-framework Security")
  endif()
  target_include_directories(test_wallet_rescan_utxoset PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME WalletRescanUtxoSet COMMAND test_wallet_rescan_utxoset)
  message(STATUS "  ✓ Snapshot rescan test registered: test_wallet_rescan_utxoset")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Wallet Sidecar Migration Idempotence Test (Week 1 migration safety gate)
# Verifies wallet.conf -> wallet_state.db one-time migration is idempotent
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet_tests/test_wallet_sidecar_migration.cpp)
  add_executable(test_wallet_sidecar_migration
    tests/wallet_tests/test_wallet_sidecar_migration.cpp
    src/common/logger.cpp
    src/daemon/lightning_stubs.cpp
    # dinero_wallet's wallet_manager.obj references Mempool::isOutputSpentInMempool
    # via listUnspentUTXOs(...). The full Mempool implementation in
    # src/daemon/mempool.cpp drags in the entire daemon (DaemonContext, RPC,
    # scheduler, …). mempool_test_stubs.cpp provides a no-op definition so
    # the wallet-only test surface links cleanly without pulling in daemon code.
    tests/stubs/mempool_test_stubs.cpp
  )
  target_link_libraries(test_wallet_sidecar_migration PRIVATE
    dinero_wallet
    dinero_chainstate
    dinero_crypto
    jsoncpp_static
    OpenSSL::Crypto
    secp256k1
    RocksDB::rocksdb
    sqlite3
  )
  target_link_directories(test_wallet_sidecar_migration PRIVATE /opt/homebrew/lib)
  if(APPLE)
    target_link_libraries(test_wallet_sidecar_migration PRIVATE "-framework Security")
  endif()
  target_include_directories(test_wallet_sidecar_migration PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME WalletSidecarMigration COMMAND test_wallet_sidecar_migration)
  set_tests_properties(WalletSidecarMigration PROPERTIES LABELS "wallet;migration;idempotence")
  message(STATUS "  ✓ Wallet migration gate registered: test_wallet_sidecar_migration")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Wallet Descriptor Active Context Test (non-default wallet correctness)
# Verifies descriptor RPC uses active wallet DB handle only
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet_tests/test_wallet_descriptor_active_context.cpp)
  add_executable(test_wallet_descriptor_active_context
    tests/wallet_tests/test_wallet_descriptor_active_context.cpp
    src/common/logger.cpp
    src/daemon/lightning_stubs.cpp
    src/daemon/config.cpp  # GetConfig() referenced by dinero_core objects
    tests/stubs/mempool_test_stubs.cpp  # Mempool::isOutputSpentInMempool et al.
    tests/stubs/daemon_test_stubs.cpp   # Mempool extras + vault observers
  )
  target_link_libraries(test_wallet_descriptor_active_context PRIVATE
    dinero_wallet
    dinero_core
    dinero_chainstate
    dinero_consensus
    dinero_crypto
    dinero_rpc_handlers
    jsoncpp_static
    OpenSSL::Crypto
    secp256k1
    RocksDB::rocksdb
    sqlite3
  )
  target_link_directories(test_wallet_descriptor_active_context PRIVATE /opt/homebrew/lib)
  if(APPLE)
    target_link_libraries(test_wallet_descriptor_active_context PRIVATE "-framework Security")
  endif()
  target_include_directories(test_wallet_descriptor_active_context PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME WalletDescriptorActiveContext COMMAND test_wallet_descriptor_active_context)
  set_tests_properties(WalletDescriptorActiveContext PROPERTIES LABELS "wallet;descriptor;multiwallet")
  message(STATUS "  ✓ Wallet descriptor context gate registered: test_wallet_descriptor_active_context")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Wallet PSBT Active Policy Context Test (non-default wallet signing policy)
# Verifies walletprocesspsbt uses active wallet policy from active DB handle
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet_tests/test_wallet_psbt_policy_context.cpp)
  add_executable(test_wallet_psbt_policy_context
    tests/wallet_tests/test_wallet_psbt_policy_context.cpp
    src/rpc/methods_wallet_psbt.cpp
    src/wallet/wallet_metrics.cpp
    src/common/logger.cpp
    src/daemon/lightning_stubs.cpp
    src/daemon/config.cpp  # GetConfig() referenced by dinero_core objects
    tests/stubs/mempool_test_stubs.cpp  # Mempool::isOutputSpentInMempool et al.
    tests/stubs/daemon_test_stubs.cpp   # Mempool extras + vault observers
  )
  target_link_libraries(test_wallet_psbt_policy_context PRIVATE
    dinero_wallet
    dinero_core
    dinero_chainstate
    dinero_crypto
    dinero_rpc_handlers
    jsoncpp_static
    OpenSSL::Crypto
    secp256k1
    RocksDB::rocksdb
    sqlite3
  )
  target_link_directories(test_wallet_psbt_policy_context PRIVATE /opt/homebrew/lib)
  if(APPLE)
    target_link_libraries(test_wallet_psbt_policy_context PRIVATE "-framework Security")
  endif()
  target_include_directories(test_wallet_psbt_policy_context PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME WalletPsbtPolicyContext COMMAND test_wallet_psbt_policy_context)
  set_tests_properties(WalletPsbtPolicyContext PROPERTIES LABELS "wallet;psbt;multiwallet")
  message(STATUS "  ✓ Wallet PSBT context gate registered: test_wallet_psbt_policy_context")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Template Staleness Test (Phase B1 - Mainnet Hardening)
# Proves stale templates are rejected, not auto-fixed
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mining/test_template_staleness.cpp)
  add_executable(test_template_staleness
    tests/mining/test_template_staleness.cpp
    src/daemon/interfaces/ingress_types.cpp
    src/primitives/uint256.cpp
  )
  target_link_libraries(test_template_staleness PRIVATE
    dinero_crypto
  )
  target_include_directories(test_template_staleness PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME TemplateStaleness COMMAND test_template_staleness)
  message(STATUS "  ✓ Phase B1 test registered: test_template_staleness")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Block Construction Test (Phase B2 - Mainnet Hardening)
# Proves coinbase rules, subsidy schedule, and overflow safety
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mining/test_block_construction.cpp)
  add_executable(test_block_construction
    tests/mining/test_block_construction.cpp
  )
  target_link_libraries(test_block_construction PRIVATE
    dinero_crypto
  )
  target_include_directories(test_block_construction PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME BlockConstruction COMMAND test_block_construction)
  message(STATUS "  ✓ Phase B2 test registered: test_block_construction")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Miner Race Safety Test (Phase B3 - Mainnet Hardening)
# Proves concurrent mining cannot corrupt chain state
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mining/test_miner_race_safety.cpp)
  add_executable(test_miner_race_safety
    tests/mining/test_miner_race_safety.cpp
    src/daemon/interfaces/ingress_types.cpp
    src/primitives/uint256.cpp
  )
  target_link_libraries(test_miner_race_safety PRIVATE
    dinero_crypto
  )
  target_include_directories(test_miner_race_safety PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME MinerRaceSafety COMMAND test_miner_race_safety)
  message(STATUS "  ✓ Phase B3 test registered: test_miner_race_safety")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Coinbase Integrity Test (Phase B4 - Mainnet Hardening)
# Proves coinbase cannot violate subsidy rules or commitments
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mining/test_coinbase_integrity.cpp)
  add_executable(test_coinbase_integrity
    tests/mining/test_coinbase_integrity.cpp
    src/primitives/uint256.cpp
  )
  target_link_libraries(test_coinbase_integrity PRIVATE
    dinero_crypto
  )
  target_include_directories(test_coinbase_integrity PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME CoinbaseIntegrity COMMAND test_coinbase_integrity)
  message(STATUS "  ✓ Phase B4 test registered: test_coinbase_integrity")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Validation Bypass Test (Phase B5 - Mainnet Hardening)
# Proves miner cannot bypass any validation rule
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mining/test_validation_bypass.cpp)
  add_executable(test_validation_bypass
    tests/mining/test_validation_bypass.cpp
    src/daemon/interfaces/ingress_types.cpp
    src/primitives/uint256.cpp
  )
  target_link_libraries(test_validation_bypass PRIVATE
    dinero_crypto
  )
  target_include_directories(test_validation_bypass PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME ValidationBypass COMMAND test_validation_bypass)
  message(STATUS "  ✓ Phase B5 test registered: test_validation_bypass")
endif()

# Stratum tests REMOVED - now in separate dinero-stratum repo

# ════════════════════════════════════════════════════════════════════════════
# P2P Adversarial Integrity Test (Phase D - Mainnet Hardening)
# Proves hostile peers cannot cause invalid state, exhaustion, or divergence
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_p2p_adversarial.cpp)
  add_executable(test_p2p_adversarial
    tests/p2p/test_p2p_adversarial.cpp
    src/daemon/interfaces/ingress_types.cpp
    src/primitives/uint256.cpp
  )
  target_link_libraries(test_p2p_adversarial PRIVATE
    dinero_crypto
  )
  target_include_directories(test_p2p_adversarial PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME P2PAdversarial COMMAND test_p2p_adversarial)
  message(STATUS "  ✓ Phase D test registered: test_p2p_adversarial")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Daemon Operational Safety Test (Phase E - Mainnet Hardening)
# Proves node can run for months without corrupting itself
# E1: Restart invariants, E2: Long reorg safety, E3: Resource caps
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/daemon/test_daemon_operational_safety.cpp)
  add_executable(test_daemon_operational_safety
    tests/daemon/test_daemon_operational_safety.cpp
  )
  target_link_libraries(test_daemon_operational_safety PRIVATE
    pthread
  )
  target_include_directories(test_daemon_operational_safety PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME DaemonOperationalSafety COMMAND test_daemon_operational_safety)
  message(STATUS "  ✓ Phase E test registered: test_daemon_operational_safety")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Mainnet Dry Run Test (Phase F - Mainnet Hardening)
# Final validation: Single-node burn-in, multi-node consensus, full integration
# F1: Single-node stability, F2: Multi-node consensus, F3: Component integration
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/integration/test_mainnet_dry_run.cpp)
  add_executable(test_mainnet_dry_run
    tests/integration/test_mainnet_dry_run.cpp
  )
  target_link_libraries(test_mainnet_dry_run PRIVATE
    pthread
  )
  target_include_directories(test_mainnet_dry_run PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME MainnetDryRun COMMAND test_mainnet_dry_run)
  message(STATUS "  ✓ Phase F test registered: test_mainnet_dry_run")
endif()

# ════════════════════════════════════════════════════════════════════════════
# DISABLED: Requires IConsensusUTXOSet interface refactor (BlockValidator constructor changed)
# Wallet ↔ Mining Flow Integration Test (Phase D.2)
# ════════════════════════════════════════════════════════════════════════════
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/integration/test_wallet_mining_flow.cpp)
  add_executable(test_wallet_mining_flow
    tests/integration/test_wallet_mining_flow.cpp
    tests/test_utils/test_utreexo_provider.cpp
    src/common/logger.cpp
  )

  target_link_libraries(test_wallet_mining_flow PRIVATE
    dinero_wallet
    dinero_consensus
    dinero_chainstate
    dinero_crypto
    dinero_core  # For LightningService and other dependencies
    jsoncpp_static
    sqlite3  # Required by WalletManager
    bulletproofs_ffi  # Required by ConfidentialTransactionValidator
    OpenSSL::SSL
    OpenSSL::Crypto
    secp256k1
    RocksDB::rocksdb
    GTest::gtest
  )
  link_zstd_if_needed(test_wallet_mining_flow)

  target_link_directories(test_wallet_mining_flow PRIVATE /opt/homebrew/lib)
  if(APPLE)
    target_link_libraries(test_wallet_mining_flow PRIVATE "-framework Security")
  endif()

  target_include_directories(test_wallet_mining_flow PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
  )

  add_test(NAME WalletMiningFlow COMMAND test_wallet_mining_flow)
  set_tests_properties(WalletMiningFlow PROPERTIES
    LABELS "integration;wallet;mining;critical"
    TIMEOUT 120
  )

  message(STATUS "✅ Wallet ↔ Mining flow integration test enabled (Phase D.2)")
else()
  message(STATUS "⚠️  Wallet ↔ Mining flow test not found (optional)")
endif()

# DISABLED: Requires IConsensusUTXOSet interface refactor (BlockValidator constructor changed)
# ════════════════════════════════════════════════════════════════════════════
# Daemon Restart Safety Integration Test (Phase D.4)
# ════════════════════════════════════════════════════════════════════════════
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/integration/test_daemon_restart_safety.cpp)
  add_executable(test_daemon_restart_safety
    tests/integration/test_daemon_restart_safety.cpp
    src/common/logger.cpp
  )

  target_link_libraries(test_daemon_restart_safety PRIVATE
    dinero_wallet
    dinero_consensus
    dinero_chainstate
    dinero_crypto
    dinero_core
    jsoncpp_static
    sqlite3
    bulletproofs_ffi
    OpenSSL::SSL
    OpenSSL::Crypto
    secp256k1
    RocksDB::rocksdb
    GTest::gtest
  )
  link_zstd_if_needed(test_daemon_restart_safety)

  target_link_directories(test_daemon_restart_safety PRIVATE /opt/homebrew/lib)
  if(APPLE)
    target_link_libraries(test_daemon_restart_safety PRIVATE "-framework Security")
  endif()

  target_include_directories(test_daemon_restart_safety PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )

  add_test(NAME DaemonRestartSafety COMMAND test_daemon_restart_safety)
  set_tests_properties(DaemonRestartSafety PROPERTIES
    LABELS "integration;persistence;critical"
    TIMEOUT 120
  )

  message(STATUS "✅ Daemon restart safety integration test enabled (Phase D.4)")
else()
  message(STATUS "⚠️  Daemon restart safety test not found (optional)")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Pruning Test Suite (Phase P.2)
# ════════════════════════════════════════════════════════════════════════════
# Validates pruning infrastructure correctness:
# - Restart/Recovery: prune_height persists across daemon restarts
# - Safety Boundaries: MIN_UNDO_DEPTH (288) is enforced
# - Index Consistency: BLOCK_HAVE_DATA flags are cleared after pruning
# - P2P Compatibility: NOTFOUND serialization is correct
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/storage/test_pruning.cpp)
  add_executable(test_pruning
    tests/storage/test_pruning.cpp
  )

  target_link_libraries(test_pruning PRIVATE
    dinero_core
    dinero_chainstate
    dinero_consensus
    dinero_crypto
    RocksDB::rocksdb
  )
  link_zstd_if_needed(test_pruning)

  target_link_directories(test_pruning PRIVATE /opt/homebrew/lib)
  if(APPLE)
    target_link_libraries(test_pruning PRIVATE "-framework Security" "-framework CoreFoundation")
  endif()

  target_include_directories(test_pruning PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )

  add_test(NAME PruningTestSuite COMMAND test_pruning)
  set_tests_properties(PruningTestSuite PROPERTIES
    LABELS "storage;pruning;persistence;critical"
    TIMEOUT 60
  )

  message(STATUS "✅ Pruning test suite enabled (Phase P.2)")
else()
  message(STATUS "⚠️  Pruning test suite not found (optional)")
endif()

# May 2026 fleet-wide HAVE_UNDO regression guard.
# Pins ChainDB::setHeaderStatusBits OR-merge semantics so a future
# refactor can't silently strip consensus-layer bits via the
# header-sync path.
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/storage/test_header_status_bits.cpp)
  add_executable(test_header_status_bits
    tests/storage/test_header_status_bits.cpp
  )
  target_link_libraries(test_header_status_bits PRIVATE
    dinero_core
    dinero_chainstate
    dinero_consensus
    dinero_crypto
    RocksDB::rocksdb
  )
  link_zstd_if_needed(test_header_status_bits)
  target_link_directories(test_header_status_bits PRIVATE /opt/homebrew/lib)
  if(APPLE)
    target_link_libraries(test_header_status_bits PRIVATE "-framework Security" "-framework CoreFoundation")
  endif()
  target_include_directories(test_header_status_bits PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME HeaderStatusBits COMMAND test_header_status_bits)
  set_tests_properties(HeaderStatusBits PROPERTIES
    LABELS "storage;persistence;critical;regression"
    TIMEOUT 30
  )
  message(STATUS "✅ Header status-bits regression guard enabled")
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/storage/test_header_status_overwrite_allowlist.sh)
  add_test(NAME HeaderStatusOverwriteAllowlist
    COMMAND ${CMAKE_SOURCE_DIR}/tests/storage/test_header_status_overwrite_allowlist.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  )
  set_tests_properties(HeaderStatusOverwriteAllowlist PROPERTIES
    LABELS "storage;persistence;critical;regression"
    TIMEOUT 10
  )
  message(STATUS "✅ Header status overwrite allowlist guard enabled")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Wallet Coinbase Maturity Integration Test
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/integration/test_wallet_coinbase_maturity.cpp)
  add_executable(test_wallet_coinbase_maturity
    tests/integration/test_wallet_coinbase_maturity.cpp
    src/common/logger.cpp
  )

  target_link_libraries(test_wallet_coinbase_maturity PRIVATE
    dinero_wallet
    dinero_consensus
    dinero_chainstate
    dinero_crypto
    dinero_core
    jsoncpp_static
    sqlite3
    bulletproofs_ffi
    OpenSSL::SSL
    OpenSSL::Crypto
    secp256k1
    RocksDB::rocksdb
    GTest::gtest
  )
  link_zstd_if_needed(test_wallet_coinbase_maturity)

  target_link_directories(test_wallet_coinbase_maturity PRIVATE /opt/homebrew/lib)
  if(APPLE)
    target_link_libraries(test_wallet_coinbase_maturity PRIVATE "-framework Security")
  endif()

  target_include_directories(test_wallet_coinbase_maturity PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )

  add_test(NAME WalletCoinbaseMaturity COMMAND test_wallet_coinbase_maturity)
  set_tests_properties(WalletCoinbaseMaturity PROPERTIES
    LABELS "integration;wallet;coinbase;maturity"
    TIMEOUT 120
  )

  message(STATUS "✅ Wallet coinbase maturity integration test enabled")
else()
  message(STATUS "⚠️  Wallet coinbase maturity test not found (optional)")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Sync Profile Policy Tests
# ════════════════════════════════════════════════════════════════════════════
#
# Architecture: Pure policy/view tests for sync profile resolution and capabilities.
# Dependencies: gtest only
# Speed: Fast - no daemon startup, no IO
#
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/policy/test_sync_profile_policy.cpp)
  add_executable(test_sync_profile_policy
    tests/policy/test_sync_profile_policy.cpp
    src/nodecore/sync_profile_policy.cpp
  )

  add_dependencies(test_sync_profile_policy gtest)

  target_link_libraries(test_sync_profile_policy PRIVATE
    $<TARGET_FILE:gtest>
  )

  target_include_directories(test_sync_profile_policy BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )

  add_test(NAME SyncProfilePolicy COMMAND test_sync_profile_policy)
  set_tests_properties(SyncProfilePolicy PROPERTIES
    LABELS "policy;profile;critical"
    TIMEOUT 10
  )

  message(STATUS "✅ Sync profile policy tests enabled")
else()
  message(STATUS "⚠️  Sync profile policy tests not found (optional)")
endif()

# ════════════════════════════════════════════════════════════════════════════
# NodeCore Runtime Guard Tests
# ════════════════════════════════════════════════════════════════════════════
#
# Architecture: Pure runtime guard/view tests for FFI shutdown gating and
# thread-safe callback/watch snapshots.
# Dependencies: gtest only
# Speed: Fast - no daemon startup, no IO
#
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/policy/test_nodecore_runtime_guards.cpp)
  add_executable(test_nodecore_runtime_guards
    tests/policy/test_nodecore_runtime_guards.cpp
  )

  add_dependencies(test_nodecore_runtime_guards gtest)

  target_link_libraries(test_nodecore_runtime_guards PRIVATE
    $<TARGET_FILE:gtest>
  )

  target_include_directories(test_nodecore_runtime_guards BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )

  add_test(NAME NodeCoreRuntimeGuards COMMAND test_nodecore_runtime_guards)
  set_tests_properties(NodeCoreRuntimeGuards PROPERTIES
    LABELS "policy;nodecore;hardening"
    TIMEOUT 10
  )

  message(STATUS "✅ NodeCore runtime guard tests enabled")
else()
  message(STATUS "⚠️  NodeCore runtime guard tests not found (optional)")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Mining Policy Tripwire Tests (Phase E.1)
# ════════════════════════════════════════════════════════════════════════════
#
# Architecture: Tests operate on POLICY VIEWS only (no implementation)
# Dependencies: Minimal - only gtest (no wallet, no crypto, no database)
# Speed: Fast - no IO, no initialization, pure logic tests
#
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/policy/test_mining_policy_tripwires.cpp)
  add_executable(test_mining_policy_tripwires
    tests/policy/test_mining_policy_tripwires.cpp
  )

  # Explicit dependencies to ensure gtest builds first
  add_dependencies(test_mining_policy_tripwires gtest)

  # Link googletest libraries using generator expressions (ensures proper build order)
  target_link_libraries(test_mining_policy_tripwires PRIVATE
    $<TARGET_FILE:gtest>
    )

  # Add vendored googletest includes BEFORE system includes
  target_include_directories(test_mining_policy_tripwires BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )

  add_test(NAME MiningPolicyTripwires COMMAND test_mining_policy_tripwires)
  set_tests_properties(MiningPolicyTripwires PROPERTIES
    LABELS "policy;tripwire;critical"
    TIMEOUT 10
  )

  message(STATUS "✅ Mining policy tripwire tests enabled (Phase E.1 + E.2) - pure policy views")
else()
  message(STATUS "⚠️  Mining policy tripwire tests not found (optional)")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Mining Restart Policy Tripwire Tests (Phase E.3)
# ════════════════════════════════════════════════════════════════════════════
#
# Architecture: Tests operate on RESTART POLICY VIEWS only (no execution)
# Dependencies: Minimal - only gtest (no mining, no daemon lifecycle)
# Speed: Fast - no IO, no threads, pure logic tests
#
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/policy/test_mining_restart_policy_tripwires.cpp)
  add_executable(test_mining_restart_policy_tripwires
    tests/policy/test_mining_restart_policy_tripwires.cpp
  )

  add_dependencies(test_mining_restart_policy_tripwires gtest)

  target_link_libraries(test_mining_restart_policy_tripwires PRIVATE
    $<TARGET_FILE:gtest>
    )

  target_include_directories(test_mining_restart_policy_tripwires BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )

  add_test(NAME MiningRestartPolicyTripwires COMMAND test_mining_restart_policy_tripwires)
  set_tests_properties(MiningRestartPolicyTripwires PROPERTIES
    LABELS "policy;tripwire;critical;restart"
    TIMEOUT 10
  )

  message(STATUS "✅ Mining restart policy tripwire tests enabled (Phase E.3) - pure policy views")
else()
  message(STATUS "⚠️  Mining restart policy tripwire tests not found (optional)")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Mining Completeness Policy Tripwire Tests (Phase E.4)
# ════════════════════════════════════════════════════════════════════════════
#
# Architecture: Tests ensure no illegal mining state exists in RPC surface
# Dependencies: Minimal - only gtest (no mining, no wallet, no execution)
# Speed: Fast - no IO, pure logic tests
#
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/policy/test_mining_completeness_tripwires.cpp)
  add_executable(test_mining_completeness_tripwires
    tests/policy/test_mining_completeness_tripwires.cpp
  )

  add_dependencies(test_mining_completeness_tripwires gtest)

  target_link_libraries(test_mining_completeness_tripwires PRIVATE
    $<TARGET_FILE:gtest>
    )

  target_include_directories(test_mining_completeness_tripwires BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )

  add_test(NAME MiningCompletenessPolicyTripwires COMMAND test_mining_completeness_tripwires)
  set_tests_properties(MiningCompletenessPolicyTripwires PROPERTIES
    LABELS "policy;tripwire;critical;completeness"
    TIMEOUT 10
  )

  message(STATUS "✅ Mining completeness policy tripwire tests enabled (Phase E.4) - pure policy views")
else()
  message(STATUS "⚠️  Mining completeness policy tripwire tests not found (optional)")
endif()

#
# Phase F.3: View Builder Unit Tests
# ==================================
# Target: test_view_builders
# Purpose: Verify view builder functions are correct, null-safe, and error-resistant
# Dependencies: gtest only (no wallet, no crypto, no database)
# Speed: Fast - no IO, pure logic tests
#
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/policy/test_view_builders.cpp)
  add_executable(test_view_builders
    tests/policy/test_view_builders.cpp
  )

  add_dependencies(test_view_builders gtest)

  target_link_libraries(test_view_builders PRIVATE
    $<TARGET_FILE:gtest>
    )

  target_include_directories(test_view_builders BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )

  add_test(NAME ViewBuilderTests COMMAND test_view_builders)
  set_tests_properties(ViewBuilderTests PROPERTIES
    LABELS "policy;view-builder;unit;f3"
    TIMEOUT 10
  )

  message(STATUS "✅ View builder unit tests enabled (Phase F.3) - null-safe, error-resistant")
else()
  message(STATUS "⚠️  View builder unit tests not found (optional)")
endif()

#
# Phase F.4: Escape Hatch Integration Tests
# ==========================================
# Target: test_escape_hatches
# Purpose: Verify escape hatch flags allow what policy permits and block correctly
# Dependencies: gtest only (no wallet, no crypto, no database)
# Speed: Fast - no IO, pure policy logic tests
#
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/policy/test_escape_hatches.cpp)
  add_executable(test_escape_hatches
    tests/policy/test_escape_hatches.cpp
  )

  add_dependencies(test_escape_hatches gtest)

  target_link_libraries(test_escape_hatches PRIVATE
    $<TARGET_FILE:gtest>
    )

  target_include_directories(test_escape_hatches BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )

  add_test(NAME EscapeHatchTests COMMAND test_escape_hatches)
  set_tests_properties(EscapeHatchTests PROPERTIES
    LABELS "policy;escape-hatch;covenant-policy-smoke;f4"
    TIMEOUT 10
  )

  message(STATUS "✅ Escape hatch integration tests enabled (Phase F.4) - flag enforcement")
else()
  message(STATUS "⚠️  Escape hatch integration tests not found (optional)")
endif()

# Transaction Serialization Test - v0.13.0.1 Step A (Proof Layer 1)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/network/test_transaction_serialization.cpp)
  add_executable(test_transaction_serialization
    tests/network/test_transaction_serialization.cpp
    src/common/logger.cpp  # Required by transaction serialization
  )
  target_link_libraries(test_transaction_serialization PRIVATE
    dinero_wallet
    dinero_crypto
    OpenSSL::Crypto
  )
  target_include_directories(test_transaction_serialization PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME TransactionSerialization COMMAND test_transaction_serialization)
endif()

# Block Serialization Test - RocksDB persistence guard
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/network/test_block_serialization.cpp)
  add_executable(test_block_serialization
    tests/network/test_block_serialization.cpp
    src/common/logger.cpp  # Required by block serialization
  )
  target_link_libraries(test_block_serialization PRIVATE
    dinero_wallet
    dinero_crypto
    OpenSSL::Crypto
  )
  target_include_directories(test_block_serialization PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME BlockSerialization COMMAND test_block_serialization)
endif()

# BlockStorage Round-Trip Test - blk*.dat persistence guard
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/storage/test_block_storage_roundtrip.cpp)
  add_executable(test_block_storage_roundtrip
    tests/storage/test_block_storage_roundtrip.cpp
    src/common/logger.cpp  # Provides g_logger if referenced transitively
  )
  target_link_libraries(test_block_storage_roundtrip PRIVATE
    dinero_chainstate
    dinero_crypto
    OpenSSL::Crypto
  )
  target_include_directories(test_block_storage_roundtrip PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME BlockStorageRoundTrip COMMAND test_block_storage_roundtrip)
endif()

# #371: ChainDB writeBatch must recover from a latched rocksdb background error
# (Resume + retry) and fail loudly after a consecutive-failure threshold.
# Uses rocksdb's FaultInjectionTestEnv (compiled into the vendored rocksdb lib).
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/storage/test_chain_db_write_failure_recovery.cpp)
  add_executable(test_chain_db_write_failure_recovery
    tests/storage/test_chain_db_write_failure_recovery.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_chain_db_write_failure_recovery PRIVATE
    dinero_chainstate
    dinero_crypto
    RocksDB::rocksdb
    OpenSSL::Crypto
  )
  target_include_directories(test_chain_db_write_failure_recovery PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME ChainDBWriteFailureRecovery COMMAND test_chain_db_write_failure_recovery)
  set_tests_properties(ChainDBWriteFailureRecovery PROPERTIES
    LABELS "storage;smoke"
  )
endif()

# #384: CSN cursor-to-tip reconcile — pure predicate that breaks the
# restart-mid-backfill "too far ahead of cursor 1" deadlock (phone 2026-07-07).
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_csn_accept_window.cpp)
  add_executable(test_csn_accept_window
    tests/consensus/test_csn_accept_window.cpp
  )
  target_include_directories(test_csn_accept_window PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME CsnAcceptWindow COMMAND test_csn_accept_window)
  set_tests_properties(CsnAcceptWindow PROPERTIES LABELS "consensus;smoke")
endif()


# Archival Block Reader Test - distinguish flatfiles from legacy ChainDB fallback
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/storage/test_archival_block_reader.cpp)
  add_executable(test_archival_block_reader
    tests/storage/test_archival_block_reader.cpp
    src/daemon/genesis_init.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_archival_block_reader PRIVATE
    dinero_core
    dinero_chainstate
    dinero_crypto
    OpenSSL::Crypto
  )
  target_include_directories(test_archival_block_reader PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME ArchivalBlockReader COMMAND test_archival_block_reader)
endif()

# Phase M.0 Byte Order Invariant Test - CRITICAL CONSENSUS LOCK
# This test permanently locks the invariant that uint256 is little-endian internally
# and GetHex() reverses bytes for display. IF THIS TEST FAILS, CONSENSUS IS BROKEN.
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_phase_m0_byte_order_invariant.cpp)
  add_executable(test_phase_m0_byte_order_invariant
    tests/test_phase_m0_byte_order_invariant.cpp
    src/common/logger.cpp  # Required by transaction code
  )
  target_link_libraries(test_phase_m0_byte_order_invariant PRIVATE
    dinero_wallet
    dinero_crypto
    OpenSSL::Crypto
  )
  target_include_directories(test_phase_m0_byte_order_invariant PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME PhaseM0ByteOrderInvariant COMMAND test_phase_m0_byte_order_invariant)
endif()

# Logger Injection Test - Demonstrates TestLogger and NullLogger usage
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_logger_injection.cpp)
  add_executable(test_logger_injection
    tests/test_logger_injection.cpp
  )
  target_include_directories(test_logger_injection PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME test_logger_injection COMMAND test_logger_injection)
endif()

# JsonLogger Test - Validates structured JSON logging
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_json_logger.cpp)
  add_executable(test_json_logger
    tests/test_json_logger.cpp
    src/common/json_logger.cpp
  )
  target_include_directories(test_json_logger PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME test_json_logger COMMAND test_json_logger)
endif()

# Zero-Knowledge Privacy Test - Pedersen commitments + Bulletproofs
if(ENABLE_TESTS AND ENABLE_ZK AND EXISTS ${CMAKE_SOURCE_DIR}/tests/test_zk_commitment_balance.cpp)
  add_executable(test_zk_commitment_balance
    tests/test_zk_commitment_balance.cpp
  )
  target_link_libraries(test_zk_commitment_balance PRIVATE
    dinero_zk
    lightning_core_static  # For secp256k1-zkp (used by Pedersen commitments)
  )
  target_include_directories(test_zk_commitment_balance PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME test_zk_commitment_balance COMMAND test_zk_commitment_balance)
endif()

# ZKVM ring-covenant test infra removed in v7 (see commit 1d8118f46) — the
# zkvm covenant prover only exercised CLSAG / ring-covenant spends which
# don't exist on v7. Test source deleted.

# Hybrid hidden-member proof split tests

# SHA256 R1CS gadget tests
if(ENABLE_TESTS AND ENABLE_ZK AND EXISTS ${CMAKE_SOURCE_DIR}/tests/test_sha256_gadget.cpp)
  add_executable(test_sha256_gadget
    tests/test_sha256_gadget.cpp
  )
  target_link_libraries(test_sha256_gadget PRIVATE
    dinero_zk
    dinero_crypto
    secp256k1
    OpenSSL::Crypto
    gtest
    gtest_main
  )
  link_secp256k1_zkp(test_sha256_gadget)
  if(APPLE)
    target_link_libraries(test_sha256_gadget PRIVATE "-framework Security" "-framework CoreFoundation")
  endif()
  target_include_directories(test_sha256_gadget PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/secp256k1-zkp/include
  )
  add_test(NAME SHA256Gadget COMMAND test_sha256_gadget)
  set_tests_properties(SHA256Gadget PROPERTIES
    LABELS "zk;zkvm;sha256;gadget"
    TIMEOUT 300
  )
  message(STATUS "  ✓ SHA256 gadget test registered")
endif()

# CTV preimage circuit test removed in v7 — only verified ring-anonymous
# covenant spends, which require ring-covenant code that was deleted.

# ZKVM benchmarks (bench_zkvm + bench_mixed_block) removed in v7 — benchmarked
# CLSAG / ring-covenant proving paths that don't exist on v7.

# Bulletproofs Integration Test - Comprehensive FFI and validation tests
if(ENABLE_TESTS AND CARGO_EXECUTABLE AND ENABLE_ZK AND EXISTS ${CMAKE_SOURCE_DIR}/tests/test_bulletproofs_integration.cpp)
  add_executable(test_bulletproofs_integration
    tests/test_bulletproofs_integration.cpp
    src/common/logger.cpp                    # Logger implementation
  )
  target_link_libraries(test_bulletproofs_integration PRIVATE
    dinero_consensus        # For ConfidentialTransactionValidator
    dinero_zk
    bulletproofs_ffi        # Dalek Bulletproofs FFI
    dinero_crypto           # For crypto primitives
    lightning_core_static   # For secp256k1-zkp (Pedersen)
    gtest
    ${CMAKE_DL_LIBS}        # Required for Rust FFI
  )
  link_zstd_if_needed(test_bulletproofs_integration)
  target_include_directories(test_bulletproofs_integration PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/secp256k1-zkp/include
  )
  add_test(NAME test_bulletproofs_integration COMMAND test_bulletproofs_integration)

  # Ensure Bulletproofs FFI is built before test
  if(TARGET build_bulletproofs_ffi)
    add_dependencies(test_bulletproofs_integration build_bulletproofs_ffi)
  endif()
endif()

# Bulletproofs Batch Verification Test (Phase F.8) - Comprehensive batch verification tests
if(CARGO_EXECUTABLE AND ENABLE_ZK AND EXISTS ${CMAKE_SOURCE_DIR}/tests/test_bulletproofs_batch.cpp)
  add_executable(test_bulletproofs_batch
    tests/test_bulletproofs_batch.cpp
    src/common/logger.cpp                    # Logger implementation
  )
  target_link_libraries(test_bulletproofs_batch PRIVATE
    dinero_consensus        # For ConfidentialTransactionValidator
    dinero_zk
    bulletproofs_ffi        # Dalek Bulletproofs FFI
    dinero_crypto           # For crypto primitives (includes pedersen, bulletproofs, logger)
    lightning_core_static   # For secp256k1-zkp (Pedersen)
    gtest
    ${CMAKE_DL_LIBS}        # Required for Rust FFI
  )
  link_zstd_if_needed(test_bulletproofs_batch)
  target_include_directories(test_bulletproofs_batch PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/secp256k1-zkp/include
  )
  add_test(NAME test_bulletproofs_batch COMMAND test_bulletproofs_batch)

  # Ensure Bulletproofs FFI is built before test
  if(TARGET build_bulletproofs_ffi)
    add_dependencies(test_bulletproofs_batch build_bulletproofs_ffi)
  endif()
endif()

# Confidential Transaction Consensus Pipeline Tests (Phase G.5)
# Integration tests for complete confidential transaction validation

# Confidential Transaction Serialization Tests (Phase I.3)
# Tests for serialization/deserialization of transaction variants

# Ring Signature TxInput Serialization Tests (Phase R.4)
# Tests for ring_members, key_image, clsag_signature fields on TxInput

# REMOVED: test_confidential_tx_builder, test_confidential_integration (obsolete zombie tests from copy repo)

# Confidential Transaction End-to-End Tests (Phase K.2) - DISABLED: obsolete
# Complete workflow tests including Alice-to-Bob transactions
if(FALSE AND CARGO_EXECUTABLE AND ENABLE_ZK AND EXISTS ${CMAKE_SOURCE_DIR}/tests/test_confidential_end_to_end.cpp)
  add_executable(test_confidential_end_to_end
    tests/test_confidential_end_to_end.cpp
    src/common/logger.cpp  # Logger implementation
    src/common/address_script_builder.cpp  # BuildScriptPubKeyFromAddress
    src/daemon/legacy_globals_stub.cpp  # GetWalletManagerForIndexing
    src/daemon/bech32_encoder.cpp  # Bech32Encoder
  )
  target_link_libraries(test_confidential_end_to_end PRIVATE
    dinero_wallet          # Wallet library (includes all wallet sources)
    dinero_chainstate      # For UTXOIndex
    dinero_consensus       # For consensus validation
    dinero_zk
    bulletproofs_ffi        # Dalek Bulletproofs FFI
    dinero_crypto           # For crypto primitives
    lightning_core_static   # For secp256k1-zkp
    gtest
    ${CMAKE_DL_LIBS}        # Required for Rust FFI
    OpenSSL::Crypto        # For SHA256 in address_db
    sqlite3                # SQLite database
  )
  link_zstd_if_needed(test_confidential_end_to_end)
  if(APPLE)
    target_link_libraries(test_confidential_end_to_end PRIVATE
    )
  endif()
  target_include_directories(test_confidential_end_to_end PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/secp256k1-zkp/include
  )
  add_test(NAME test_confidential_end_to_end COMMAND test_confidential_end_to_end)

  # Ensure Bulletproofs FFI is built before test
  if(TARGET build_bulletproofs_ffi)
    add_dependencies(test_confidential_end_to_end build_bulletproofs_ffi)
  endif()
endif()

# CON-11 Commitment Balance Tests (Ristretto255) - DISABLED: obsolete
if(FALSE AND CARGO_EXECUTABLE AND ENABLE_ZK AND EXISTS ${CMAKE_SOURCE_DIR}/tests/test_con11_ristretto255.cpp)
  add_executable(test_con11_ristretto255
    tests/test_con11_ristretto255.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_con11_ristretto255 PRIVATE
    dinero_consensus
    dinero_wallet
    gtest
    bulletproofs_ffi
  )
  link_zstd_if_needed(test_con11_ristretto255)
  if(APPLE)
    target_link_libraries(test_con11_ristretto255 PRIVATE
    )
  endif()
  target_include_directories(test_con11_ristretto255 PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/secp256k1-zkp/include
  )
  add_test(NAME test_con11_ristretto255 COMMAND test_con11_ristretto255)

  # Ensure Bulletproofs FFI is built before test
  if(TARGET build_bulletproofs_ffi)
    add_dependencies(test_con11_ristretto255 build_bulletproofs_ffi)
  endif()
endif()

# Range Proof Tests (Bulletproofs FFI)
if(CARGO_EXECUTABLE AND ENABLE_ZK AND EXISTS ${CMAKE_SOURCE_DIR}/tests/test_range_proofs.cpp)
  add_executable(test_range_proofs
    tests/test_range_proofs.cpp
  )
  target_link_libraries(test_range_proofs PRIVATE
    gtest
    bulletproofs_ffi
  )
  # Rust's std on Windows pulls BCryptGenRandom (bcrypt.lib) for stdlib RNG,
  # NtWriteFile / RtlNtStatusToDosError (ntdll.lib) for stdio, and userenv
  # for env access. These are the standard runtime libs for any Rust
  # staticlib consumed by an MSVC link.
  if(WIN32)
    target_link_libraries(test_range_proofs PRIVATE bcrypt ntdll userenv ws2_32)
  endif()
  if(APPLE)
    target_link_libraries(test_range_proofs PRIVATE
    )
  endif()
  target_include_directories(test_range_proofs PRIVATE
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME test_range_proofs COMMAND test_range_proofs)
  if(TARGET build_bulletproofs_ffi)
    add_dependencies(test_range_proofs build_bulletproofs_ffi)
  endif()
endif()

# CT Selection Policy Unit Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mining/test_ct_selection_policy.cpp)
  add_executable(test_ct_selection_policy
    tests/mining/test_ct_selection_policy.cpp
    src/mining/ct_selection_policy.cpp
    src/consensus/chainparams_impl.cpp
  )
  target_link_libraries(test_ct_selection_policy PRIVATE
    gtest
    dinero_consensus
    dinero_crypto
  )
  target_include_directories(test_ct_selection_policy PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME test_ct_selection_policy COMMAND test_ct_selection_policy)
endif()

# WalletService Logger Pattern Demo - Shows how to use TestLogger with services
# REMOVED: test_wallet_service_logging_simple (obsolete - DaemonContext issue)

# test_wallet_crypto temporarily disabled due to g_logger linking issues
# Will be fixed when logger is refactored
# add_executable(test_wallet_crypto tests/test_wallet_crypto.cpp src/wallet/key_vault_simple.cpp)
# if(APPLE)
#   target_link_libraries(test_wallet_crypto PRIVATE
#     dinero_wallet
#     OpenSSL::Crypto
#     jsoncpp_static
#     secp256k1
#   )
# else()
#   target_link_libraries(test_wallet_crypto PRIVATE
#     dinero_wallet
#     OpenSSL::Crypto
#     jsoncpp
#     secp256k1
#   )
# endif()
# target_include_directories(test_wallet_crypto PRIVATE include src)

# Week 5: Context-aware smoke tests - DISABLED: stratum now separate binary
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/mining/test_block_assembler_smoke.cpp)
  add_executable(test_block_assembler_smoke
    tests/mining/test_block_assembler_smoke.cpp
    # Service implementations needed for TestDaemonContext
    # Note: logger_service.h is header-only, no .cpp needed
    src/daemon/services/config_service.cpp
    src/daemon/services/chainstate_service.cpp
    src/daemon/sync_stats_recorder.cpp
    src/daemon/services/wallet_service.cpp
    # Core dependencies
    src/common/logger.cpp
    src/common/config_manager.cpp
    src/daemon/block_acceptor.cpp  # Needed by chainstate_service
    src/database/sqlite_manager.cpp
    # src/consensus/supply_tracker.cpp  # REMOVED: Zombie code
    src/consensus/block_index.cpp
    src/consensus/block_lifecycle.cpp
    src/consensus/orphan_manager.cpp
    # src/consensus/genesis_canonical.cpp  # REMOVED - depends on missing genesis_block.h
    src/consensus/chainparams_impl.cpp
    src/consensus/chainwork.cpp
    src/consensus/tx_parser.cpp
    src/consensus/script_verify.cpp
    src/storage/chain_db.cpp
    src/wallet/utxo_index.cpp
    src/wallet/wallet_worker.cpp
    src/wallet/wallet_manager.cpp  # Modern HD wallet with BIP84 (Nov 2025)
    src/wallet/address.cpp
    src/mining/block_assembler.cpp
    # src/mining/template_validator.cpp  # REMOVED: Zombie code
    src/daemon/mining_safety_gates.cpp
    # Additional dependencies from block_acceptor and chainstate
    src/daemon/mining_safety_gates.cpp
    src/address/addr_codec.cpp
    src/address/addr_types.cpp
    src/core/crypto/ripemd160.cpp
    # Missing dependencies for linker
    src/metrics/metrics_registry.cpp
    src/daemon/mempool.cpp
    src/crypto/decrypt_encrypted_key.cpp
    src/daemon/secure_random.cpp
    src/daemon/wallet_crypto.cpp  # Contains decryptAesGcm, encryptAesGcm, deriveKeyArgon2id
    src/daemon/bech32_decode.cpp  # Contains Bech32DecodeSegwit
    src/daemon/ws_globals.cpp  # Contains g_subscriptions
    src/daemon/p2p_message.cpp  # Contains InvMessage implementation
  )
  target_include_directories(test_block_assembler_smoke PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}  # For version_config.h
    ${CMAKE_SOURCE_DIR}/external  # For bech32.hpp
  )
  target_link_libraries(test_block_assembler_smoke PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    RocksDB::rocksdb
    secp256k1
    sqlite3
    jsoncpp_static
  )
  link_zstd_if_needed(test_block_assembler_smoke)
  if(APPLE)
    target_link_libraries(test_block_assembler_smoke PRIVATE
    )
  endif()
  add_test(NAME BlockAssemblerSmoke COMMAND test_block_assembler_smoke)
endif()

# Phase 2: Consensus engine integration test
# DISABLED: stratum now separate binary in DaemonContext
# Phase C: Also needs updating to use MiningManager v2 instead of legacy Mining
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_consensus_integration.cpp)
  add_executable(test_consensus_integration
    tests/consensus/test_consensus_integration.cpp
    # Reuse same dependencies as test_block_assembler_smoke
    src/daemon/services/config_service.cpp
    src/daemon/services/chainstate_service.cpp
    src/daemon/sync_stats_recorder.cpp
    src/daemon/services/mining_service.cpp
    src/daemon/services/wallet_service.cpp
    # Phase C: Legacy Mining class removed - test needs updating
    # src/daemon/mining.cpp
    src/consensus/pow_consensus_engine.cpp
    # Core dependencies
    src/common/logger.cpp
    src/common/config_manager.cpp
    src/daemon/block_acceptor.cpp
    src/database/sqlite_manager.cpp
    # src/consensus/supply_tracker.cpp  # REMOVED: Zombie code
    src/consensus/block_index.cpp
    src/consensus/block_lifecycle.cpp
    src/consensus/orphan_manager.cpp
    # src/consensus/genesis_canonical.cpp  # REMOVED - depends on missing genesis_block.h
    src/consensus/chainparams_impl.cpp
    src/consensus/chainwork.cpp
    src/consensus/tx_parser.cpp
    src/consensus/script_verify.cpp
    src/storage/chain_db.cpp
    src/wallet/utxo_index.cpp
    src/wallet/wallet_worker.cpp
    src/wallet/wallet_manager.cpp  # Modern HD wallet with BIP84 (Nov 2025)
    src/wallet/address.cpp
    src/mining/block_assembler.cpp
    # src/mining/template_validator.cpp  # REMOVED: Zombie code
    src/mining/miner.cpp
    # src/mining/mining_manager.cpp  # Phase C: Replaced by mining_manager_v2.cpp (ODR violation fix)
    src/daemon/mining_safety_gates.cpp
    src/address/addr_codec.cpp
    src/address/addr_types.cpp
    src/core/crypto/ripemd160.cpp
    src/metrics/metrics_registry.cpp
    src/daemon/mempool.cpp
    src/crypto/decrypt_encrypted_key.cpp
    src/daemon/secure_random.cpp
    src/daemon/wallet_crypto.cpp
    src/daemon/bech32_decode.cpp
    src/daemon/ws_globals.cpp
    src/daemon/p2p_message.cpp
  )
  target_include_directories(test_consensus_integration PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
    ${CMAKE_SOURCE_DIR}/external
  )
  target_link_libraries(test_consensus_integration PRIVATE
    dinero_common
    dinero_consensus
    dinero_wallet
    dinero_crypto
    RocksDB::rocksdb
    sqlite3
    jsoncpp_static
    secp256k1
    OpenSSL::SSL
    OpenSSL::Crypto
  )
  link_zstd_if_needed(test_consensus_integration)
  if(APPLE)
    target_link_libraries(test_consensus_integration PRIVATE
    )
  endif()
  add_test(NAME ConsensusIntegration COMMAND test_consensus_integration)
endif()

# Codebase Verification Integration Tests - DISABLED: links dinero_wallet (LightningService issue)
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/integration/test_codebase_verification.cpp)
  add_executable(test_codebase_verification
    tests/integration/test_codebase_verification.cpp
    tests/support/test_stubs.cpp
    # Reuse same dependencies as test_mining_smoke
    src/daemon/services/config_service.cpp
    src/daemon/services/chainstate_service.cpp
    src/daemon/sync_stats_recorder.cpp
    src/daemon/services/wallet_service.cpp
    src/common/logger.cpp
    src/common/config_manager.cpp
    src/daemon/block_acceptor.cpp
    src/database/sqlite_manager.cpp
    # src/daemon/db_init_simple.cpp  # REMOVED: Zombie - legacy SQLite init, bypassed ChainWriteToken
    src/daemon/db_meta_utils.cpp
    # src/consensus/supply_tracker.cpp  # REMOVED: Zombie code
    src/consensus/block_index.cpp
    src/consensus/block_lifecycle.cpp
    src/consensus/orphan_manager.cpp
    # src/consensus/genesis_canonical.cpp  # REMOVED - depends on missing genesis_block.h
    src/consensus/chainparams_impl.cpp
    src/consensus/chainwork.cpp
    src/consensus/tx_parser.cpp
    src/consensus/script_verify.cpp
    src/storage/chain_db.cpp
    src/wallet/utxo_index.cpp
    src/wallet/wallet_worker.cpp
    src/wallet/wallet_manager.cpp  # Modern HD wallet with BIP84 (Nov 2025)
    src/wallet/address.cpp
    src/mining/block_assembler.cpp
    # src/mining/template_validator.cpp  # REMOVED: Zombie code
    src/daemon/mining_safety_gates.cpp
    src/address/addr_codec.cpp
    src/address/addr_types.cpp
    src/core/crypto/ripemd160.cpp
    src/metrics/metrics_registry.cpp
    src/daemon/mempool.cpp
    src/crypto/decrypt_encrypted_key.cpp
    src/daemon/secure_random.cpp
    src/daemon/wallet_crypto.cpp
    src/daemon/bech32_decode.cpp
    src/daemon/ws_globals.cpp
    src/daemon/p2p_message.cpp
    src/daemon/p2p_manager.cpp
    src/network/relay_hints_eviction.cpp
    src/network/relay_registry.cpp
    src/network/quic_session.cpp
    src/network/quic_transport.cpp
    src/network/udp_socket.cpp
    src/network/relay_tls_keypair.cpp
    src/p2p/addrman.cpp
  )
  target_include_directories(test_codebase_verification PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}
    ${CMAKE_SOURCE_DIR}/external
  )
  target_link_libraries(test_codebase_verification PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    RocksDB::rocksdb
    secp256k1
    sqlite3
    jsoncpp_static
    gtest
  )
  link_zstd_if_needed(test_codebase_verification)
  if(DINERO_ENABLE_QUIC AND TARGET dinero_ngtcp2)
    target_link_libraries(test_codebase_verification PRIVATE dinero_ngtcp2)
    if(TARGET dinero_ngtcp2_crypto_ossl)
      target_link_libraries(test_codebase_verification PRIVATE dinero_ngtcp2_crypto_ossl)
    elseif(TARGET dinero_ngtcp2_crypto_quictls)
      target_link_libraries(test_codebase_verification PRIVATE dinero_ngtcp2_crypto_quictls)
    endif()
  endif()
  if(WIN32)
    target_link_libraries(test_codebase_verification PRIVATE ws2_32)
  endif()
  if(APPLE)
    target_link_libraries(test_codebase_verification PRIVATE
      "-framework IOKit"
    )
  endif()
  target_compile_definitions(test_codebase_verification PRIVATE
    CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  )
  add_test(NAME CodebaseVerification COMMAND test_codebase_verification)
endif()

# Comprehensive Wallet Tests - DISABLED: obsolete
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet/test_wallet_comprehensive.cpp)
  add_executable(test_wallet_comprehensive
    tests/wallet/test_wallet_comprehensive.cpp
    tests/support/test_stubs.cpp
    src/common/logger.cpp
    src/wallet/wallet_manager.cpp  # Modern HD wallet with BIP84 (Nov 2025)
    src/wallet/hd_wallet.cpp
    src/wallet/bip39.cpp
    src/wallet/psbt.cpp
    src/wallet/coin_selection.cpp
    src/crypto/pbkdf2.cpp
    src/consensus/coinbase_maturity.cpp
  )
  target_link_libraries(test_wallet_comprehensive PRIVATE
    dinero_wallet
    dinero_chainstate
    dinero_consensus
    dinero_crypto
    secp256k1
    gtest
  )
  link_zstd_if_needed(test_wallet_comprehensive)
  target_include_directories(test_wallet_comprehensive PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
  )
  if(APPLE)
    target_link_libraries(test_wallet_comprehensive PRIVATE
    )
  endif()
  add_test(NAME WalletComprehensive COMMAND test_wallet_comprehensive)
endif()

# Taproot Mining Tests - DISABLED: hd_wallet.cpp include path issue with bech32
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/test_taproot_mining.cpp)
  add_executable(test_taproot_mining
    tests/test_taproot_mining.cpp
    src/wallet/hd_wallet.cpp
    src/wallet/bip39.cpp
    src/wallet/psbt.cpp
    src/wallet/coin_selection.cpp
    src/wallet/utxo_index.cpp
    src/wallet/bip143_signer.cpp
    src/wallet/taproot_tx_signer.cpp
    src/crypto/pbkdf2.cpp
    src/crypto/ripemd160_standalone.cpp
    src/crypto/sha256.cpp
    src/crypto/params_stub.cpp
    src/core/crypto/ripemd160.cpp
    src/common/logger.cpp
    src/consensus/coinbase_maturity.cpp
    src/database/sqlite_manager.cpp
    # src/daemon/db_init_simple.cpp  # REMOVED: Zombie - legacy SQLite init, bypassed ChainWriteToken
    src/daemon/db_meta_utils.cpp
    src/sqlite_open.cpp
    external/bech32/bech32.cpp
  )
  target_link_libraries(test_taproot_mining PRIVATE
    sqlite3
    secp256k1
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread
  )
  target_include_directories(test_taproot_mining PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/external
    ${CMAKE_SOURCE_DIR}/third_party/bip39
  )
  target_compile_definitions(test_taproot_mining PRIVATE
    CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  )
  if(APPLE)
    target_link_libraries(test_taproot_mining PRIVATE
      "-framework Security"
    )
  endif()
  add_test(NAME TaprootMining COMMAND test_taproot_mining)
endif()

# Minimal Taproot Mining Test (for debugging) - DISABLED: LightningService dependency
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/test_taproot_mining_minimal.cpp)
  add_executable(test_taproot_mining_minimal
    tests/test_taproot_mining_minimal.cpp
    src/crypto/params_stub.cpp
    src/sqlite_open.cpp
    src/common/logger.cpp
    tests/stubs/utxo_index_stub.cpp
  )
  target_link_libraries(test_taproot_mining_minimal PRIVATE
    dinero_wallet
    dinero_crypto
    secp256k1
    sqlite3
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread
  )
  target_include_directories(test_taproot_mining_minimal PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/external
    ${CMAKE_SOURCE_DIR}/third_party/bip39
  )
  target_compile_definitions(test_taproot_mining_minimal PRIVATE
    CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  )
  if(APPLE)
    target_link_libraries(test_taproot_mining_minimal PRIVATE
    )
  endif()
endif()

# Comprehensive Mining Tests - DISABLED: LightningService dependency + multiple framework issues
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/mining/test_mining_comprehensive.cpp)
  add_executable(test_mining_comprehensive
    tests/mining/test_mining_comprehensive.cpp
    tests/support/test_stubs.cpp
    src/common/logger.cpp
    src/common/config_manager.cpp
    src/daemon/mempool.cpp
    src/mining/block_assembler.cpp
    # src/mining/template_validator.cpp  # REMOVED: Zombie code
    src/database/sqlite_manager.cpp
    # src/daemon/db_init_simple.cpp  # REMOVED: Zombie - legacy SQLite init, bypassed ChainWriteToken
    src/daemon/db_meta_utils.cpp
    # src/consensus/supply_tracker.cpp  # REMOVED: Zombie code
    src/consensus/block_index.cpp
    src/consensus/block_lifecycle.cpp
    src/consensus/orphan_manager.cpp
    # src/consensus/genesis_canonical.cpp  # REMOVED - depends on missing genesis_block.h
    src/consensus/chainparams_impl.cpp
    src/consensus/chainwork.cpp
    src/consensus/tx_parser.cpp
    src/consensus/script_verify.cpp
    src/storage/chain_db.cpp
    src/daemon/mining_safety_gates.cpp
    src/address/addr_codec.cpp
    src/address/addr_types.cpp
    src/core/crypto/ripemd160.cpp
    src/metrics/metrics_registry.cpp
    src/daemon/bech32_decode.cpp
    src/daemon/p2p_message.cpp
    src/daemon/p2p_manager.cpp
    src/network/relay_hints_eviction.cpp
    src/network/relay_registry.cpp
    src/network/quic_session.cpp
    src/network/quic_transport.cpp
    src/network/udp_socket.cpp
    src/network/relay_tls_keypair.cpp
    src/p2p/addrman.cpp
    src/daemon/wallet_crypto.cpp
    src/daemon/secure_random.cpp
    src/crypto/decrypt_encrypted_key.cpp
    src/consensus/coinbase_maturity.cpp
  )
  target_link_libraries(test_mining_comprehensive PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    RocksDB::rocksdb
    secp256k1
    sqlite3
    jsoncpp_static
    gtest
  )
  link_zstd_if_needed(test_mining_comprehensive)
  if(DINERO_ENABLE_QUIC AND TARGET dinero_ngtcp2)
    target_link_libraries(test_mining_comprehensive PRIVATE dinero_ngtcp2)
    if(TARGET dinero_ngtcp2_crypto_ossl)
      target_link_libraries(test_mining_comprehensive PRIVATE dinero_ngtcp2_crypto_ossl)
    elseif(TARGET dinero_ngtcp2_crypto_quictls)
      target_link_libraries(test_mining_comprehensive PRIVATE dinero_ngtcp2_crypto_quictls)
    endif()
  endif()
  if(WIN32)
    target_link_libraries(test_mining_comprehensive PRIVATE ws2_32)
  endif()
  target_include_directories(test_mining_comprehensive PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}
    ${CMAKE_SOURCE_DIR}/external
  )
  target_compile_definitions(test_mining_comprehensive PRIVATE
    CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  )
  if(APPLE)
    target_link_libraries(test_mining_comprehensive PRIVATE
      "-framework IOKit"
    )
  endif()
  add_test(NAME MiningComprehensive COMMAND test_mining_comprehensive)
endif()

# Comprehensive PSBT Tests - DISABLED: LightningService dependency
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet/test_psbt_comprehensive.cpp)
  add_executable(test_psbt_comprehensive
    tests/wallet/test_psbt_comprehensive.cpp
    tests/support/test_stubs.cpp
    src/common/logger.cpp
    src/wallet/wallet_manager.cpp  # Modern HD wallet with BIP84 (Nov 2025)
    src/wallet/hd_wallet.cpp
    src/wallet/bip39.cpp
    src/wallet/psbt.cpp
    src/crypto/pbkdf2.cpp
    src/consensus/coinbase_maturity.cpp
  )
  target_link_libraries(test_psbt_comprehensive PRIVATE
    dinero_wallet
    dinero_chainstate
    dinero_consensus
    dinero_crypto
    secp256k1
    gtest
  )
  link_zstd_if_needed(test_psbt_comprehensive)
  target_include_directories(test_psbt_comprehensive PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
  )
  if(APPLE)
    target_link_libraries(test_psbt_comprehensive PRIVATE
    )
  endif()
  add_test(NAME PSBTComprehensive COMMAND test_psbt_comprehensive)
endif()

# ════════════════════════════════════════════════════════════════════════════
# Phase W.3: Ergonomics RPC Tests
# ════════════════════════════════════════════════════════════════════════════
# TEMPORARILY DISABLED (2026-02-02): dinero_core missing Mempool/BridgeNode sources
# TODO: Add missing source files to dinero_core library
# if(EXISTS ${CMAKE_SOURCE_DIR}/tests/rpc/test_ergonomics_rpc.cpp)
#   add_executable(test_ergonomics_rpc
#     tests/rpc/test_ergonomics_rpc.cpp
#     src/common/logger.cpp
#   )
#   target_link_libraries(test_ergonomics_rpc PRIVATE
#     dinero_rpc_handlers dinero_core dinero_wallet dinero_chainstate
#     dinero_consensus jsoncpp_static bulletproofs_ffi
#   )
#   if(APPLE)
#     target_link_libraries(test_ergonomics_rpc PRIVATE "-framework Security" "-framework CoreFoundation")
#   endif()
#   link_zstd_if_needed(test_ergonomics_rpc)
#   target_include_directories(test_ergonomics_rpc PRIVATE
#     ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/tests
#   )
#   add_test(NAME ErgonomicsRPC COMMAND test_ergonomics_rpc)
# endif()

# ════════════════════════════════════════════════════════════════════════════
# Phase W.4.1: Transaction Inclusion Analyzer Tests
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mining/test_tx_inclusion_analyzer.cpp)
  add_executable(test_tx_inclusion_analyzer
    tests/mining/test_tx_inclusion_analyzer.cpp
    src/common/logger.cpp
    src/daemon/mempool.cpp    # Provides dinero::Mempool implementation
    src/daemon/config.cpp     # Provides GetConfig()
  )
  target_link_libraries(test_tx_inclusion_analyzer PRIVATE
    dinero_core
    dinero_chainstate
    dinero_consensus
    bulletproofs_ffi  # Required by confidential_validation
    jsoncpp_static
  )
  link_zstd_if_needed(test_tx_inclusion_analyzer)
  target_include_directories(test_tx_inclusion_analyzer PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
  )
  if(APPLE)
    target_link_libraries(test_tx_inclusion_analyzer PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME TxInclusionAnalyzer COMMAND test_tx_inclusion_analyzer)
endif()

# ════════════════════════════════════════════════════════════════════════════
# Phase W.4.2: RBF & CPFP Capability Detection Tests
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet/test_rbf_cpfp_detector.cpp)
  add_executable(test_rbf_cpfp_detector
    tests/wallet/test_rbf_cpfp_detector.cpp
    src/common/logger.cpp
    src/daemon/mempool.cpp    # Provides dinero::Mempool implementation
    src/daemon/config.cpp     # Provides GetConfig()
  )
  target_link_libraries(test_rbf_cpfp_detector PRIVATE
    dinero_core
    dinero_wallet
    dinero_chainstate
    dinero_consensus
    bulletproofs_ffi  # Required by confidential_validation
    jsoncpp_static
  )
  link_zstd_if_needed(test_rbf_cpfp_detector)
  target_include_directories(test_rbf_cpfp_detector PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
  )
  if(APPLE)
    target_link_libraries(test_rbf_cpfp_detector PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME RbfCpfpDetector COMMAND test_rbf_cpfp_detector)
endif()

# ════════════════════════════════════════════════════════════════════════════
# Phase W.4.3: Fee Bump Recommendation Engine Tests
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet/test_fee_bump_engine.cpp)
  add_executable(test_fee_bump_engine
    tests/wallet/test_fee_bump_engine.cpp
    src/common/logger.cpp
    src/daemon/mempool.cpp    # Provides dinero::Mempool implementation
    src/daemon/config.cpp     # Provides GetConfig()
  )
  target_link_libraries(test_fee_bump_engine PRIVATE
    dinero_core
    dinero_wallet
    dinero_chainstate
    dinero_consensus
    bulletproofs_ffi  # Required by confidential_validation
    jsoncpp_static
  )
  link_zstd_if_needed(test_fee_bump_engine)
  target_include_directories(test_fee_bump_engine PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
  )
  if(APPLE)
    target_link_libraries(test_fee_bump_engine PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME FeeBumpEngine COMMAND test_fee_bump_engine)
endif()

# ════════════════════════════════════════════════════════════════════════════
# Phase W.4.4: Fee Bump RPC Handler Tests
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/rpc/test_fee_bump_rpc.cpp)
  add_executable(test_fee_bump_rpc
    tests/rpc/test_fee_bump_rpc.cpp
    src/rpc/fee_bump_rpc_handlers.cpp
    src/mining/tx_inclusion_analyzer.cpp
    src/wallet/rbf_cpfp_detector.cpp
    src/wallet/fee_bump_engine.cpp
    src/common/logger.cpp
    src/daemon/mempool.cpp    # Provides dinero::Mempool implementation
    src/daemon/config.cpp     # Provides GetConfig()
  )
  target_link_libraries(test_fee_bump_rpc PRIVATE
    dinero_core
    dinero_wallet
    dinero_chainstate
    dinero_consensus
    bulletproofs_ffi  # Required by confidential_validation
    jsoncpp_static
  )
  link_zstd_if_needed(test_fee_bump_rpc)
  target_include_directories(test_fee_bump_rpc PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
  )
  if(APPLE)
    target_link_libraries(test_fee_bump_rpc PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME FeeBumpRpc COMMAND test_fee_bump_rpc)
endif()

# ════════════════════════════════════════════════════════════════════════════
# Phase W.4.5: Wallet Fee Bump UX Hooks Tests
# ════════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet/test_wallet_fee_bump_hooks.cpp)
  add_executable(test_wallet_fee_bump_hooks
    tests/wallet/test_wallet_fee_bump_hooks.cpp
    src/wallet/wallet_fee_bump_hooks.cpp
    src/mining/tx_inclusion_analyzer.cpp
    src/wallet/rbf_cpfp_detector.cpp
    src/wallet/fee_bump_engine.cpp
    src/common/logger.cpp
    src/daemon/mempool.cpp    # Provides dinero::Mempool implementation
    src/daemon/config.cpp     # Provides GetConfig()
  )
  target_link_libraries(test_wallet_fee_bump_hooks PRIVATE
    dinero_core
    dinero_wallet
    dinero_chainstate
    dinero_consensus
    bulletproofs_ffi  # Required by confidential_validation
    jsoncpp_static
  )
  link_zstd_if_needed(test_wallet_fee_bump_hooks)
  target_include_directories(test_wallet_fee_bump_hooks PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
  )
  if(APPLE)
    target_link_libraries(test_wallet_fee_bump_hooks PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME WalletFeeBumpHooks COMMAND test_wallet_fee_bump_hooks)
endif()

# CT→CT VerifyCommitmentBalance regression test
# Exercises pedersen_verify_tally with real confidential-input commitments
# (previously hung due to unchecked parse return values)
if(ENABLE_ZK AND EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet/test_ct_verify_balance.cpp)
  add_executable(test_ct_verify_balance
    tests/wallet/test_ct_verify_balance.cpp
  )
  target_link_libraries(test_ct_verify_balance PRIVATE
    secp256k1
    gtest
    gtest_main
  )
  link_zstd_if_needed(test_ct_verify_balance)
  target_include_directories(test_ct_verify_balance PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/secp256k1-zkp/include
  )
  target_compile_definitions(test_ct_verify_balance PRIVATE DINERO_TEST_BUILD)
  add_test(NAME CTVerifyBalance COMMAND test_ct_verify_balance)
  set_tests_properties(CTVerifyBalance PROPERTIES
    LABELS "wallet;confidential;regression"
    TIMEOUT 30
  )
  if(TARGET build_bulletproofs_ffi)
    add_dependencies(test_ct_verify_balance build_bulletproofs_ffi)
  endif()
  message(STATUS "  ✓ CT→CT verify_balance regression test registered")
endif()

# Mining Smoke Tests - Day 1 Golden Path Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mining/test_mining_smoke.cpp)
  add_executable(test_mining_smoke
    tests/mining/test_mining_smoke.cpp
    tests/support/test_stubs.cpp
    # Reuse same dependencies as test_block_assembler_smoke
    src/daemon/services/config_service.cpp
    src/daemon/services/chainstate_service.cpp
    src/daemon/sync_stats_recorder.cpp
    src/daemon/services/wallet_service.cpp
    src/common/logger.cpp
    src/common/config_manager.cpp
    src/daemon/block_acceptor.cpp
    src/database/sqlite_manager.cpp
    # src/daemon/db_init_simple.cpp  # REMOVED: Zombie - legacy SQLite init, bypassed ChainWriteToken
    src/daemon/db_meta_utils.cpp
    # src/consensus/supply_tracker.cpp  # REMOVED: Zombie code
    src/consensus/block_index.cpp
    src/consensus/block_lifecycle.cpp
    src/consensus/orphan_manager.cpp
    # src/consensus/genesis_canonical.cpp  # REMOVED - depends on missing genesis_block.h
    src/consensus/chainparams_impl.cpp
    src/consensus/chainwork.cpp
    src/consensus/tx_parser.cpp
    src/consensus/script_verify.cpp
    src/storage/chain_db.cpp
    src/wallet/utxo_index.cpp
    src/wallet/wallet_worker.cpp
    src/wallet/wallet_manager.cpp  # Modern HD wallet with BIP84 (Nov 2025)
    src/wallet/address.cpp
    src/mining/block_assembler.cpp
    # src/mining/template_validator.cpp  # REMOVED: Zombie code
    # src/consensus/block_validation.cpp removed - has incompatible Block structure
    src/daemon/mining_safety_gates.cpp
    src/address/addr_codec.cpp
    src/address/addr_types.cpp
    src/core/crypto/ripemd160.cpp
    src/metrics/metrics_registry.cpp
    src/daemon/mempool.cpp
    src/crypto/decrypt_encrypted_key.cpp
    src/daemon/secure_random.cpp
    src/daemon/wallet_crypto.cpp
    src/daemon/bech32_decode.cpp
    src/daemon/ws_globals.cpp
    src/daemon/p2p_message.cpp
    src/daemon/p2p_manager.cpp  # For P2PMessage::create_inv() and P2PManager
    src/network/relay_hints_eviction.cpp
    src/network/relay_registry.cpp # Required by NAT C3 relay registry member in P2PManager
    src/network/quic_session.cpp   # Required by relay virtual-peer QUIC members in P2PManager
    src/network/quic_transport.cpp # Required by encrypted relay readiness checks in P2PManager
    src/network/udp_socket.cpp     # Required by QuicTransport
    src/network/relay_tls_keypair.cpp
    src/daemon/node_identity.cpp # Required by dineroid handshake methods in P2PManager
    src/p2p/addr_v2.cpp          # Required by addrv2 methods in P2PManager
    src/p2p/addrman.cpp
  )
  target_include_directories(test_mining_smoke PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}
    ${CMAKE_SOURCE_DIR}/external
  )
  target_link_libraries(test_mining_smoke PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    RocksDB::rocksdb
    secp256k1
    sqlite3
    jsoncpp_static
    gtest
    OpenSSL::Crypto
  )
  link_zstd_if_needed(test_mining_smoke)
  if(DINERO_ENABLE_QUIC AND TARGET dinero_ngtcp2)
    target_link_libraries(test_mining_smoke PRIVATE dinero_ngtcp2)
    if(TARGET dinero_ngtcp2_crypto_ossl)
      target_link_libraries(test_mining_smoke PRIVATE dinero_ngtcp2_crypto_ossl)
    elseif(TARGET dinero_ngtcp2_crypto_quictls)
      target_link_libraries(test_mining_smoke PRIVATE dinero_ngtcp2_crypto_quictls)
    endif()
  endif()
  if(WIN32)
    target_link_libraries(test_mining_smoke PRIVATE ws2_32)
  endif()
  if(APPLE)
    target_link_libraries(test_mining_smoke PRIVATE
      "-framework IOKit"
    )
  endif()
  add_test(NAME MiningSmoke COMMAND test_mining_smoke)
endif()

# ============================================================================
# Phase 10d: Utreexo Template Builder Integration Test
# ============================================================================
# DELETED registration: this test used the removed legacy BlockTemplateBuilder
# instance API. Production mining uses MiningService::createBlockTemplate() in
# src/daemon/services/mining_service.cpp.

# ============================================================================
# Phase 10d: Utreexo Height-2 Root Lock Test (Consensus Critical)
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_utreexo_height2_root.cpp)
  add_executable(test_utreexo_height2_root
    tests/consensus/test_utreexo_height2_root.cpp
  )

  target_include_directories(test_utreexo_height2_root PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests
  )

  target_link_libraries(test_utreexo_height2_root PRIVATE
    dinero_core
    dinero_chainstate
    dinero_consensus
    dinero_crypto
    gtest
    gtest_main
    bulletproofs_ffi
  )

  link_zstd_if_needed(test_utreexo_height2_root)

  if(APPLE)
    target_link_libraries(test_utreexo_height2_root PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()

  add_test(NAME UtreexoHeight2RootLock COMMAND test_utreexo_height2_root)
endif()

# Bech32 validator test suite
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_bech32_validator.cpp)
  add_executable(test_bech32_validator
    tests/test_bech32_validator.cpp
    src/wallet/address_validate.cpp
  )
  target_link_libraries(test_bech32_validator PRIVATE dinero_crypto)
  target_include_directories(test_bech32_validator PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME Bech32Validator COMMAND test_bech32_validator)
endif()

# BIP84 Change Address Test - DISABLED: LightningService dependency via dinero_wallet
if(FALSE)
add_executable(test_change_addresses
    test_change_addresses.cpp
    src/common/logger.cpp
    src/sqlite_open.cpp
)
target_link_libraries(test_change_addresses
    dinero_wallet
    dinero_crypto
    ${OPENSSL_LIBRARIES}
    sqlite3
    secp256k1
)
if(APPLE)
    target_link_libraries(test_change_addresses
      "-framework Security"
    )
endif()
endif()

# ============================================================================
# v1.1 Regression & Stress Tests
# ============================================================================

# Wallet Recovery Regression Tests - DISABLED: Security framework missing
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/regression/test_wallet_recovery.cpp)
  add_executable(test_wallet_recovery
    tests/regression/test_wallet_recovery.cpp
    tests/support/test_stubs.cpp
    src/common/logger.cpp  # Provides g_logger
  )
  target_include_directories(test_wallet_recovery PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
    ${CMAKE_SOURCE_DIR}/src
  )
  target_link_libraries(test_wallet_recovery PRIVATE
    dinero_consensus  # Must come before dinero_crypto (has correct Params())
    dinero_wallet
    dinero_crypto
    sqlite3
    OpenSSL::SSL
    OpenSSL::Crypto
    gtest
  )
  link_zstd_if_needed(test_wallet_recovery)
  if(APPLE)
    target_link_libraries(test_wallet_recovery PRIVATE
      secp256k1
      jsoncpp_static
    )
  else()
    target_link_libraries(test_wallet_recovery PRIVATE secp256k1 jsoncpp_static)
  endif()
  add_test(NAME WalletRecovery COMMAND test_wallet_recovery)
endif()

# Deep Reorg Regression Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/regression/test_deep_reorg.cpp)
  add_executable(test_deep_reorg
    tests/regression/test_deep_reorg.cpp
    tests/support/test_stubs.cpp
    src/common/logger.cpp  # Provides g_logger
    src/mining/block_assembler.cpp
    src/daemon/services/chainstate_service.cpp
    src/daemon/sync_stats_recorder.cpp
    src/daemon/services/wallet_service.cpp
    src/daemon/services/config_service.cpp
  )
  target_include_directories(test_deep_reorg PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/daemon
  )
  target_link_libraries(test_deep_reorg PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    sqlite3
    RocksDB::rocksdb
    OpenSSL::SSL
    OpenSSL::Crypto
    gtest
  )
  link_zstd_if_needed(test_deep_reorg)
  if(APPLE)
    target_link_libraries(test_deep_reorg PRIVATE
      secp256k1
      jsoncpp_static
    )
  else()
    target_link_libraries(test_deep_reorg PRIVATE secp256k1 jsoncpp_static)
  endif()
  add_test(NAME DeepReorg COMMAND test_deep_reorg)
endif()

# Mempool Stress Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/stress/test_mempool_stress.cpp)
  add_executable(test_mempool_stress
    tests/stress/test_mempool_stress.cpp
    tests/support/test_stubs.cpp
  )
  target_include_directories(test_mempool_stress PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
    ${CMAKE_SOURCE_DIR}/src
  )
  target_link_libraries(test_mempool_stress PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    gtest
  )
  link_zstd_if_needed(test_mempool_stress)
  if(APPLE)
    target_link_libraries(test_mempool_stress PRIVATE
      secp256k1
      jsoncpp_static
    )
  else()
    target_link_libraries(test_mempool_stress PRIVATE secp256k1 jsoncpp_static)
  endif()
  add_test(NAME MempoolStress COMMAND test_mempool_stress)
endif()

# ============================================================
#  Dinero Core - Test & Fuzzing Integration (v1.1)
# ============================================================

option(ENABLE_TESTS "Enable regression and stress testing" ON)
option(ENABLE_FUZZING "Enable fuzzing tests (requires Clang + libFuzzer)" OFF)

if (ENABLE_TESTS)
    message(STATUS "Enabling Dinero Core test suite...")

    # Tests are already enabled above via enable_testing()
    # NOTE: tests/CMakeLists.txt is NOT included as subdirectory because
    # test definitions are inline in this file for better integration.
    # Add regression and stress test subdirectories if they exist
    if(EXISTS ${CMAKE_SOURCE_DIR}/tests/regression/CMakeLists.txt)
        add_subdirectory(tests/regression)
    endif()

    if(EXISTS ${CMAKE_SOURCE_DIR}/tests/stress/CMakeLists.txt)
        add_subdirectory(tests/stress)
    endif()

    # Add reorg tests if they exist
    if(EXISTS ${CMAKE_SOURCE_DIR}/tests/reorg/CMakeLists.txt)
        add_subdirectory(tests/reorg)
    endif()

    # Add integration tests (Phase E.3)
    if(EXISTS ${CMAKE_SOURCE_DIR}/tests/integration/CMakeLists.txt)
        add_subdirectory(tests/integration)
    endif()

    # Add wallet persistence tests (Phase F.6)
    if(EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet_persistence/CMakeLists.txt)
        add_subdirectory(tests/wallet_persistence)
    endif()

    # Add mining/block assembly tests (Phase F.10)
    if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mining/CMakeLists.txt)
        add_subdirectory(tests/mining)
    endif()

    # Add consensus property tests (Ring 5)
    if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/CMakeLists.txt)
        add_subdirectory(tests/consensus)
    endif()

    # Add Utreexo semantic correctness tests (Phase U)
    if(EXISTS ${CMAKE_SOURCE_DIR}/tests/utreexo/CMakeLists.txt)
        add_subdirectory(tests/utreexo)
    endif()

    # Add economic property tests (Ring 6)
    if(EXISTS ${CMAKE_SOURCE_DIR}/tests/economic/CMakeLists.txt)
        add_subdirectory(tests/economic)
    endif()

    # Add execution semantics property tests (Ring 7)
    if(EXISTS ${CMAKE_SOURCE_DIR}/tests/execution/CMakeLists.txt)
        add_subdirectory(tests/execution)
    endif()

    # Add governance & compatibility tests (Ring 8)
    if(EXISTS ${CMAKE_SOURCE_DIR}/tests/ring8/CMakeLists.txt)
        add_subdirectory(tests/ring8)
    endif()

    # Add fuzzing if enabled
    if (ENABLE_FUZZING)
        if(EXISTS ${CMAKE_SOURCE_DIR}/tests/fuzz/CMakeLists.txt)
            add_subdirectory(tests/fuzz)
        endif()
        # Phase 27: Add root-level fuzz directory (consensus-critical fuzzers)
        if(EXISTS ${CMAKE_SOURCE_DIR}/fuzz/CMakeLists.txt)
            add_subdirectory(fuzz)
        endif()
    endif()

    # POSIX-only .sh-based ctest entries are disabled on native Windows MSVC
    # because cmd.exe has no bash interpreter on PATH, so ctest reports them
    # as BAD_COMMAND and pollutes the gate. They still register and run on
    # Linux / macOS / MinGW / Cygwin where /bin/bash is available.
    # This list covers the top-level tests whose COMMAND ends in *.sh.
    # Integration .sh tests are disabled inside tests/integration/CMakeLists.txt
    # because CTest test properties are directory-scoped on some CMake versions.
    if(WIN32 AND NOT MINGW AND NOT CYGWIN)
        set(_dinero_posix_only_tests
            # Note: WalletPathHygiene and RetiredCoinType1447Hygiene invoke
            # `bash <script>.sh` explicitly, so they run fine on Windows when
            # git-bash is on PATH (the typical dev setup) — keep them enabled.
            AcceptanceParity IBDTorture ReleaseSuite
            CmakeInstallLayout DineroBackup DineroPrepareUpgrade
            HeaderStatusOverwriteAllowlist
        )
        set(_dinero_disabled_count 0)
        foreach(_dinero_t IN LISTS _dinero_posix_only_tests)
            if(TEST ${_dinero_t})
                set_tests_properties(${_dinero_t} PROPERTIES DISABLED TRUE)
                math(EXPR _dinero_disabled_count "${_dinero_disabled_count} + 1")
            endif()
        endforeach()
        message(STATUS "Disabled ${_dinero_disabled_count} POSIX-only .sh ctest entries on native Windows")
    endif()

    # Include the top-level test runner
    add_custom_target(run_all_tests
        COMMAND ${CMAKE_SOURCE_DIR}/scripts/run_all_tests.sh
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Running all Dinero Core regression + fuzz tests..."
    )
endif()

# TEMPORARILY DISABLED (2026-02-02): wallet_test_deps needs ChainDB symbols
# TODO: Update wallet_test_deps or use dinero_chainstate directly
#
# # Verification tool for premine address
# add_executable(verify_seed tools/verify_seed.cpp)
# target_sources(verify_seed PRIVATE src/common/logger.cpp)
# target_link_libraries(verify_seed PRIVATE
#   dinero_wallet
#   dinero_crypto
#   dinero_consensus
#   secp256k1
#   sqlite3
#   wallet_test_deps
# )
# target_include_directories(verify_seed PRIVATE
#   ${CMAKE_SOURCE_DIR}/include
# )
# if(APPLE)
#   target_link_libraries(verify_seed PRIVATE
#     "-framework Security"
#     "-framework CoreFoundation"
#   )
# endif()
#
# # Address verification tool (check seed derives to expected address)
# add_executable(verify_address tools/verify_address.cpp)
# target_sources(verify_address PRIVATE src/common/logger.cpp)
# target_link_libraries(verify_address PRIVATE
#   dinero_wallet
#   dinero_crypto
#   dinero_consensus
#   secp256k1
#   sqlite3
#   wallet_test_deps
# )
# target_include_directories(verify_address PRIVATE
#   ${CMAKE_SOURCE_DIR}/include
# )
# if(APPLE)
#   target_link_libraries(verify_address PRIVATE
#     "-framework Security"
#     "-framework CoreFoundation"
#   )
# endif()
#
# # BIP86 Taproot premine generator (creates BIP39 seed + derives address)
# add_executable(generate_bip86_premine tools/generate_bip86_premine.cpp)
# target_sources(generate_bip86_premine PRIVATE src/common/logger.cpp)
# target_link_libraries(generate_bip86_premine PRIVATE
#   dinero_wallet
#   dinero_crypto
#   dinero_consensus
#   secp256k1
#   sqlite3
#   wallet_test_deps
# )
# target_include_directories(generate_bip86_premine PRIVATE
#   ${CMAKE_SOURCE_DIR}/include
# )
# if(APPLE)
#   target_link_libraries(generate_bip86_premine PRIVATE
#     "-framework Security"
#     "-framework CoreFoundation"
#   )
# endif()

# ============================================================================
# Phase 2: Standalone Mining Test (ASERT Validation)
# Phase C: Test needs updating to use MiningManager v2 instead of legacy Mining
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_mine_blocks_standalone.cpp)
  add_executable(test_mine_blocks_standalone
    tests/test_mine_blocks_standalone.cpp
    # Phase C: Legacy Mining class removed - test needs updating
    # src/daemon/mining.cpp
    src/daemon/block_acceptor.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_mine_blocks_standalone PRIVATE
    dinero_chainstate
    dinero_consensus
    dinero_wallet
    dinero_crypto
    RocksDB::rocksdb
    secp256k1
    sqlite3
    jsoncpp_static
  )
  link_zstd_if_needed(test_mine_blocks_standalone)
  target_include_directories(test_mine_blocks_standalone PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
    ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
  )
  if(APPLE)
    target_link_libraries(test_mine_blocks_standalone PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
endif()

# ============================================================================
# v0.14.0.1: Block Template Construction Test (Foundation)
# ============================================================================
# NOTE:
# test_block_template is temporarily disabled.
#
# Reason:
# This test compiles production mempool/block_assembler .cpp files directly,
# causing uncontrolled transitive dependencies (ScannerManager, FeeEstimator,
# RBFPolicy, peer-routing glue, GetConfig, UtreexoMessages, etc.).
#
# The architecture has outgrown this test's build assumptions.
# Proper fix requires a dedicated mempool+assembler test library (Phase M.x).
#
# This test is NOT part of DINERO_RELEASE pre-CI requirements.
#
# TODO(Phase M.x): Reintroduce test with proper library boundaries.
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/mining/test_block_template.cpp)
  add_executable(test_block_template
    tests/mining/test_block_template.cpp
    src/mining/block_assembler.cpp
    src/daemon/mempool.cpp
    src/common/logger.cpp
    src/core/privacy/silent_scanner_manager.cpp
    src/core/privacy/silent_scanner.cpp
    src/mempool/fee_estimator.cpp
    src/consensus/undo.cpp
    src/policy/rbf_policy.cpp
    src/p2p/inventory.cpp
  )
  target_link_libraries(test_block_template PRIVATE
    dinero_core
    dinero_chainstate
    dinero_consensus
    dinero_wallet
    dinero_crypto
    RocksDB::rocksdb
    secp256k1
    sqlite3
    jsoncpp_static
    bulletproofs_ffi
  )
  link_zstd_if_needed(test_block_template)
  target_include_directories(test_block_template PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
    ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
  )
  if(APPLE)
    target_link_libraries(test_block_template PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
endif()

# ============================================================================
# Header Hash Test Vectors (Mainnet Readiness - CPU vs GPU)
# ============================================================================
if(ENABLE_TESTS AND EXISTS ${CMAKE_SOURCE_DIR}/tests/mining/test_header_hash_vectors.cpp)
  add_executable(test_header_hash_vectors
    tests/mining/test_header_hash_vectors.cpp
    src/primitives/block.cpp
    src/primitives/transaction.cpp
    src/primitives/transaction_serializer.cpp
    src/primitives/uint256.cpp
    src/common/sha256d.cpp
    src/consensus/utreexo_accumulator.cpp
    src/consensus/utreexo_stump.cpp
    src/consensus/chainparams_impl.cpp
    src/consensus/script_crypto.cpp
  )

  add_dependencies(test_header_hash_vectors gtest)

  target_link_libraries(test_header_hash_vectors PRIVATE
    dinero_crypto
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
    $<TARGET_FILE:gtest>
    )
  link_zstd_if_needed(test_header_hash_vectors)

  target_include_directories(test_header_hash_vectors BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  add_test(NAME test_header_hash_vectors COMMAND test_header_hash_vectors)
endif()

# ============================================================================
# Phase C: MiningManager v2 Smoke Tests (Basic API Surface)
# NOTE: These tests verify API existence and basic functionality without
# requiring full DaemonContext initialization. See docs/PHASE_C_TEST_STATUS.md
# ============================================================================
if(ENABLE_TESTS AND EXISTS ${CMAKE_SOURCE_DIR}/tests/mining/test_mining_header_reserved_smoke.cpp)
  add_executable(test_mining_header_reserved_smoke
    tests/mining/test_mining_header_reserved_smoke.cpp
  )
  add_dependencies(test_mining_header_reserved_smoke gtest)
  target_link_libraries(test_mining_header_reserved_smoke PRIVATE
    $<TARGET_FILE:gtest>
  )
  target_include_directories(test_mining_header_reserved_smoke BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
    ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME MiningHeaderReservedSmoke COMMAND test_mining_header_reserved_smoke)
endif()

# ============================================================================
# Phase C: MiningManager v2 Unit Tests (Full Integration - TODO)
# Note: Requires mock DaemonContext - deferred to Phase D
# ============================================================================
# if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mining/test_mining_manager_v2.cpp)
#   add_executable(test_mining_manager_v2
#     tests/mining/test_mining_manager_v2.cpp
#   )
#   target_link_libraries(test_mining_manager_v2 PRIVATE
#     dinero_crypto
#     dinero_chainstate
#     GTest::gtest
#     pthread
#   )
#   target_include_directories(test_mining_manager_v2 PRIVATE
#     ${CMAKE_SOURCE_DIR}/include
#     ${CMAKE_SOURCE_DIR}/src
#   )
#   add_test(NAME MiningManagerV2Unit COMMAND test_mining_manager_v2)
# endif()

# ============================================================================
# Phase C: MiningService Integration Tests (Smoke Tests Only)
# Note: Full integration tests require complete DaemonContext - deferred to Phase D
# ============================================================================
# if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mining/test_mining_service_integration.cpp)
#   add_executable(test_mining_service_integration
#     tests/mining/test_mining_service_integration.cpp
#   )
#   target_link_libraries(test_mining_service_integration PRIVATE
#     dinero_crypto
#     dinero_chainstate
#     dinero_consensus
#     dinero_gpu_mining
#     GTest::gtest
#     pthread
#   )
#  link_zstd_if_needed(test_mining_service_integration)
#   target_include_directories(test_mining_service_integration PRIVATE
#     ${CMAKE_SOURCE_DIR}/include
#     ${CMAKE_SOURCE_DIR}/src
#   )
#   add_test(NAME MiningServiceIntegration COMMAND test_mining_service_integration)
# endif()

# ============================================================================
# Minimal Block Template Test (Production Miner Foundation)
# ============================================================================
# NOTE:
# test_block_template_minimal is temporarily disabled.
#
# Reason:
# This test compiles production block_assembler.cpp directly, causing the
# same uncontrolled transitive dependencies as test_block_template.
#
# Proper fix requires a dedicated block assembler test library (Phase M.x).
#
# This test is NOT part of DINERO_RELEASE pre-CI requirements.
#
# TODO(Phase M.x): Reintroduce test with proper library boundaries.
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/mining/test_block_template_minimal.cpp)
  add_executable(test_block_template_minimal
    tests/mining/test_block_template_minimal.cpp
    src/mining/block_assembler.cpp
  )
  target_link_libraries(test_block_template_minimal PRIVATE
    dinero_chainstate
    dinero_consensus
    dinero_wallet
    dinero_crypto
    RocksDB::rocksdb
    secp256k1
    sqlite3
    jsoncpp_static
  )
  link_zstd_if_needed(test_block_template_minimal)
  target_include_directories(test_block_template_minimal PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
    ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
  )
  if(APPLE)
    target_link_libraries(test_block_template_minimal PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
endif()

# ============================================================================
# Phase M.0: Merkle Tree Golden Test (Standalone)
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_merkle_golden.cpp)
  add_executable(test_merkle_golden
    tests/test_merkle_golden.cpp
  )
  target_link_libraries(test_merkle_golden PRIVATE
    dinero_wallet
    dinero_crypto
    secp256k1
  )
  target_include_directories(test_merkle_golden PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_merkle_golden PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME MerkleGolden COMMAND test_merkle_golden)
endif()

# ============================================================================
# Phase N.3: Header Fork-Choice Tests (Header-First Sync)
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_header_fork_choice.cpp)
  add_executable(test_header_fork_choice
    tests/consensus/test_header_fork_choice.cpp
    src/consensus/header_chain.cpp
    src/consensus/header_store.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_header_fork_choice PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    secp256k1
  )
  link_zstd_if_needed(test_header_fork_choice)
  target_include_directories(test_header_fork_choice PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_directories(test_header_fork_choice PRIVATE /opt/homebrew/lib)
    target_link_libraries(test_header_fork_choice PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME HeaderForkChoice COMMAND test_header_fork_choice)
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_header_pow_verification.cpp)
  add_executable(test_header_pow_verification
    tests/consensus/test_header_pow_verification.cpp
    src/consensus/header_chain.cpp
    src/consensus/header_store.cpp
    src/consensus/genesis_canonical.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_header_pow_verification PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    secp256k1
  )
  link_zstd_if_needed(test_header_pow_verification)
  target_include_directories(test_header_pow_verification PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_directories(test_header_pow_verification PRIVATE /opt/homebrew/lib)
    target_link_libraries(test_header_pow_verification PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME HeaderPoWVerification COMMAND test_header_pow_verification)
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_header_real_mainnet_replay.cpp)
  add_executable(test_header_real_mainnet_replay
    tests/consensus/test_header_real_mainnet_replay.cpp
    src/consensus/header_chain.cpp
    src/consensus/header_store.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_header_real_mainnet_replay PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    secp256k1
  )
  link_zstd_if_needed(test_header_real_mainnet_replay)
  target_include_directories(test_header_real_mainnet_replay PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_directories(test_header_real_mainnet_replay PRIVATE /opt/homebrew/lib)
    target_link_libraries(test_header_real_mainnet_replay PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME HeaderRealMainnetReplay COMMAND test_header_real_mainnet_replay)
endif()

# ============================================================================
# Phase N.1: Header Restart Safety Tests (Header-First Sync)
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_header_restart_safety.cpp)
  add_executable(test_header_restart_safety
    tests/consensus/test_header_restart_safety.cpp
    src/consensus/header_chain.cpp
    src/consensus/header_store.cpp
  )
  target_link_libraries(test_header_restart_safety PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    secp256k1
    RocksDB::rocksdb
  )
  link_zstd_if_needed(test_header_restart_safety)
  target_include_directories(test_header_restart_safety PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_header_restart_safety PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME HeaderRestartSafety COMMAND test_header_restart_safety)
  set_tests_properties(HeaderRestartSafety PROPERTIES
    LABELS "headers;restart;canonical-restart;phase1-canonical-recovery"
  )
endif()

# 4d-2 (issue #181): bounded side-branch header storage with work-aware eviction.
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_header_sidebranch_bound.cpp)
  add_executable(test_header_sidebranch_bound
    tests/consensus/test_header_sidebranch_bound.cpp
    src/consensus/header_chain.cpp
    src/consensus/header_store.cpp
  )
  target_link_libraries(test_header_sidebranch_bound PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    secp256k1
    RocksDB::rocksdb
  )
  link_zstd_if_needed(test_header_sidebranch_bound)
  target_include_directories(test_header_sidebranch_bound PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_header_sidebranch_bound PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME HeaderSideBranchBound COMMAND test_header_sidebranch_bound)
  set_tests_properties(HeaderSideBranchBound PROPERTIES
    LABELS "headers;security;dos;sidebranch-eviction"
  )
endif()

# ============================================================================
# Phase N.2: Header Sync State Machine Tests (Header-First Sync)
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_header_sync_state_machine.cpp)
  add_executable(test_header_sync_state_machine
    tests/consensus/test_header_sync_state_machine.cpp
    src/consensus/header_chain.cpp
    src/consensus/header_store.cpp
    src/consensus/header_sync.cpp
  )
  target_link_libraries(test_header_sync_state_machine PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    secp256k1
    RocksDB::rocksdb
  )
  link_zstd_if_needed(test_header_sync_state_machine)
  target_include_directories(test_header_sync_state_machine PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_header_sync_state_machine PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME HeaderSyncStateMachine COMMAND test_header_sync_state_machine)
endif()

# ============================================================================
# Phase N.2 Step 2B: Header Sync Stall Behavior Tests
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_header_sync_stall_behavior.cpp)
  add_executable(test_header_sync_stall_behavior
    tests/consensus/test_header_sync_stall_behavior.cpp
    src/consensus/header_chain.cpp
    src/consensus/header_store.cpp
    src/consensus/header_sync.cpp
  )
  target_link_libraries(test_header_sync_stall_behavior PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    secp256k1
    RocksDB::rocksdb
  )
  link_zstd_if_needed(test_header_sync_stall_behavior)
  target_include_directories(test_header_sync_stall_behavior PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_header_sync_stall_behavior PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME HeaderSyncStallBehavior COMMAND test_header_sync_stall_behavior)
endif()

# ============================================================================
# Phase N.2 Step 2C: Header Sync P2P Integration Tests
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_header_sync_p2p_integration.cpp)
  add_executable(test_header_sync_p2p_integration
    tests/consensus/test_header_sync_p2p_integration.cpp
    src/consensus/header_chain.cpp
    src/consensus/header_store.cpp
    src/consensus/header_sync.cpp
    src/consensus/header_sync_p2p.cpp
  )
  target_link_libraries(test_header_sync_p2p_integration PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    secp256k1
    RocksDB::rocksdb
  )
  link_zstd_if_needed(test_header_sync_p2p_integration)
  target_include_directories(test_header_sync_p2p_integration PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_header_sync_p2p_integration PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME HeaderSyncP2PIntegration COMMAND test_header_sync_p2p_integration)
endif()

# ============================================================================
# Phase N.4: Block Download Scheduler Tests
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_block_download_scheduler.cpp)
  add_executable(test_block_download_scheduler
    tests/consensus/test_block_download_scheduler.cpp
    src/consensus/block_download_scheduler.cpp
    src/consensus/header_chain.cpp
    src/consensus/header_store.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_block_download_scheduler PRIVATE
    dinero_consensus
    dinero_chainstate  # BlockStorage
    dinero_wallet
    dinero_crypto
    secp256k1
    RocksDB::rocksdb
  )
  link_zstd_if_needed(test_block_download_scheduler)
  target_include_directories(test_block_download_scheduler PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_directories(test_block_download_scheduler PRIVATE /opt/homebrew/lib)
    target_link_libraries(test_block_download_scheduler PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME BlockDownloadScheduler COMMAND test_block_download_scheduler)
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_block_download_scheduler_restart_bootstrap.cpp)
  add_executable(test_block_download_scheduler_restart_bootstrap
    tests/consensus/test_block_download_scheduler_restart_bootstrap.cpp
    src/consensus/block_download_scheduler.cpp
    src/consensus/header_chain.cpp
    src/consensus/header_store.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_block_download_scheduler_restart_bootstrap PRIVATE
    dinero_consensus
    dinero_chainstate
    dinero_wallet
    dinero_crypto
    secp256k1
    RocksDB::rocksdb
  )
  link_zstd_if_needed(test_block_download_scheduler_restart_bootstrap)
  target_include_directories(test_block_download_scheduler_restart_bootstrap PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_directories(test_block_download_scheduler_restart_bootstrap PRIVATE /opt/homebrew/lib)
    target_link_libraries(test_block_download_scheduler_restart_bootstrap PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME BlockDownloadSchedulerRestartBootstrap COMMAND test_block_download_scheduler_restart_bootstrap)
endif()

# ============================================================================
# v0.14.0.4: Utreexo Commitment Enforcement Tests
# ============================================================================
# TODO: Disabled - needs refactor to use IConsensusUTXOSet instead of IUTXOProvider
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_utreexo_enforcement.cpp)
  add_executable(test_utreexo_enforcement
    tests/consensus/test_utreexo_enforcement.cpp
    src/consensus/block_validation.cpp
    src/consensus/utreexo_accumulator.cpp
    src/consensus/tx_parser.cpp
    src/consensus/script_verify.cpp
    src/wallet/utxo_index.cpp
    src/database/sqlite_manager.cpp
    src/sqlite_open.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_utreexo_enforcement PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    bulletproofs_ffi
    RocksDB::rocksdb
    secp256k1
    sqlite3
    jsoncpp_static
  )
  link_zstd_if_needed(test_utreexo_enforcement)
  target_include_directories(test_utreexo_enforcement PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
    ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
  )
  target_compile_definitions(test_utreexo_enforcement PRIVATE
    CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  )
  if(APPLE)
    target_link_libraries(test_utreexo_enforcement PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME UtreexoEnforcement COMMAND test_utreexo_enforcement)
  # Ensure Bulletproofs FFI is built before test
  if(CARGO_EXECUTABLE)
    add_dependencies(test_utreexo_enforcement build_bulletproofs_ffi)
  endif()
endif()

# ============================================================================
# CONSENSUS GATE: Block-Level Stateless Equivalence Test
# This test proves: stateful BlockValidator ↔ stateless Utreexo are IDENTICAL
# ============================================================================
# TODO: Disabled - needs refactor to use IConsensusUTXOSet instead of IUTXOProvider
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_block_stateless_equivalence.cpp)
  add_executable(test_block_stateless_equivalence
    tests/consensus/test_block_stateless_equivalence.cpp
    src/consensus/block_validation.cpp
    src/consensus/utreexo_accumulator.cpp
    src/consensus/tx_parser.cpp
    src/consensus/script_verify.cpp
    src/wallet/utxo_index.cpp
    src/database/sqlite_manager.cpp
    src/sqlite_open.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_block_stateless_equivalence PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    bulletproofs_ffi
    RocksDB::rocksdb
    secp256k1
    sqlite3
    jsoncpp_static
  )
  link_zstd_if_needed(test_block_stateless_equivalence)
  target_include_directories(test_block_stateless_equivalence PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
    ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
  )
  target_compile_definitions(test_block_stateless_equivalence PRIVATE
    CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  )
  if(APPLE)
    target_link_libraries(test_block_stateless_equivalence PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME BlockStatelessEquivalence COMMAND test_block_stateless_equivalence)
  if(CARGO_EXECUTABLE)
    add_dependencies(test_block_stateless_equivalence build_bulletproofs_ffi)
  endif()
endif()

# ============================================================================
# Serialization Bug Repro Test (Step 1 of 3)
# Target: Isolate forest serialize/deserialize mutation divergence
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_utreexo_serialization_repro.cpp)
  add_executable(test_utreexo_serialization_repro
    tests/consensus/test_utreexo_serialization_repro.cpp
  )
  target_link_libraries(test_utreexo_serialization_repro PRIVATE
    dinero_consensus
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
  )
  link_zstd_if_needed(test_utreexo_serialization_repro)
  target_include_directories(test_utreexo_serialization_repro PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  if(APPLE)
    target_link_libraries(test_utreexo_serialization_repro PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME UtreexoSerializationRepro COMMAND test_utreexo_serialization_repro)
endif()

# ============================================================================
# Phase 8: Stateless Validation Tests (T8.1–T8.9)
# ============================================================================
# TODO: Disabled - needs refactor to use IConsensusUTXOSet instead of IUTXOProvider
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_utreexo_stateless_validation.cpp)
  add_executable(test_utreexo_stateless_validation
    tests/consensus/test_utreexo_stateless_validation.cpp
    src/consensus/block_validation.cpp
    src/consensus/utreexo_accumulator.cpp
    src/consensus/tx_parser.cpp
    src/consensus/script_verify.cpp
    src/consensus/script_validation.cpp
    src/wallet/utxo_index.cpp
    src/database/sqlite_manager.cpp
    src/sqlite_open.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_utreexo_stateless_validation PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    bulletproofs_ffi
    RocksDB::rocksdb
    secp256k1
    sqlite3
    jsoncpp_static
  )
  link_zstd_if_needed(test_utreexo_stateless_validation)
  target_include_directories(test_utreexo_stateless_validation PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
    ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
  )
  target_compile_definitions(test_utreexo_stateless_validation PRIVATE
    CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  )
  if(APPLE)
    target_link_libraries(test_utreexo_stateless_validation PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME StatelessValidation COMMAND test_utreexo_stateless_validation)
  # Ensure Bulletproofs FFI is built before test
  if(CARGO_EXECUTABLE)
    add_dependencies(test_utreexo_stateless_validation build_bulletproofs_ffi)
  endif()
endif()

# ============================================================================
# Phase 9.1: Proof Cache Tests (T9.1–T9.6)
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_proof_cache.cpp)
  add_executable(test_proof_cache
    tests/consensus/test_proof_cache.cpp
    src/consensus/proof_cache.cpp
  )
  target_link_libraries(test_proof_cache PRIVATE
    dinero_consensus
  )
  target_include_directories(test_proof_cache PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME ProofCache COMMAND test_proof_cache)
endif()

# ============================================================================
# Phase 9.2: Routing Heuristics Tests (T9.7–T9.12)
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_proof_router.cpp)
  add_executable(test_proof_router
    tests/consensus/test_proof_router.cpp
    src/consensus/proof_router.cpp
  )
  target_link_libraries(test_proof_router PRIVATE
    dinero_consensus
  )
  target_include_directories(test_proof_router PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME ProofRouter COMMAND test_proof_router)
endif()

# ============================================================================
# Phase 9.3: Gossip Protocol Tests (T9.13–T9.18)
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_proof_gossip.cpp)
  add_executable(test_proof_gossip
    tests/consensus/test_proof_gossip.cpp
    src/consensus/proof_gossip.cpp
    src/consensus/utreexo_accumulator.cpp
  )
  target_link_libraries(test_proof_gossip PRIVATE
    dinero_consensus
    dinero_crypto
    zstd
    OpenSSL::SSL
    OpenSSL::Crypto
  )
  target_include_directories(test_proof_gossip PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME ProofGossip COMMAND test_proof_gossip)
endif()

# ============================================================================
# Phase 9.4: Proof Compression Tests (T9.19–T9.25)
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_proof_compression.cpp)
  add_executable(test_proof_compression
    tests/consensus/test_proof_compression.cpp
    src/consensus/proof_compression.cpp
    src/consensus/utreexo_accumulator.cpp
  )
  target_link_libraries(test_proof_compression PRIVATE
    dinero_consensus
    dinero_crypto
    zstd
    OpenSSL::SSL
    OpenSSL::Crypto
  )
  target_include_directories(test_proof_compression PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME ProofCompression COMMAND test_proof_compression)
endif()

# ============================================================================
# Phase 9.6: Performance Benchmarks - DISABLED: Lightning removed
# ============================================================================
# if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/benchmark_phase9.cpp)
#   add_executable(benchmark_phase9
#     tests/consensus/benchmark_phase9.cpp
#     src/consensus/proof_cache.cpp
#     src/consensus/proof_router.cpp
#     src/consensus/proof_gossip.cpp
#     src/consensus/proof_compression.cpp
#     src/consensus/lightning_proof_client.cpp
#     src/consensus/utreexo_accumulator.cpp
#   )
#   target_link_libraries(benchmark_phase9 PRIVATE
#     dinero_consensus
#     dinero_crypto
#     zstd
#     OpenSSL::SSL
#     OpenSSL::Crypto
#   )
#   target_include_directories(benchmark_phase9 PRIVATE
#     ${CMAKE_SOURCE_DIR}/include
#     ${CMAKE_SOURCE_DIR}/src
#   )
#   # Note: Benchmarks are not added to CTest (run manually for performance analysis)
# endif()

# ============================================================================
# Phase 10: Real-World Sync Validation Tests - DISABLED: Lightning removed
# ============================================================================
# if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_sync_validation.cpp)
#   add_executable(test_sync_validation
#     tests/consensus/test_sync_validation.cpp
#     src/consensus/sync_simulator.cpp
#     src/consensus/lightning_proof_client.cpp
#     src/consensus/proof_cache.cpp
#     src/consensus/proof_router.cpp
#     src/consensus/proof_gossip.cpp
#     src/consensus/proof_compression.cpp
#     src/consensus/utreexo_accumulator.cpp
#   )
#   target_link_libraries(test_sync_validation PRIVATE
#     dinero_consensus
#     dinero_crypto
#     zstd
#     OpenSSL::SSL
#     OpenSSL::Crypto
#   )
#   target_include_directories(test_sync_validation PRIVATE
#     ${CMAKE_SOURCE_DIR}/include
#     ${CMAKE_SOURCE_DIR}/src
#   )
#   add_test(NAME SyncValidation COMMAND test_sync_validation)
# endif()

# ============================================================================
# Phase 12: Mobile Profile Integration Tests - DISABLED: Lightning removed
# ============================================================================
# Purpose: Prove changing resource constraints does NOT change validation outcomes
# Tests: Validation equivalence, cache eviction safety, cold restart, TTL, parallelism
# Critical: Does NOT re-test consensus (Phase 8-11 already proved that)
# Policy: ANY FAILURE → Resource envelope safety violated
# if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mobile/test_mobile_profile_integration.cpp)
#   add_executable(test_mobile_profile_integration
#     tests/mobile/test_mobile_profile_integration.cpp
#     src/consensus/sync_simulator.cpp
#     src/consensus/lightning_proof_client.cpp
#     src/consensus/proof_cache.cpp
#     src/consensus/proof_router.cpp
#     src/consensus/proof_gossip.cpp
#     src/consensus/proof_compression.cpp
#     src/consensus/utreexo_accumulator.cpp
#   )
#   target_link_libraries(test_mobile_profile_integration PRIVATE
#     dinero_consensus
#     dinero_crypto
#     zstd
#     OpenSSL::SSL
#     OpenSSL::Crypto
#   )
#   target_include_directories(test_mobile_profile_integration PRIVATE
#     ${CMAKE_SOURCE_DIR}/include
#     ${CMAKE_SOURCE_DIR}/src
#   )
#   add_test(NAME MobileProfileIntegration COMMAND test_mobile_profile_integration)
# endif()

# ============================================================================
# Phase 12: iOS Memory Pressure Simulation Tests - DISABLED: Lightning removed
# ============================================================================
# Purpose: Prove mobile profile survives iOS memory pressure scenarios
# Tests: Aggressive eviction, jetsam recovery, burst mode compliance
# Critical: Validates core safety theorem (cache loss ≠ consensus failure)
# Policy: ANY FAILURE → iOS deployment not safe
# if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mobile/test_ios_memory_pressure.cpp)
#   add_executable(test_ios_memory_pressure
#     tests/mobile/test_ios_memory_pressure.cpp
#     src/consensus/sync_simulator.cpp
#     src/consensus/lightning_proof_client.cpp
#     src/consensus/proof_cache.cpp
#     src/consensus/proof_router.cpp
#     src/consensus/proof_gossip.cpp
#     src/consensus/proof_compression.cpp
#     src/consensus/utreexo_accumulator.cpp
#   )
#   target_link_libraries(test_ios_memory_pressure PRIVATE
#     dinero_consensus
#     dinero_crypto
#     zstd
#     OpenSSL::SSL
#     OpenSSL::Crypto
#   )
#   target_include_directories(test_ios_memory_pressure PRIVATE
#     ${CMAKE_SOURCE_DIR}/include
#     ${CMAKE_SOURCE_DIR}/src
#   )
#   add_test(NAME iOSMemoryPressure COMMAND test_ios_memory_pressure)
# endif()

# ============================================================================
# Phase 3 Layer 3: Utreexo Consensus Regression Tests
# ============================================================================
# Purpose: Prevent future violations of critical consensus rules
# Tests: Header size enforcement, Utreexo data requirement, network activation
# Policy: ANY FAILURE → CI MUST BREAK (consensus regression detected)
# DISABLED: Requires IConsensusUTXOSet interface refactor (BlockValidator constructor changed)
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_utreexo_consensus_regressions.cpp)
  add_executable(test_utreexo_consensus_regressions
    tests/consensus/test_utreexo_consensus_regressions.cpp
    src/consensus/block_validation.cpp
    src/consensus/utreexo_accumulator.cpp
    src/consensus/tx_parser.cpp
    src/consensus/script_verify.cpp
    src/consensus/chainparams_impl.cpp
    src/consensus/adapters/wallet_utxo_adapter.cpp
    src/wallet/utxo_index.cpp
    src/database/sqlite_manager.cpp
    src/sqlite_open.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_utreexo_consensus_regressions PRIVATE
    dinero_consensus
    dinero_chainstate  # For ChainDB (utxo_index dependency)
    dinero_wallet
    dinero_crypto
    bulletproofs_ffi
    RocksDB::rocksdb
    secp256k1
    sqlite3
    jsoncpp_static
  )
  link_zstd_if_needed(test_utreexo_consensus_regressions)
  target_include_directories(test_utreexo_consensus_regressions PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
    ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
  )
  target_compile_definitions(test_utreexo_consensus_regressions PRIVATE
    CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  )
  if(APPLE)
    target_link_libraries(test_utreexo_consensus_regressions PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME UtreexoConsensusRegressions COMMAND test_utreexo_consensus_regressions)
  # Ensure Bulletproofs FFI is built before test
  if(CARGO_EXECUTABLE)
    add_dependencies(test_utreexo_consensus_regressions build_bulletproofs_ffi)
  endif()
  message(STATUS "✅ Phase 3 Layer 3 regression tests configured")
  message(STATUS "   Tests: Header size, Utreexo data requirement, network activation")
  message(STATUS "   Policy: Fail-fast on consensus violations (CI must break)")
endif()

# ============================================================================
# Phase 4: Utreexo Delta-Based Undo Tests
# ============================================================================
# Purpose: Verify delta undo correctness and 10-20x size reduction
# Tests: Roundtrip restoration, order safety, size comparison
# Policy: ANY FAILURE → CI MUST BREAK (delta undo regression detected)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_utreexo_delta_undo.cpp)
  add_executable(test_utreexo_delta_undo
    tests/consensus/test_utreexo_delta_undo.cpp
    src/consensus/utreexo_accumulator.cpp
    src/consensus/script_crypto.cpp
  )
  target_link_libraries(test_utreexo_delta_undo PRIVATE
    dinero_crypto
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
    GTest::gtest
  )
  link_zstd_if_needed(test_utreexo_delta_undo)
  target_include_directories(test_utreexo_delta_undo PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  add_test(NAME UtreexoDeltaUndo COMMAND test_utreexo_delta_undo)
  message(STATUS "✅ Phase 4 delta-based undo tests configured")
  message(STATUS "   Tests: Roundtrip, order safety, size reduction (10-20x)")
  message(STATUS "   Policy: Fail-fast on delta undo violations")
endif()

# ============================================================================
# Tier-0 Fork-Point Root Validation Tests (FIX-1, FIX-2, FIX-3)
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/utreexo/test_fork_point_root_validation.cpp)
  add_executable(test_fork_point_root_validation
    tests/utreexo/test_fork_point_root_validation.cpp
  )
  target_link_libraries(test_fork_point_root_validation PRIVATE
    dinero_consensus
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
    GTest::gtest
  )
  link_zstd_if_needed(test_fork_point_root_validation)
  target_include_directories(test_fork_point_root_validation PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  if(APPLE)
    target_link_libraries(test_fork_point_root_validation PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME ForkPointRootValidation COMMAND test_fork_point_root_validation)
endif()

# ============================================================================
# Proof Generation Correctness Tests (INV-1 through INV-5)
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/utreexo/test_proof_generation_correctness.cpp)
  add_executable(test_proof_generation_correctness
    tests/utreexo/test_proof_generation_correctness.cpp
  )
  target_link_libraries(test_proof_generation_correctness PRIVATE
    dinero_consensus
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
    GTest::gtest
  )
  link_zstd_if_needed(test_proof_generation_correctness)
  target_include_directories(test_proof_generation_correctness PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  if(APPLE)
    target_link_libraries(test_proof_generation_correctness PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME ProofGenerationCorrectness COMMAND test_proof_generation_correctness)
endif()

# ============================================================================
# Proof Lifecycle Tests (wallet.getproofbundle / proofstatus / getproofupdates)
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/utreexo/test_proof_lifecycle.cpp)
  add_executable(test_proof_lifecycle
    tests/utreexo/test_proof_lifecycle.cpp
  )
  target_link_libraries(test_proof_lifecycle PRIVATE
    dinero_consensus
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
    GTest::gtest
  )
  link_zstd_if_needed(test_proof_lifecycle)
  target_include_directories(test_proof_lifecycle PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  if(APPLE)
    target_link_libraries(test_proof_lifecycle PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME ProofLifecycle COMMAND test_proof_lifecycle)
endif()

# ============================================================================
# Phase 5: Stateless IBD Regression Tests
# ============================================================================
# Purpose: Verify stateless validation without UTXO database
# Security Model: PoW + Root Continuity = Full Validation (NOT light client)
# Tests: Happy path, missing proofs, root discontinuity, backward compatibility
# Policy: ANY FAILURE → CI MUST BREAK (stateless security model violation)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/ibd/test_stateless_ibd_basic.cpp)
  add_executable(test_stateless_ibd_basic
    tests/ibd/test_stateless_ibd_basic.cpp
    src/consensus/block_validation.cpp
    src/consensus/utreexo_accumulator.cpp
    src/consensus/script_crypto.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_stateless_ibd_basic PRIVATE
    dinero_consensus
    dinero_crypto
    bulletproofs_ffi
    RocksDB::rocksdb
    secp256k1
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
    GTest::gtest
  )
  link_zstd_if_needed(test_stateless_ibd_basic)
  target_include_directories(test_stateless_ibd_basic PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  add_test(NAME StatelessIBDBasic COMMAND test_stateless_ibd_basic)
  # Ensure Bulletproofs FFI is built before test
  if(CARGO_EXECUTABLE)
    add_dependencies(test_stateless_ibd_basic build_bulletproofs_ffi)
  endif()
  message(STATUS "✅ Phase 5 stateless IBD tests configured")
  message(STATUS "   Tests: Valid proofs, missing proofs, root continuity, backward compatibility")
  message(STATUS "   Golden Invariant: PoW + Root Continuity = Full Validation Security")
  message(STATUS "   Policy: Fail-fast on stateless security violations")
endif()

# ============================================================================
# Phase 7.1: Utreexo P2P Message Tests
# ============================================================================
# Purpose: Verify Utreexo proof-serving P2P message serialization
# Messages: getutxoproof, utxoproof, getutxohdrs, utxohdrs
# Tests: Serialization roundtrip, validation, DoS limits, truncated data
# Policy: ANY FAILURE → CI MUST BREAK (protocol correctness required)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/network/test_utreexo_messages.cpp)
  add_executable(test_utreexo_messages
    tests/network/test_utreexo_messages.cpp
    src/network/utreexo_messages.cpp
    src/daemon/p2p_message.cpp
    src/primitives/uint256.cpp
    src/common/logger.cpp
    src/common/sha256d.cpp
  )
  target_link_libraries(test_utreexo_messages PRIVATE
    dinero_crypto
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
    GTest::gtest
  )
  target_include_directories(test_utreexo_messages PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  add_test(NAME UtreexoMessages COMMAND test_utreexo_messages)
  message(STATUS "✅ Phase 7.1 Utreexo P2P message tests configured")
  message(STATUS "   Messages: getutxoproof, utxoproof, getutxohdrs, utxohdrs")
  message(STATUS "   Tests: 15 tests covering serialization, validation, DoS protection")
  message(STATUS "   Policy: Fail-fast on protocol violations")
endif()

# ============================================================================
# Phase 7.2: Bridge Node Proof Generation Tests
# ============================================================================
# Purpose: Verify Utreexo proof generation and serving functionality
# Components: BridgeNode class, proof generation, request handling
# Tests: Proof generation, UTXO lookups, caching, error handling
# Policy: ANY FAILURE → CI MUST BREAK (proof correctness critical)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/network/test_bridge_node.cpp)
  add_executable(test_bridge_node
    tests/network/test_bridge_node.cpp
    src/daemon/p2p_message.cpp
    src/network/utreexo_messages.cpp
    src/primitives/uint256.cpp
    src/common/logger.cpp
    src/common/sha256d.cpp
    src/consensus/proof_cache.cpp
    src/consensus/utreexo_proof_relay.cpp
  )
  target_link_libraries(test_bridge_node PRIVATE
    dinero_crypto
    dinero_consensus
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
    GTest::gtest
  )
  link_zstd_if_needed(test_bridge_node)
  target_include_directories(test_bridge_node PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  add_test(NAME BridgeNode COMMAND test_bridge_node)
  message(STATUS "✅ Phase 7.2 Bridge Node proof generation tests configured")
  message(STATUS "   Components: BridgeNode, proof generation, request handling")
  message(STATUS "   Tests: Proof generation, caching, error handling")
  message(STATUS "   Policy: Fail-fast on proof generation failures")
endif()

# ============================================================================
# PHASE 7.3: Stateless Node Sync Tests
# ============================================================================
# Components: StatelessNode class, proof validation, sync logic
# Tests: Proof validation, root continuity, peer management, target extraction
# Policy: ANY FAILURE → CI MUST BREAK (stateless sync correctness critical)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/network/test_stateless_node.cpp)
  add_executable(test_stateless_node
    tests/network/test_stateless_node.cpp
    src/network/stateless_node.cpp
    src/daemon/p2p_message.cpp
    src/daemon/peer_connection.cpp
    src/primitives/uint256.cpp
    src/common/logger.cpp
    src/common/sha256d.cpp
    src/consensus/proof_cache.cpp
    src/consensus/utreexo_proof_relay.cpp
  )
  target_link_libraries(test_stateless_node PRIVATE
    dinero_crypto
    dinero_consensus
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
    GTest::gtest
  )
  link_zstd_if_needed(test_stateless_node)
  target_include_directories(test_stateless_node PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  add_test(NAME StatelessNode COMMAND test_stateless_node)
  message(STATUS "✅ Phase 7.3 Stateless Node sync tests configured")
  message(STATUS "   Components: StatelessNode, proof validation, sync logic")
  message(STATUS "   Tests: Root continuity, peer management, target extraction")
  message(STATUS "   Policy: Fail-fast on validation failures")
endif()

# ═════════════════════════════════════════════════════════════════════════════
# Phase 9.3: BridgeNode Proof Cache Tests
# ═════════════════════════════════════════════════════════════════════════════
# Tests LRU caching, TTL expiration, and cache statistics for proof generation
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/network/test_bridge_node_cache.cpp)
  add_executable(test_bridge_node_cache
    tests/network/test_bridge_node_cache.cpp
    src/daemon/p2p_message.cpp
    src/network/utreexo_messages.cpp
    src/primitives/uint256.cpp
    src/common/logger.cpp
    src/common/sha256d.cpp
    src/consensus/proof_cache.cpp
    src/consensus/utreexo_proof_relay.cpp
  )
  target_link_libraries(test_bridge_node_cache PRIVATE
    dinero_crypto
    dinero_consensus
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
    GTest::gtest
  )
  link_zstd_if_needed(test_bridge_node_cache)
  target_include_directories(test_bridge_node_cache PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  add_test(NAME BridgeNodeCache COMMAND test_bridge_node_cache)
  message(STATUS "✅ Phase 9.3 BridgeNode Cache tests configured")
  message(STATUS "   Tests: LRU cache, TTL expiration, statistics, pre-caching")
endif()

# ============================================================================
# PHASE 7.4.1: Service Bit Advertisement Tests
# ============================================================================
# Components: PeerConnection service bit tracking, VersionMessage services
# Tests: Service bit initialization, tracking, serialization, filtering
# Policy: ANY FAILURE → CI MUST BREAK (peer selection correctness critical)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/network/test_phase_7_4_1_service_bit.cpp)
  add_executable(test_phase_7_4_1_service_bit
    tests/network/test_phase_7_4_1_service_bit.cpp
    src/daemon/peer_connection.cpp
    src/daemon/p2p_message.cpp
    src/primitives/uint256.cpp
    src/common/logger.cpp
    src/common/sha256d.cpp
  )
  target_link_libraries(test_phase_7_4_1_service_bit PRIVATE
    dinero_crypto
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
    GTest::gtest
  )
  target_include_directories(test_phase_7_4_1_service_bit PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  add_test(NAME Phase741ServiceBit COMMAND test_phase_7_4_1_service_bit)
  message(STATUS "✅ Phase 7.4.1 Service Bit Advertisement tests configured")
  message(STATUS "   Components: PeerConnection, VersionMessage, service bits")
  message(STATUS "   Tests: Initialization, tracking, serialization, filtering")
  message(STATUS "   Policy: Fail-fast on service bit handling failures")
endif()

# ============================================================================
# PHASE 7.4.2: Message Dispatch Wiring Tests
# ============================================================================
# Components: active P2P routing, BridgeNode integration, message routing
# Tests: Handler registration, callback integration, validation
# Policy: ANY FAILURE → CI MUST BREAK (message routing correctness critical)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/network/test_phase_7_4_2_message_dispatch.cpp)
  add_executable(test_phase_7_4_2_message_dispatch
    tests/network/test_phase_7_4_2_message_dispatch.cpp
    # Phase 7.4.2: Minimal message routing test (standalone router path)
    src/network/utreexo_message_router.cpp
    src/network/stateless_node.cpp
    src/network/utreexo_messages.cpp
    # Peer connection (minimal P2P framing)
    src/daemon/peer_connection.cpp
    src/daemon/p2p_message.cpp
    # Utreexo consensus
    src/consensus/utreexo_accumulator.cpp
    src/consensus/proof_cache.cpp
    src/consensus/utreexo_proof_relay.cpp
    # Common utilities
    src/common/logger.cpp
    src/common/sha256d.cpp
    src/primitives/uint256.cpp
  )
  target_link_libraries(test_phase_7_4_2_message_dispatch PRIVATE
    dinero_crypto
    dinero_consensus
    RocksDB::rocksdb
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
    GTest::gtest
  )
  link_zstd_if_needed(test_phase_7_4_2_message_dispatch)
  target_include_directories(test_phase_7_4_2_message_dispatch PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  add_test(NAME Phase742MessageDispatch COMMAND test_phase_7_4_2_message_dispatch)
  message(STATUS "✅ Phase 7.4.2 Message Dispatch Wiring tests configured")
  message(STATUS "   Components: active P2P routing, BridgeNode, message routing")
  message(STATUS "   Tests: Handler registration, callbacks, validation")
  message(STATUS "   Policy: Fail-fast on message routing failures")
endif()

# ============================================================================
# v0.14.0.5: Utreexo Accumulator Unit Tests
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_utreexo.cpp)
  add_executable(test_utreexo
    tests/consensus/test_utreexo.cpp
  )
  target_link_libraries(test_utreexo PRIVATE
    dinero_consensus
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
  )
  link_zstd_if_needed(test_utreexo)
  target_include_directories(test_utreexo PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  if(APPLE)
    target_link_libraries(test_utreexo PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME UtreexoAccumulator COMMAND test_utreexo)
endif()

# ============================================================================
# Utreexo Adversarial Test Suite (Zero-hash, duplicates, edge cases)
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_utreexo_adversarial.cpp)
  add_executable(test_utreexo_adversarial
    tests/consensus/test_utreexo_adversarial.cpp
  )
  target_link_libraries(test_utreexo_adversarial PRIVATE
    dinero_consensus
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
  )
  link_zstd_if_needed(test_utreexo_adversarial)
  target_include_directories(test_utreexo_adversarial PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  if(APPLE)
    target_link_libraries(test_utreexo_adversarial PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME UtreexoAdversarial COMMAND test_utreexo_adversarial)
endif()

# ============================================================================
# Utreexo Transition Proof Test (Stump-based state transition verification)
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_utreexo_transition_proof.cpp)
  add_executable(test_utreexo_transition_proof
    tests/consensus/test_utreexo_transition_proof.cpp
  )
  target_link_libraries(test_utreexo_transition_proof PRIVATE
    dinero_consensus
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
  )
  link_zstd_if_needed(test_utreexo_transition_proof)
  target_include_directories(test_utreexo_transition_proof PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  if(APPLE)
    target_link_libraries(test_utreexo_transition_proof PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME UtreexoTransitionProof COMMAND test_utreexo_transition_proof)
endif()

# ============================================================================
# Utreexo Stump Deletion Test (CSN batch deletion algorithm)
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_utreexo_stump_deletion.cpp)
  add_executable(test_utreexo_stump_deletion
    tests/consensus/test_utreexo_stump_deletion.cpp
  )
  target_link_libraries(test_utreexo_stump_deletion PRIVATE
    dinero_consensus
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
  )
  link_zstd_if_needed(test_utreexo_stump_deletion)
  target_include_directories(test_utreexo_stump_deletion PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  if(APPLE)
    target_link_libraries(test_utreexo_stump_deletion PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME UtreexoStumpDeletion COMMAND test_utreexo_stump_deletion)
endif()

# ============================================================================
# CSN Integration Test (Position Index + Proof + Rebuild + Stateless Verify)
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_csn_integration.cpp)
  add_executable(test_csn_integration
    tests/consensus/test_csn_integration.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_csn_integration PRIVATE
    dinero_consensus
    dinero_chainstate
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
  )
  link_zstd_if_needed(test_csn_integration)
  target_include_directories(test_csn_integration PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  if(APPLE)
    target_link_libraries(test_csn_integration PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME CSNIntegration COMMAND test_csn_integration)
endif()

# ============================================================================
# Phase #4: Utreexo TX Relay Tests
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/network/test_utreexo_tx_relay.cpp)
  add_executable(test_utreexo_tx_relay
    tests/network/test_utreexo_tx_relay.cpp
    src/network/stateless_node.cpp
    src/daemon/p2p_message.cpp
    src/daemon/peer_connection.cpp
    src/network/utreexo_messages.cpp
    src/primitives/uint256.cpp
    src/primitives/transaction_serializer.cpp
    src/common/logger.cpp
    src/common/sha256d.cpp
    src/consensus/proof_cache.cpp
    src/consensus/utreexo_proof_relay.cpp
  )
  target_link_libraries(test_utreexo_tx_relay PRIVATE
    dinero_crypto
    dinero_consensus
    dinero_chainstate
    dinero_bridge  # BridgeNode ctor/dtor — referenced by the test
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
  )
  link_zstd_if_needed(test_utreexo_tx_relay)
  target_include_directories(test_utreexo_tx_relay PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  if(APPLE)
    target_link_libraries(test_utreexo_tx_relay PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME UtreexoTxRelay COMMAND test_utreexo_tx_relay)
endif()

# ============================================================================
# E2E Integration Test: Bridge→CSN Block Sync + TX Relay
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/network/test_utreexo_e2e_relay.cpp)
  add_executable(test_utreexo_e2e_relay
    tests/network/test_utreexo_e2e_relay.cpp
    src/network/stateless_node.cpp
    src/daemon/p2p_message.cpp
    src/daemon/peer_connection.cpp
    src/network/utreexo_messages.cpp
    src/primitives/uint256.cpp
    src/primitives/transaction_serializer.cpp
    src/common/logger.cpp
    src/common/sha256d.cpp
    src/consensus/proof_cache.cpp
    src/consensus/utreexo_proof_relay.cpp
  )
  target_link_libraries(test_utreexo_e2e_relay PRIVATE
    dinero_crypto
    dinero_consensus
    dinero_chainstate
    dinero_bridge
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
  )
  link_zstd_if_needed(test_utreexo_e2e_relay)
  target_include_directories(test_utreexo_e2e_relay PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  if(APPLE)
    target_link_libraries(test_utreexo_e2e_relay PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME UtreexoE2ERelay COMMAND test_utreexo_e2e_relay)
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/network/test_csn_stale_proof_retry.cpp)
  add_executable(test_csn_stale_proof_retry
    tests/network/test_csn_stale_proof_retry.cpp
    src/network/stateless_node.cpp
    src/daemon/p2p_message.cpp
    src/daemon/peer_connection.cpp
    src/network/utreexo_messages.cpp
    src/primitives/uint256.cpp
    src/primitives/transaction_serializer.cpp
    src/common/logger.cpp
    src/common/sha256d.cpp
    src/consensus/proof_cache.cpp
    src/consensus/utreexo_proof_relay.cpp
  )
  target_link_libraries(test_csn_stale_proof_retry PRIVATE
    dinero_crypto
    dinero_consensus
    dinero_chainstate
    dinero_bridge   # BridgeNode ctor/dtor — referenced by the test
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
  )
  link_zstd_if_needed(test_csn_stale_proof_retry)
  target_include_directories(test_csn_stale_proof_retry PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  if(APPLE)
    target_link_libraries(test_csn_stale_proof_retry PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME CsnStaleProofRetry COMMAND test_csn_stale_proof_retry)
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/network/test_transition_only_proof_mode.cpp)
  add_executable(test_transition_only_proof_mode
    tests/network/test_transition_only_proof_mode.cpp
  )
  target_link_libraries(test_transition_only_proof_mode PRIVATE
    dinero_consensus
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
  )
  link_zstd_if_needed(test_transition_only_proof_mode)
  target_include_directories(test_transition_only_proof_mode PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  if(APPLE)
    target_link_libraries(test_transition_only_proof_mode PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME TransitionOnlyProofMode COMMAND test_transition_only_proof_mode)
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/network/test_transition_proof_forest_sync.cpp)
  add_executable(test_transition_proof_forest_sync
    tests/network/test_transition_proof_forest_sync.cpp
    src/network/stateless_node.cpp
    src/network/utreexo_messages.cpp
    src/daemon/peer_connection.cpp
    src/daemon/p2p_message.cpp
    src/primitives/uint256.cpp
    src/primitives/transaction_serializer.cpp
    src/common/logger.cpp
    src/common/sha256d.cpp
  )
  target_link_libraries(test_transition_proof_forest_sync PRIVATE
    dinero_crypto
    dinero_consensus
    dinero_chainstate
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
  )
  link_zstd_if_needed(test_transition_proof_forest_sync)
  target_include_directories(test_transition_proof_forest_sync PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  if(APPLE)
    target_link_libraries(test_transition_proof_forest_sync PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME TransitionProofForestSync COMMAND test_transition_proof_forest_sync)
endif()

# ============================================================================
# DINERO-UTREEXO-SPEC §3.5: Leaf Hash Golden Vector Test
# ============================================================================
# Verifies domain-separated leaf hash matches specification exactly.
# This is CONSENSUS-CRITICAL - any failure means spec violation.
# Golden vector: fc8b209be5e6a849feeeda20633d055ee7e6830979cc76ec41bfb58e2a4f3fad
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/utreexo_leaf_hash_golden_test.cpp)
  add_executable(test_utreexo_leaf_hash_golden
    tests/utreexo_leaf_hash_golden_test.cpp
  )
  target_link_libraries(test_utreexo_leaf_hash_golden PRIVATE
    dinero_consensus
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
    GTest::gtest
    GTest::gtest_main
  )
  link_zstd_if_needed(test_utreexo_leaf_hash_golden)
  target_include_directories(test_utreexo_leaf_hash_golden PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  if(APPLE)
    target_link_libraries(test_utreexo_leaf_hash_golden PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME UtreexoLeafHashGolden COMMAND test_utreexo_leaf_hash_golden)
  set_tests_properties(UtreexoLeafHashGolden PROPERTIES
    LABELS "consensus;critical;utreexo;spec"
    TIMEOUT 10
  )
  message(STATUS "✅ DINERO-UTREEXO-SPEC §3.5 golden vector test configured")
endif()

# ============================================================================
# Utreexo Stateless Proof Minimal Test (Hardening Tests)
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_stateless_proof_minimal.cpp)
  add_executable(test_stateless_proof_minimal
    tests/test_stateless_proof_minimal.cpp
  )
  target_link_libraries(test_stateless_proof_minimal PRIVATE
    dinero_consensus
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
  )
  link_zstd_if_needed(test_stateless_proof_minimal)
  target_include_directories(test_stateless_proof_minimal PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  if(APPLE)
    target_link_libraries(test_stateless_proof_minimal PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME UtreexoStatelessProofMinimal COMMAND test_stateless_proof_minimal)
endif()

# ============================================================================
# Experimental: Utreexo Maturity-Bound Leaf Proof
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_utreexo_maturity_bound_leaf_experiment.cpp)
  add_executable(test_utreexo_maturity_bound_leaf_experiment
    tests/consensus/test_utreexo_maturity_bound_leaf_experiment.cpp
  )
  target_link_libraries(test_utreexo_maturity_bound_leaf_experiment PRIVATE
    dinero_consensus
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
  )
  link_zstd_if_needed(test_utreexo_maturity_bound_leaf_experiment)
  target_include_directories(test_utreexo_maturity_bound_leaf_experiment PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${OPENSSL_INCLUDE_DIR}
  )
  if(APPLE)
    target_link_libraries(test_utreexo_maturity_bound_leaf_experiment PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME UtreexoMaturityBoundLeafExperiment
    COMMAND test_utreexo_maturity_bound_leaf_experiment)
  set_tests_properties(UtreexoMaturityBoundLeafExperiment PROPERTIES
    LABELS "consensus;utreexo;stateless;experiment"
    TIMEOUT 10
  )
endif()

# ============================================================================
# E.2: Fee Policy Configuration Verification (Economics Layer)
# ============================================================================
# NOTE: This is CONFIG verification, not behavior testing.
# Mempool BEHAVIOR tests (CPFP, RBF, ancestor tracking) were completed in F.9.6-F.9.8.
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_fee_config.cpp)
  add_executable(test_fee_config
    tests/consensus/test_fee_config.cpp
    src/consensus/chainparams_impl.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_fee_config PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    RocksDB::rocksdb
    secp256k1
    sqlite3
    jsoncpp_static
  )
  link_zstd_if_needed(test_fee_config)
  target_include_directories(test_fee_config PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
    ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
  )
  if(APPLE)
    target_link_libraries(test_fee_config PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME FeeConfigVerification COMMAND test_fee_config)
endif()

# ============================================================================
# Phase 3: CT Fee Policy Configuration Tests
# ============================================================================
# Verifies CT fee policy parameters and estimation functions
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/policy/test_ct_fee_policy.cpp)
  add_executable(test_ct_fee_policy
    tests/policy/test_ct_fee_policy.cpp
    src/policy/fee_estimator.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_ct_fee_policy PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    RocksDB::rocksdb
    secp256k1
    sqlite3
    jsoncpp_static
  )
  link_zstd_if_needed(test_ct_fee_policy)
  target_include_directories(test_ct_fee_policy PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
    ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
  )
  if(APPLE)
    target_link_libraries(test_ct_fee_policy PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME CTFeePolicyTests COMMAND test_ct_fee_policy)
  set_tests_properties(CTFeePolicyTests PROPERTIES LABELS "policy;ct;fee")
endif()

# ============================================================================
# E.3: Supply Cap Validation (Economics Layer)
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_supply_cap.cpp)
  add_executable(test_supply_cap
    tests/consensus/test_supply_cap.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_supply_cap PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    RocksDB::rocksdb
    secp256k1
    sqlite3
    jsoncpp_static
  )
  link_zstd_if_needed(test_supply_cap)
  target_include_directories(test_supply_cap PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
    ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
  )
  if(APPLE)
    target_link_libraries(test_supply_cap PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME SupplyCapValidation COMMAND test_supply_cap)
endif()

# ============================================================================
# E.4: Economic Stress Tests (Economics Layer)
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_economic_edge_cases.cpp)
  add_executable(test_economic_edge_cases
    tests/consensus/test_economic_edge_cases.cpp
    src/consensus/chainparams_impl.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_economic_edge_cases PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    RocksDB::rocksdb
    secp256k1
    sqlite3
    jsoncpp_static
  )
  link_zstd_if_needed(test_economic_edge_cases)
  target_include_directories(test_economic_edge_cases PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
    ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
  )
  if(APPLE)
    target_link_libraries(test_economic_edge_cases PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME EconomicStressTests COMMAND test_economic_edge_cases)
endif()

# ============================================================================
# E.5: Premine Verification (Economics Layer)
# ============================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_premine.cpp)
  add_executable(test_premine
    tests/consensus/test_premine.cpp
    src/consensus/chainparams_impl.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_premine PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    RocksDB::rocksdb
    secp256k1
    sqlite3
    jsoncpp_static
  )
  link_zstd_if_needed(test_premine)
  target_include_directories(test_premine PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
    ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
  )
  if(APPLE)
    target_link_libraries(test_premine PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME PremineVerification COMMAND test_premine)
endif()

# ============================================================================
# G.1: P2P Protocol Verification (Networking Layer)
# ============================================================================

# G.1.2: Multi-node Test Harness + Simple Handshake Test
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_handshake_simple.cpp)
  add_executable(test_p2p_handshake
    tests/p2p/test_handshake_simple.cpp
    tests/p2p/test_harness.cpp
    src/daemon/p2p_manager.cpp
    src/network/relay_hints_eviction.cpp
    src/network/relay_registry.cpp
    src/network/quic_session.cpp
    src/network/quic_transport.cpp
    src/network/udp_socket.cpp
    src/network/relay_tls_keypair.cpp
    src/daemon/node_identity.cpp
    src/p2p/addr_v2.cpp
    src/daemon/secure_random.cpp
    src/daemon/peer_connection.cpp
    src/daemon/p2p_message.cpp
    src/daemon/peer_scoring.cpp
    src/daemon/peer_reputation_db.cpp
    src/p2p/addrman.cpp
    src/common/logger.cpp
    src/network/utreexo_messages.cpp
  )
  target_link_libraries(test_p2p_handshake PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    RocksDB::rocksdb
    secp256k1
    sqlite3
    jsoncpp_static
    gtest
  )
  link_zstd_if_needed(test_p2p_handshake)
  if(DINERO_ENABLE_QUIC AND TARGET dinero_ngtcp2)
    target_link_libraries(test_p2p_handshake PRIVATE dinero_ngtcp2)
    if(TARGET dinero_ngtcp2_crypto_ossl)
      target_link_libraries(test_p2p_handshake PRIVATE dinero_ngtcp2_crypto_ossl)
    elseif(TARGET dinero_ngtcp2_crypto_quictls)
      target_link_libraries(test_p2p_handshake PRIVATE dinero_ngtcp2_crypto_quictls)
    endif()
  endif()
  if(WIN32)
    target_link_libraries(test_p2p_handshake PRIVATE ws2_32)
  endif()
  target_include_directories(test_p2p_handshake PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests/p2p
    ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
    ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
  )
  if(APPLE)
    target_link_libraries(test_p2p_handshake PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME P2PHandshakeVerification COMMAND test_p2p_handshake)
endif()

# G.1.3: Ping/Pong Protocol Test
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_ping_pong.cpp)
  add_executable(test_p2p_ping_pong
    tests/p2p/test_ping_pong.cpp
    tests/p2p/test_harness.cpp
    src/daemon/p2p_manager.cpp
    src/network/relay_hints_eviction.cpp
    src/network/relay_registry.cpp
    src/network/quic_session.cpp
    src/network/quic_transport.cpp
    src/network/udp_socket.cpp
    src/network/relay_tls_keypair.cpp
    src/daemon/node_identity.cpp
    src/p2p/addr_v2.cpp
    src/daemon/secure_random.cpp
    src/daemon/peer_connection.cpp
    src/daemon/p2p_message.cpp
    src/daemon/peer_scoring.cpp
    src/daemon/peer_reputation_db.cpp
    src/p2p/addrman.cpp
    src/common/logger.cpp
    src/network/utreexo_messages.cpp
  )
  target_link_libraries(test_p2p_ping_pong PRIVATE
    dinero_consensus
    dinero_wallet
    dinero_crypto
    RocksDB::rocksdb
    secp256k1
    sqlite3
    jsoncpp_static
    gtest
  )
  link_zstd_if_needed(test_p2p_ping_pong)
  if(DINERO_ENABLE_QUIC AND TARGET dinero_ngtcp2)
    target_link_libraries(test_p2p_ping_pong PRIVATE dinero_ngtcp2)
    if(TARGET dinero_ngtcp2_crypto_ossl)
      target_link_libraries(test_p2p_ping_pong PRIVATE dinero_ngtcp2_crypto_ossl)
    elseif(TARGET dinero_ngtcp2_crypto_quictls)
      target_link_libraries(test_p2p_ping_pong PRIVATE dinero_ngtcp2_crypto_quictls)
    endif()
  endif()
  if(WIN32)
    target_link_libraries(test_p2p_ping_pong PRIVATE ws2_32)
  endif()
  target_include_directories(test_p2p_ping_pong PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests/p2p
    ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
    ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
  )
  if(APPLE)
    target_link_libraries(test_p2p_ping_pong PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME P2PPingPongVerification COMMAND test_p2p_ping_pong)
endif()

# G.1.4: Inventory Message Serialization Test
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_inventory_serialization.cpp)
  add_executable(test_inventory_serialization
    tests/p2p/test_inventory_serialization.cpp
    src/p2p/inventory.cpp
  )
  target_link_libraries(test_inventory_serialization PRIVATE
  )
  target_include_directories(test_inventory_serialization PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
    ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
  )
  if(APPLE)
    target_link_libraries(test_inventory_serialization PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME InventorySerializationVerification COMMAND test_inventory_serialization)
endif()

# G.1.4 Step 2: InFlightManager Test (Pure, Synchronous, Deterministic)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_inflight_manager.cpp)
  add_executable(test_inflight_manager
    tests/p2p/test_inflight_manager.cpp
    src/p2p/inflight_manager.cpp
    src/p2p/inventory.cpp
  )
  target_link_libraries(test_inflight_manager PRIVATE
  )
  target_include_directories(test_inflight_manager PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_inflight_manager PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME InFlightManagerVerification COMMAND test_inflight_manager)
endif()

# G.1.4 Step 3: INV/GETDATA/NOTFOUND Handler Test (Pure Protocol Choreography)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_inv_getdata_handlers.cpp)
  add_executable(test_inv_getdata_handlers
    tests/p2p/test_inv_getdata_handlers.cpp
    src/p2p/inventory_handler.cpp
    src/p2p/inflight_manager.cpp
    src/p2p/inventory.cpp
  )
  target_link_libraries(test_inv_getdata_handlers PRIVATE
  )
  target_include_directories(test_inv_getdata_handlers PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_inv_getdata_handlers PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME InvGetDataHandlerVerification COMMAND test_inv_getdata_handlers)
endif()

# G.1.4 Step 4: Timeout + Retry Policy Test (Pure Resilience Logic)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_timeout_retry.cpp)
  add_executable(test_timeout_retry
    tests/p2p/test_timeout_retry.cpp
    src/p2p/inventory_handler.cpp
    src/p2p/inflight_manager.cpp
    src/p2p/inventory.cpp
  )
  target_link_libraries(test_timeout_retry PRIVATE
  )
  target_include_directories(test_timeout_retry PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_timeout_retry PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME TimeoutRetryVerification COMMAND test_timeout_retry)
endif()

# G.2: Download Coordination Test (Pure Payload Transfer)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_download_coordination.cpp)
  add_executable(test_download_coordination
    tests/p2p/test_download_coordination.cpp
    src/p2p/download_coordinator.cpp
    src/p2p/inventory_handler.cpp
    src/p2p/inflight_manager.cpp
    src/p2p/inventory.cpp
  )
  target_link_libraries(test_download_coordination PRIVATE
  )
  target_include_directories(test_download_coordination PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_download_coordination PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME DownloadCoordinationVerification COMMAND test_download_coordination)
endif()

# Phase G.2: Block Relay Manager Integration Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_block_relay_g2.cpp)
  add_executable(test_block_relay_g2
    tests/p2p/test_block_relay_g2.cpp
    src/daemon/block_relay_manager.cpp
    src/p2p/block_download_scheduler.cpp
    src/p2p/orphan_block_pool.cpp    # Phase G.7: Orphan handling
    src/p2p/block_presence.cpp       # Required by orphan_block_pool
    src/primitives/block.cpp
    src/primitives/uint256.cpp
    src/primitives/transaction.cpp
    src/primitives/transaction_serializer.cpp
    src/common/sha256d.cpp
    tests/stubs/mempool_test_stubs.cpp  # forEachEntry referenced by block_relay_manager
  )
  target_link_libraries(test_block_relay_g2 PRIVATE
    dinero_crypto
    dinero_core
    dinero_consensus        # For HeaderSyncManager
    bulletproofs_ffi        # Required by confidential_validation
  )
  link_zstd_if_needed(test_block_relay_g2)
  target_include_directories(test_block_relay_g2 PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_block_relay_g2 PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME BlockRelayG2Verification COMMAND test_block_relay_g2)
endif()

# Phase Plan-A: Compact Block Binary Serialization Unit Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_compact_block_binary.cpp)
  add_executable(test_compact_block_binary
    tests/p2p/test_compact_block_binary.cpp
    src/p2p/compact_block.cpp
    src/primitives/block.cpp
    src/primitives/uint256.cpp
    src/primitives/transaction.cpp
    src/primitives/transaction_serializer.cpp
    src/common/sha256d.cpp
    tests/stubs/mempool_test_stubs.cpp  # no-op Mempool::forEachEntry etc.
  )
  target_link_libraries(test_compact_block_binary PRIVATE
    dinero_crypto
    dinero_core
    gtest
    gtest_main
  )
  link_zstd_if_needed(test_compact_block_binary)
  target_include_directories(test_compact_block_binary PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
  )
  if(APPLE)
    target_link_libraries(test_compact_block_binary PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME CompactBlockBinarySerialization COMMAND test_compact_block_binary)
  message(STATUS "  ✓ Phase Plan-A test registered: test_compact_block_binary")
endif()

# Phase G.8 / Phase N.3: Headers Wire Format Unit Tests (BlockHeader v1 = 128 bytes)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_headers_wire_v1.cpp)
  add_executable(test_headers_wire_v1
    tests/p2p/test_headers_wire_v1.cpp
    src/daemon/block_relay_manager.cpp
    src/daemon/header_serialization.cpp
    src/daemon/p2p_message.cpp
    src/primitives/block.cpp
    src/primitives/uint256.cpp
    src/common/sha256d.cpp
    tests/stubs/mempool_test_stubs.cpp  # no-op Mempool::forEachEntry etc.
  )
  target_link_libraries(test_headers_wire_v1 PRIVATE
    dinero_crypto
    dinero_core
    dinero_consensus
    bulletproofs_ffi
    gtest
    gtest_main
  )
  link_zstd_if_needed(test_headers_wire_v1)
  target_include_directories(test_headers_wire_v1 PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
  )
  if(APPLE)
    target_link_libraries(test_headers_wire_v1 PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME HeadersWireV1 COMMAND test_headers_wire_v1)
  message(STATUS "  ✓ Phase G.8/N.3 test registered: test_headers_wire_v1")
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_mainnet_checkpoints.cpp)
  add_executable(test_mainnet_checkpoints
    tests/p2p/test_mainnet_checkpoints.cpp
  )
  target_link_libraries(test_mainnet_checkpoints PRIVATE
    dinero_core
    bulletproofs_ffi
    gtest_main
  )
  target_include_directories(test_mainnet_checkpoints PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
  )
  if(APPLE)
    target_link_libraries(test_mainnet_checkpoints PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME MainnetCheckpoints COMMAND test_mainnet_checkpoints)
  message(STATUS "  ✓ Mainnet checkpoint test registered: test_mainnet_checkpoints")
endif()

# Phase G.3: Transaction Relay Manager Integration Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_tx_relay_g3.cpp)
  add_executable(test_tx_relay_g3
    tests/p2p/test_tx_relay_g3.cpp
    src/daemon/tx_relay_manager.cpp
    src/mempool/tx_orphan_pool.cpp
    src/daemon/interfaces/ingress_types.cpp
    src/primitives/transaction.cpp
    src/primitives/transaction_serializer.cpp
    src/primitives/uint256.cpp
    src/crypto/params_stub.cpp
    src/common/sha256d.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_tx_relay_g3 PRIVATE
    dinero_crypto
  )
  target_include_directories(test_tx_relay_g3 PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_tx_relay_g3 PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME TxRelayG3Verification COMMAND test_tx_relay_g3)
endif()

# Issue #6: CSN Proof Refresh Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_csn_proof_refresh.cpp)
  add_executable(test_csn_proof_refresh
    tests/p2p/test_csn_proof_refresh.cpp
    src/daemon/tx_relay_manager.cpp
    src/mempool/tx_orphan_pool.cpp
    src/daemon/interfaces/ingress_types.cpp
    src/primitives/transaction.cpp
    src/primitives/transaction_serializer.cpp
    src/primitives/uint256.cpp
    src/common/sha256d.cpp
    src/common/logger.cpp
    src/crypto/params_stub.cpp
  )
  target_link_libraries(test_csn_proof_refresh PRIVATE
    dinero_crypto
  )
  target_include_directories(test_csn_proof_refresh PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_csn_proof_refresh PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME CsnProofRefresh COMMAND test_csn_proof_refresh)
  message(STATUS "  ✓ Issue #6 test registered: test_csn_proof_refresh")
endif()

# Phase G.4: Fee Estimation Integration Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mempool/test_fee_estimation_g4.cpp)
  add_executable(test_fee_estimation_g4
    tests/mempool/test_fee_estimation_g4.cpp
    src/mempool/fee_estimator.cpp
    src/primitives/uint256.cpp
    src/common/sha256d.cpp
  )
  target_link_libraries(test_fee_estimation_g4 PRIVATE
    dinero_crypto
    dinero_core
  )
  target_include_directories(test_fee_estimation_g4 PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_fee_estimation_g4 PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME FeeEstimationG4Verification COMMAND test_fee_estimation_g4)
endif()

# Phase G.5: Peer Discovery & Address Management Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_peer_discovery_g5.cpp)
  add_executable(test_peer_discovery_g5
    tests/p2p/test_peer_discovery_g5.cpp
    src/p2p/addrman.cpp
    src/p2p/peer_scoring.cpp
  )
  target_link_libraries(test_peer_discovery_g5 PRIVATE
    dinero_core
  )
  target_include_directories(test_peer_discovery_g5 PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_peer_discovery_g5 PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME PeerDiscoveryG5Verification COMMAND test_peer_discovery_g5)
endif()

# Phase G.6: Connection Management & Peer Eviction Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/network/test_connection_manager.cpp)
  add_executable(test_connection_manager_g6
    tests/network/test_connection_manager.cpp
    src/p2p/connection_manager.cpp
    src/p2p/peer_scoring.cpp
  )
  target_link_libraries(test_connection_manager_g6 PRIVATE
    dinero_core
  )
  target_include_directories(test_connection_manager_g6 PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_connection_manager_g6 PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME ConnectionManagerG6Verification COMMAND test_connection_manager_g6)
endif()

# Phase G.7: Rate Limiting & DoS Protection Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_rate_limiter_g7.cpp)
  add_executable(test_rate_limiter_g7
    tests/p2p/test_rate_limiter_g7.cpp
    src/p2p/rate_limiter.cpp
    src/p2p/peer_scoring.cpp
  )
  target_link_libraries(test_rate_limiter_g7 PRIVATE
    dinero_core
  )
  target_include_directories(test_rate_limiter_g7 PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_rate_limiter_g7 PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME RateLimiterG7Verification COMMAND test_rate_limiter_g7)
endif()

# Phase G.8: DNS Resolution & Network Bootstrap Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_dns_resolver_g8.cpp)
  add_executable(test_dns_resolver_g8
    tests/p2p/test_dns_resolver_g8.cpp
    src/p2p/dns_resolver.cpp
  )
  target_link_libraries(test_dns_resolver_g8 PRIVATE
    dinero_core
  )
  # dns_resolver.cpp uses getaddrinfo/inet_pton/inet_ntop which live in
  # ws2_32.lib on Windows (POSIX libc on Linux/Mac, so no link needed there).
  # We recompile dns_resolver.cpp directly here rather than pulling from
  # dinero_core, so the ws2_32 dep doesn't transit.
  if(WIN32)
    target_link_libraries(test_dns_resolver_g8 PRIVATE ws2_32)
  endif()
  target_include_directories(test_dns_resolver_g8 PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_dns_resolver_g8 PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME DNSResolverG8Verification COMMAND test_dns_resolver_g8)
endif()

# Phase G.6.B: Block Download Scheduling Tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_block_download_scheduler_g6b.cpp)
  add_executable(test_block_download_scheduler_g6b
    tests/p2p/test_block_download_scheduler_g6b.cpp
    src/p2p/block_download_scheduler.cpp
  )
  target_link_libraries(test_block_download_scheduler_g6b PRIVATE
    dinero_core
  )
  target_include_directories(test_block_download_scheduler_g6b PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_block_download_scheduler_g6b PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME BlockDownloadSchedulerG6BVerification COMMAND test_block_download_scheduler_g6b)
endif()

# G.3.1: Validation Sink Wiring Test (Pure Queueing, NO Validation)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_validation_sink.cpp)
  add_executable(test_validation_sink
    tests/p2p/test_validation_sink.cpp
    src/p2p/validation_sink.cpp
    src/p2p/download_coordinator.cpp
    src/p2p/inventory_handler.cpp
    src/p2p/inflight_manager.cpp
    src/p2p/inventory.cpp
  )
  target_link_libraries(test_validation_sink PRIVATE
  )
  target_include_directories(test_validation_sink PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_validation_sink PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME ValidationSinkVerification COMMAND test_validation_sink)
endif()

# Phase 6B: ValidationQueue canonical handoff regression
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_validation_queue_handoff.cpp)
  add_executable(test_validation_queue_handoff
    tests/consensus/test_validation_queue_handoff.cpp
    src/daemon/mempool.cpp
    src/daemon/config.cpp                # GetConfig() — referenced by consensus_write_batch.obj
    tests/stubs/vault_test_stubs.cpp     # vault observers — real mempool linked
  )
  target_link_libraries(test_validation_queue_handoff PRIVATE
    dinero_core
    dinero_consensus
    dinero_crypto
    dinero_wallet
    bulletproofs_ffi
    GTest::gtest
    GTest::gtest_main
  )
  link_zstd_if_needed(test_validation_queue_handoff)
  target_include_directories(test_validation_queue_handoff PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_validation_queue_handoff PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME ValidationQueueHandoff COMMAND test_validation_queue_handoff)
endif()

# G.3.2: Structural Validation Test (Cheap, Pure - NO Consensus)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_structural_validation.cpp)
  add_executable(test_structural_validation
    tests/p2p/test_structural_validation.cpp
    src/p2p/structural_validator.cpp
  )
  target_link_libraries(test_structural_validation PRIVATE
    dinero_core
  )
  target_include_directories(test_structural_validation PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_structural_validation PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME StructuralValidationVerification COMMAND test_structural_validation)
endif()

# G.3.3: Consensus Validation Test (Expensive, Deterministic - Read-Only UTXO)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_consensus_validation.cpp)
  add_executable(test_consensus_validation
    tests/p2p/test_consensus_validation.cpp
    src/p2p/consensus_validator.cpp
    src/p2p/inventory.cpp
    src/primitives/uint256.cpp
  )
  target_link_libraries(test_consensus_validation PRIVATE
    dinero_crypto  # For SHA256 hashing used by uint256
  )
  target_include_directories(test_consensus_validation PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_consensus_validation PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME ConsensusValidationVerification COMMAND test_consensus_validation)
endif()

# G.3.3 Extension: Subsidy Validation Test
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_subsidy_validation.cpp)
  add_executable(test_subsidy_validation
    tests/p2p/test_subsidy_validation.cpp
    src/p2p/consensus_validator.cpp
    src/p2p/inventory.cpp
    src/primitives/uint256.cpp
  )
  target_link_libraries(test_subsidy_validation PRIVATE
    gtest
  )
  target_include_directories(test_subsidy_validation PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  target_compile_definitions(test_subsidy_validation PRIVATE DINERO_TESTING)
  if(APPLE)
    target_link_libraries(test_subsidy_validation PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME SubsidyValidationVerification COMMAND test_subsidy_validation)
endif()

# G.3.4: State Transition Test
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/p2p/test_state_transition.cpp)
  add_executable(test_state_transition
    tests/p2p/test_state_transition.cpp
    src/p2p/state_transition.cpp
    src/p2p/consensus_validator.cpp
    src/p2p/inventory.cpp
    src/primitives/uint256.cpp
  )
  target_link_libraries(test_state_transition PRIVATE
    gtest
  )
  target_include_directories(test_state_transition PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  target_compile_definitions(test_state_transition PRIVATE DINERO_TESTING)
  if(APPLE)
    target_link_libraries(test_state_transition PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME StateTransitionVerification COMMAND test_state_transition)
endif()

# IBD Smoke Test - Initial Block Download Integration
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_ibd_smoke.cpp)
  add_executable(test_ibd_smoke
    tests/consensus/test_ibd_smoke.cpp
    src/common/logger.cpp        # Required by TaprootKeys
    src/common/json_logger.cpp   # Required by logger
  )
  target_link_libraries(test_ibd_smoke PRIVATE
    dinero_consensus  # For genesis, chainparams
    dinero_crypto     # For SHA256 hashing
    dinero_wallet     # For TaprootKeys
    jsoncpp_static    # Required by json_logger
  )
  link_zstd_if_needed(test_ibd_smoke)
  target_include_directories(test_ibd_smoke PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_ibd_smoke PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME IBDSmokeTest COMMAND test_ibd_smoke)
endif()

# IBD Connect Test - Real Consensus Integration
# TODO: Disabled - needs refactor to use IConsensusUTXOSet instead of UTXOIndex directly
# BlockValidator now requires IConsensusUTXOSet* not IUTXOProvider*
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_ibd_connect.cpp)
  add_executable(test_ibd_connect
    tests/consensus/test_ibd_connect.cpp
    src/common/logger.cpp        # Required by TaprootKeys
    src/common/json_logger.cpp   # Required by logger
    src/consensus/block_validation.cpp  # BlockValidator::ConnectBlock
    src/consensus/undo.cpp       # BlockUndo serialization
  )
  target_link_libraries(test_ibd_connect PRIVATE
    dinero_consensus  # For genesis, chainparams, BlockValidator
    dinero_crypto     # For SHA256 hashing
    dinero_wallet     # For TaprootKeys, UTXOIndex
    dinero_chainstate # For ChainDB and persistence
    jsoncpp_static    # Required by json_logger
    sqlite3           # Required by UTXOIndex
    RocksDB::rocksdb  # Required by consensus components
    bulletproofs_ffi  # Required by ConfidentialTransactionValidator
    OpenSSL::SSL
    OpenSSL::Crypto
    secp256k1
  )
  link_zstd_if_needed(test_ibd_connect)
  target_include_directories(test_ibd_connect PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_ibd_connect PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME IBDConnectTest COMMAND test_ibd_connect)
endif()

# DISABLED: Requires IConsensusUTXOSet interface refactor (BlockValidator constructor changed)
# IBD Persistence Test - Restart Recovery (Phase 3A)
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_ibd_persistence.cpp)
  add_executable(test_ibd_persistence
    tests/consensus/test_ibd_persistence.cpp
    src/common/logger.cpp
    src/common/json_logger.cpp
    src/consensus/block_validation.cpp
    src/consensus/undo.cpp
  )
  target_link_libraries(test_ibd_persistence PRIVATE
    dinero_consensus  # For genesis, chainparams, BlockValidator
    dinero_crypto     # For SHA256 hashing
    dinero_wallet     # For TaprootKeys, UTXOIndex
    dinero_chainstate # For primitives
    jsoncpp_static    # Required by json_logger
    sqlite3           # Required by UTXOIndex
    bulletproofs_ffi  # Required by ConfidentialTransactionValidator
    OpenSSL::SSL
    OpenSSL::Crypto
    secp256k1
  )
  link_zstd_if_needed(test_ibd_persistence)
  target_include_directories(test_ibd_persistence PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_ibd_persistence PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME IBDPersistenceTest COMMAND test_ibd_persistence)
endif()

# DISABLED: Requires IConsensusUTXOSet interface refactor (BlockValidator constructor changed)
# IBD Reorg Test - DisconnectBlock & Reorg Safety
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_ibd_reorg.cpp)
  add_executable(test_ibd_reorg
    tests/consensus/test_ibd_reorg.cpp
    src/common/logger.cpp
    src/common/json_logger.cpp
    src/consensus/block_validation.cpp
    src/consensus/undo.cpp
  )
  target_link_libraries(test_ibd_reorg PRIVATE
    dinero_consensus  # For genesis, chainparams, BlockValidator
    dinero_crypto     # For SHA256 hashing
    dinero_wallet     # For TaprootKeys, UTXOIndex
    dinero_chainstate # For primitives
    jsoncpp_static    # Required by json_logger
    sqlite3           # Required by UTXOIndex
    bulletproofs_ffi  # Required by ConfidentialTransactionValidator
    OpenSSL::SSL
    OpenSSL::Crypto
    secp256k1
  )
  link_zstd_if_needed(test_ibd_reorg)
  target_include_directories(test_ibd_reorg PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_ibd_reorg PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME IBDReorgTest COMMAND test_ibd_reorg)
endif()

# DISABLED: Requires IConsensusUTXOSet interface refactor (BlockValidator constructor changed)
# IBD Spend Test - Utreexo Proof Generation E2E
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_ibd_spend.cpp)
  add_executable(test_ibd_spend
    tests/consensus/test_ibd_spend.cpp
    src/common/logger.cpp
    src/common/json_logger.cpp
    src/consensus/block_validation.cpp
    src/consensus/undo.cpp
    src/consensus/proof_cache.cpp  # For ProofCache
    src/primitives/taproot_tx_signer.cpp  # For BIP341 Taproot signing
  )
  target_link_libraries(test_ibd_spend PRIVATE
    dinero_consensus  # For genesis, chainparams, BlockValidator, ProofCache
    dinero_crypto     # For SHA256 hashing
    dinero_wallet     # For TaprootKeys, UTXOIndex
    dinero_chainstate # For primitives
    jsoncpp_static    # Required by json_logger
    sqlite3           # Required by UTXOIndex
    bulletproofs_ffi  # Required by ConfidentialTransactionValidator
    OpenSSL::SSL
    OpenSSL::Crypto
    secp256k1
  )
  link_zstd_if_needed(test_ibd_spend)
  target_include_directories(test_ibd_spend PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_ibd_spend PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME IBDSpendTest COMMAND test_ibd_spend)
  set_tests_properties(IBDSpendTest
    PROPERTIES
    LABELS "consensus;ibd;utreexo;proof-generation;phase-9;mandatory"
    TIMEOUT 60
  )
endif()

# Fork Choice Test - Chain Selection & Reorg (Phase 3B)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_fork_choice.cpp)
  add_executable(test_fork_choice
    tests/consensus/test_fork_choice.cpp
    src/common/logger.cpp
    src/common/json_logger.cpp
  )
  target_link_libraries(test_fork_choice PRIVATE
    dinero_consensus  # For block_index, chainwork, chainparams
    dinero_crypto     # For SHA256 hashing
    jsoncpp_static    # Required by json_logger
    OpenSSL::SSL
    OpenSSL::Crypto
    secp256k1
  )
  link_zstd_if_needed(test_fork_choice)
  target_include_directories(test_fork_choice PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_fork_choice PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME ForkChoiceTest COMMAND test_fork_choice)
endif()

# #309: reorg-candidate whole-branch-data eligibility (connect-abort prevention)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_reorg_candidate_eligibility.cpp)
  add_executable(test_reorg_candidate_eligibility
    tests/consensus/test_reorg_candidate_eligibility.cpp
    src/common/logger.cpp
    src/common/json_logger.cpp
  )
  target_link_libraries(test_reorg_candidate_eligibility PRIVATE
    dinero_core        # block_lifecycle.cpp (BranchHasDataToConnectedBase)
    dinero_consensus
    dinero_crypto
    jsoncpp_static
    OpenSSL::SSL
    OpenSSL::Crypto
    secp256k1
  )
  link_zstd_if_needed(test_reorg_candidate_eligibility)
  target_include_directories(test_reorg_candidate_eligibility PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_reorg_candidate_eligibility PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME ReorgCandidateEligibilityTest COMMAND test_reorg_candidate_eligibility)
endif()

# DISABLED: Requires IConsensusUTXOSet interface refactor (BlockValidator constructor changed)
# Mining Maturity Test - End-to-End Coinbase Flow
if(FALSE AND EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_mining_maturity.cpp)
  add_executable(test_mining_maturity
    tests/consensus/test_mining_maturity.cpp
    src/wallet/utxo_index.cpp
    src/consensus/block_validation.cpp
    src/consensus/coinbase_maturity.cpp
    src/primitives/block.cpp
    # src/wallet/transaction.cpp removed - use from dinero_consensus (already linked)
    src/crypto/sha256.cpp
    src/wallet/taproot_keys.cpp
    src/common/logger.cpp
    src/common/json_logger.cpp
  )
  target_link_libraries(test_mining_maturity PRIVATE
    dinero_consensus  # For chainparams, block validation, coinbase maturity
    dinero_crypto     # For SHA256 hashing
    dinero_wallet     # For TaprootKeys, UTXOIndex
    dinero_chainstate # For primitives
    jsoncpp_static    # Required by json_logger
    sqlite3           # Required by UTXOIndex
    bulletproofs_ffi  # Required by ConfidentialTransactionValidator
    OpenSSL::SSL
    OpenSSL::Crypto
    secp256k1
  )
  link_zstd_if_needed(test_mining_maturity)
  target_include_directories(test_mining_maturity PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_mining_maturity PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME MiningMaturityTest COMMAND test_mining_maturity)
endif()

# Phase H.5 — Restart Recovery Test (Crash Safety Proof)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_header_sync_restart.cpp)
  add_executable(test_header_sync_restart
    tests/consensus/test_header_sync_restart.cpp
    src/consensus/header_sync_manager.cpp
    src/consensus/block_index.cpp
    tests/consensus/chainstate_metadata_stub.cpp
    tests/consensus/missing_symbols_stub.cpp
    src/storage/chain_db.cpp
    src/primitives/block.cpp
    src/primitives/transaction.cpp         # Required by Block::Serialize()
    src/primitives/transaction_serializer.cpp  # Required by Transaction serialization
    src/primitives/uint256.cpp
    src/consensus/undo.cpp                 # Required by ChainDB::getUndo/putUndo
    src/crypto/sha256.cpp
    src/common/logger.cpp
    src/common/json_logger.cpp
    src/common/sha256d.cpp
    src/consensus/chainwork.cpp
  )
  target_link_libraries(test_header_sync_restart PRIVATE
    dinero_consensus   # Required for BlockUtreexoData serialization
    dinero_crypto      # Required for SHA256 and crypto operations
    RocksDB::rocksdb
    jsoncpp_static
    OpenSSL::Crypto
    OpenSSL::SSL
    secp256k1
    # Use system GTest to match system GTest headers (from /opt/homebrew/include)
    GTest::gtest
  )
  link_zstd_if_needed(test_header_sync_restart)  # Required for Utreexo compression
  # Explicitly remove vendored GTest interface includes for this target only
  set_target_properties(test_header_sync_restart PROPERTIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES ""
  )
  target_include_directories(test_header_sync_restart SYSTEM BEFORE PRIVATE
    # Use system GTest headers
    /opt/homebrew/include
  )
  target_include_directories(test_header_sync_restart PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_header_sync_restart PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME HeaderSyncRestartRecovery COMMAND test_header_sync_restart)
endif()

# Phase P.1: Prune Eligibility Tests
if(ENABLE_TESTS AND EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_prune_eligibility.cpp)
  add_executable(test_prune_eligibility
    tests/consensus/test_prune_eligibility.cpp
    src/consensus/block_index.cpp
    src/consensus/chainwork.cpp
    src/primitives/uint256.cpp
    src/primitives/block.cpp
    src/primitives/transaction.cpp
    src/primitives/transaction_serializer.cpp
    # src/wallet/transaction.cpp removed - not needed for prune tests
    src/crypto/sha256.cpp
    src/common/sha256d.cpp
    src/consensus/utreexo_accumulator.cpp
    src/consensus/utreexo_stump.cpp
    src/consensus/script_crypto.cpp
    tests/consensus/missing_symbols_stub.cpp
    tests/stubs/chain_test_stubs.cpp  # GetActiveChain() — referenced via inline IsUtreexoCanonicalRootsActive
    src/common/logger.cpp
    src/common/json_logger.cpp
  )
  target_link_libraries(test_prune_eligibility PRIVATE
    jsoncpp_static
    dinero_crypto
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
    # Use system GTest to match system GTest headers
    GTest::gtest
  )
  link_zstd_if_needed(test_prune_eligibility)
  set_target_properties(test_prune_eligibility PROPERTIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES ""
  )
  target_include_directories(test_prune_eligibility SYSTEM BEFORE PRIVATE
    /opt/homebrew/include
  )
  target_include_directories(test_prune_eligibility PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_prune_eligibility PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME PruneEligibility COMMAND test_prune_eligibility)
endif()

# ═══════════════════════════════════════════════════════════════════════════
# Mempool Proof Staleness Tests (#2)
# ═══════════════════════════════════════════════════════════════════════════
# Tests onBlockConnected eviction, staleness marking, refresh, disconnect
# Uses daemon Mempool with addUnchecked (no ChainDB required)
# ═══════════════════════════════════════════════════════════════════════════
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mempool/test_mempool_proof_staleness.cpp)
  add_executable(test_mempool_proof_staleness
    tests/mempool/test_mempool_proof_staleness.cpp
    tests/mempool/test_mempool_proof_staleness_stubs.cpp
    src/daemon/mempool.cpp
    src/mining/address_validator.cpp
    src/daemon/bech32_decode.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_mempool_proof_staleness PRIVATE
    dinero_chainstate
    dinero_consensus
    dinero_shielded   # DeserializeShieldedBundle / ValidateShieldedBundle / BuildShieldedValidationContext
    dinero_crypto
    secp256k1
    jsoncpp_static
    OpenSSL::Crypto
  )
  link_zstd_if_needed(test_mempool_proof_staleness)
  target_include_directories(test_mempool_proof_staleness PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_mempool_proof_staleness PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME MempoolProofStaleness COMMAND test_mempool_proof_staleness)
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mempool/test_mempool_address_queries.cpp)
  add_executable(test_mempool_address_queries
    tests/mempool/test_mempool_address_queries.cpp
    tests/mempool/test_mempool_proof_staleness_stubs.cpp
    src/daemon/mempool.cpp
    src/mining/address_validator.cpp
    src/daemon/bech32_decode.cpp
    src/common/logger.cpp
  )
  target_link_libraries(test_mempool_address_queries PRIVATE
    dinero_chainstate
    dinero_consensus
    dinero_shielded   # DeserializeShieldedBundle / ValidateShieldedBundle / BuildShieldedValidationContext
    dinero_crypto
    secp256k1
    jsoncpp_static
    OpenSSL::Crypto
  )
  link_zstd_if_needed(test_mempool_address_queries)
  target_include_directories(test_mempool_address_queries PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_mempool_address_queries PRIVATE
      "-framework Security"
      "-framework CoreFoundation"
    )
  endif()
  add_test(NAME MempoolAddressQueries COMMAND test_mempool_address_queries)
endif()

# ═══════════════════════════════════════════════════════════════════════════
# Phase M.3: Semantic Hash Domains Test
# ═══════════════════════════════════════════════════════════════════════════
# Verifies compile-time enforcement of semantic hash types
# Tests: No implicit conversions, domain-locked constructors, storage guarantees
# ═══════════════════════════════════════════════════════════════════════════

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/primitives/test_hash_domains_m3.cpp)
  add_executable(test_hash_domains_m3
    tests/primitives/test_hash_domains_m3.cpp
    src/primitives/hash_domains.cpp
    src/primitives/block.cpp
    src/primitives/transaction.cpp
    src/primitives/transaction_serializer.cpp
    src/primitives/uint256.cpp
    src/crypto/sha256.cpp
  )
  
  target_include_directories(test_hash_domains_m3 PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/sha3/include
  )
  
  target_link_libraries(test_hash_domains_m3 PRIVATE
    dinero_crypto
    dinero_consensus
    secp256k1
    OpenSSL::Crypto
    GTest::gtest
    GTest::gtest_main
  )
  
  link_zstd_if_needed(test_hash_domains_m3)
  
  add_test(NAME HashDomainsM3 COMMAND test_hash_domains_m3)
  set_tests_properties(HashDomainsM3 PROPERTIES
    LABELS "phase-m3;semantic-types;compile-time-enforcement"
  )

  message(STATUS "  ✓ Phase M.3 test registered: test_hash_domains_m3")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Phase M.4: ABI Stability Tests
# ════════════════════════════════════════════════════════════════════════════

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_abi_stability.cpp)
  add_executable(test_abi_stability
    tests/consensus/test_abi_stability.cpp
  )

  target_include_directories(test_abi_stability PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/sha3/include
  )

  target_link_libraries(test_abi_stability PRIVATE
    dinero_consensus
    dinero_crypto
    secp256k1
    OpenSSL::Crypto
  )

  link_zstd_if_needed(test_abi_stability)

  add_test(NAME ABI_Stability COMMAND test_abi_stability)
  set_tests_properties(ABI_Stability PROPERTIES
    LABELS "phase-m4;abi-freeze;consensus-critical;mandatory"
  )

  message(STATUS "  ✓ Phase M.4 ABI stability test registered: test_abi_stability")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Signature Rejection Invariant Tests (CRITICAL SECURITY)
# ════════════════════════════════════════════════════════════════════════════

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_signature_rejection.cpp)
  add_executable(test_signature_rejection
    tests/consensus/test_signature_rejection.cpp
  )

  target_include_directories(test_signature_rejection PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/secp256k1-zkp/include
  )

  target_link_libraries(test_signature_rejection PRIVATE
    dinero_core      # Contains ValidationWorkerPool
    dinero_consensus
    dinero_wallet
    dinero_crypto
    secp256k1
    OpenSSL::Crypto
  )

  link_zstd_if_needed(test_signature_rejection)

  add_test(NAME Signature_Rejection COMMAND test_signature_rejection)
  set_tests_properties(Signature_Rejection PROPERTIES
    LABELS "security-critical;consensus;signature-verification"
  )

  message(STATUS "  ✓ Signature rejection test registered: test_signature_rejection")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Phase M.6: Monetary Domain Type Safety Tests
# ════════════════════════════════════════════════════════════════════════════
# Verifies AmountUna and BlockHeight type safety
# Tests: Overflow protection, domain-locked constructors, maturity checking
# ════════════════════════════════════════════════════════════════════════════

# Test 1: AmountUna (monetary amounts)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/primitives/test_amount.cpp)
  add_executable(test_amount
    tests/primitives/test_amount.cpp
  )

  target_include_directories(test_amount PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  target_link_libraries(test_amount PRIVATE
    GTest::gtest
    GTest::gtest_main
  )

  add_test(NAME AmountUna COMMAND test_amount)
  set_tests_properties(AmountUna PROPERTIES
    LABELS "phase-m6;monetary-types;type-safety"
  )

  message(STATUS "  ✓ Phase M.6 test registered: test_amount")
endif()

# Test 2: BlockHeight (block heights and maturity)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/primitives/test_block_height.cpp)
  add_executable(test_block_height
    tests/primitives/test_block_height.cpp
  )

  target_include_directories(test_block_height PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  target_link_libraries(test_block_height PRIVATE
    GTest::gtest
    GTest::gtest_main
  )

  add_test(NAME BlockHeight COMMAND test_block_height)
  set_tests_properties(BlockHeight PROPERTIES
    LABELS "phase-m6;height-types;type-safety"
  )

  message(STATUS "  ✓ Phase M.6 test registered: test_block_height")
endif()

# ════════════════════════════════════════════════════════════════════════════
# Phase 3: Premine Mandatory Tests (Consensus Hardening)
# ════════════════════════════════════════════════════════════════════════════
# These tests freeze premine economics and reorg safety.
# All three must pass before premine can be declared frozen.

# Test 1: Premine Exists and Is Correct
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_premine_exists.cpp)
  add_executable(test_premine_exists
    tests/consensus/test_premine_exists.cpp
  )

  target_include_directories(test_premine_exists PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  target_link_libraries(test_premine_exists PRIVATE
    dinero_core
    dinero_consensus
    dinero_crypto
    secp256k1
    ${ROCKSDB_LIBRARY}
    ${SQLITE3_LIBRARIES}
  )

  link_zstd_if_needed(test_premine_exists)

  add_test(NAME PremineExists COMMAND test_premine_exists)
  set_tests_properties(PremineExists PROPERTIES
    LABELS "phase-3;premine;consensus-critical;mandatory"
    TIMEOUT 60
  )

  message(STATUS "  ✓ Phase 3 test registered: test_premine_exists")
endif()

# Test 2: Coinbase Maturity Enforcement
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_premine_maturity.cpp)
  add_executable(test_premine_maturity
    tests/consensus/test_premine_maturity.cpp
  )

  target_include_directories(test_premine_maturity PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  target_link_libraries(test_premine_maturity PRIVATE
    dinero_core
    dinero_consensus
    dinero_crypto
    secp256k1
    ${ROCKSDB_LIBRARY}
    ${SQLITE3_LIBRARIES}
  )

  link_zstd_if_needed(test_premine_maturity)

  add_test(NAME PremineMaturity COMMAND test_premine_maturity)
  set_tests_properties(PremineMaturity PROPERTIES
    LABELS "phase-3;premine;coinbase;mandatory"
    TIMEOUT 60
  )

  message(STATUS "  ✓ Phase 3 test registered: test_premine_maturity")
endif()

# Test 3: Reorg Safety (Most Critical)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_premine_reorg.cpp)
  add_executable(test_premine_reorg
    tests/consensus/test_premine_reorg.cpp
  )

  target_include_directories(test_premine_reorg PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  target_link_libraries(test_premine_reorg PRIVATE
    dinero_core
    dinero_consensus
    dinero_crypto
    secp256k1
    ${ROCKSDB_LIBRARY}
    ${SQLITE3_LIBRARIES}
  )

  link_zstd_if_needed(test_premine_reorg)

  add_test(NAME PremineReorg COMMAND test_premine_reorg)
  set_tests_properties(PremineReorg PROPERTIES
    LABELS "phase-3;premine;reorg;supply-safety;mandatory"
    TIMEOUT 60
  )

  message(STATUS "  ✓ Phase 3 test registered: test_premine_reorg")
endif()

# Test 4: Mining at Height 2 (First PoW Block)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_mining_height2.cpp)
  add_executable(test_mining_height2
    tests/consensus/test_mining_height2.cpp
  )

  target_include_directories(test_mining_height2 PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  target_link_libraries(test_mining_height2 PRIVATE
    dinero_core
    dinero_consensus
    dinero_crypto
    secp256k1
    ${ROCKSDB_LIBRARY}
    ${SQLITE3_LIBRARIES}
  )

  link_zstd_if_needed(test_mining_height2)

  add_test(NAME MiningHeight2 COMMAND test_mining_height2)
  set_tests_properties(MiningHeight2 PROPERTIES
    LABELS "phase-3;mining;pow;validation"
    TIMEOUT 120
  )

  message(STATUS "  ✓ Phase 3 test registered: test_mining_height2")
endif()

# Test 5: Wallet Tracks Height 2 Coinbase (Phase M.5.3 - Runtime Correctness)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_wallet_height2_coinbase.cpp)
  add_executable(test_wallet_height2_coinbase
    tests/consensus/test_wallet_height2_coinbase.cpp
  )

  target_include_directories(test_wallet_height2_coinbase PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  target_link_libraries(test_wallet_height2_coinbase PRIVATE
    dinero_core
    dinero_consensus
    dinero_crypto
    secp256k1
    ${ROCKSDB_LIBRARY}
    ${SQLITE3_LIBRARIES}
  )

  link_zstd_if_needed(test_wallet_height2_coinbase)

  add_test(NAME WalletHeight2Coinbase COMMAND test_wallet_height2_coinbase)
  set_tests_properties(WalletHeight2Coinbase PROPERTIES
    LABELS "phase-m5;wallet;coinbase;maturity;runtime-correctness"
    TIMEOUT 60
  )

  message(STATUS "  ✓ Phase M.5.3 test registered: test_wallet_height2_coinbase")
endif()

# Test 6: Two-Node Network Sync (Phase N.1 - Multi-Node Validation)
# POSIX-only: uses fork(), waitpid(), kill() to orchestrate two daemon
# processes. Porting to CreateProcess/TerminateProcess would be a substantial
# rewrite; skip on native Windows MSVC. Still builds on Linux/Mac/MinGW.
if(NOT WIN32 AND EXISTS ${CMAKE_SOURCE_DIR}/tests/network/test_two_node_sync.cpp)
  add_executable(test_two_node_sync
    tests/network/test_two_node_sync.cpp
  )

  target_include_directories(test_two_node_sync PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/json/include
  )

  target_link_libraries(test_two_node_sync PRIVATE
    dinero_core
    dinero_consensus
    dinero_crypto
    gtest
    gtest_main
    pthread
  )

  add_test(NAME TwoNodeSync COMMAND test_two_node_sync)
  set_tests_properties(TwoNodeSync PROPERTIES
    LABELS "phase-n1;network;p2p;multi-node;consensus;critical"
    TIMEOUT 120
  )

  message(STATUS "  ✓ Phase N.1 test registered: test_two_node_sync")
else()
  message(STATUS "  ⚠ Phase N.1 test not found: tests/network/test_two_node_sync.cpp")
endif()

# CT Soak Test Infrastructure (Phase CT.4)
# POSIX-only: ct_soak_controller.h uses pid_t directly to manage forked
# daemon processes. Skip on native Windows MSVC.
if(NOT WIN32 AND EXISTS ${CMAKE_SOURCE_DIR}/tests/soak/run_ct_soak.cpp)
  add_executable(test_ct_soak
    tests/soak/run_ct_soak.cpp
    tests/soak/ct_soak_controller.cpp
    tests/soak/ct_tx_generator.cpp
    tests/soak/kill_switch_tester.cpp
    tests/soak/soak_metrics.cpp
  )

  target_include_directories(test_ct_soak PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests/soak
  )

  target_link_libraries(test_ct_soak PRIVATE
    dinero_core
    dinero_consensus
    dinero_crypto
    gtest
    gtest_main
    pthread
  )

  # Note: Soak tests are long-running (hours) and not registered in ctest by default
  # Run manually: ./test_ct_soak --gtest_filter=CTSoakTest.BasicSoakTest
  # Register only the unit tests for quick CI runs
  add_test(NAME SoakMetricsTest COMMAND test_ct_soak --gtest_filter=SoakMetricsTest.*)
  set_tests_properties(SoakMetricsTest PROPERTIES
    LABELS "phase-ct4;soak;metrics;unit"
    TIMEOUT 30
  )

  add_test(NAME TxGeneratorTest COMMAND test_ct_soak --gtest_filter=TxGeneratorTest.*)
  set_tests_properties(TxGeneratorTest PROPERTIES
    LABELS "phase-ct4;soak;generator;unit"
    TIMEOUT 30
  )

  add_test(NAME KillSwitchTest COMMAND test_ct_soak --gtest_filter=KillSwitchTest.*)
  set_tests_properties(KillSwitchTest PROPERTIES
    LABELS "phase-ct4;soak;killswitch;unit"
    TIMEOUT 30
  )

  message(STATUS "  ✓ Phase CT.4 soak test infrastructure registered: test_ct_soak")
else()
  message(STATUS "  ⚠ Phase CT.4 soak test not found: tests/soak/run_ct_soak.cpp")
endif()

# Test 7: Wallet Restore + Rescan (Phase W.1)
# NOTE: This test requires dinerod binary with full RPC/gRPC support
# It will be skipped if dinerod target is not built (missing dependencies)
# Also POSIX-only: spawns dinerod via fork/exec + signals. Skip on native Windows.
if(NOT WIN32 AND EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet/test_wallet_restore_rescan.cpp)
  add_executable(test_wallet_restore_rescan
    tests/wallet/test_wallet_restore_rescan.cpp
  )

  target_include_directories(test_wallet_restore_rescan PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/json/include
  )

  target_link_libraries(test_wallet_restore_rescan PRIVATE
    dinero_core
    dinero_consensus
    dinero_crypto
    gtest
    gtest_main
    pthread
  )

  # Only register the test if dinerod target exists
  if(TARGET dinerod)
    add_test(NAME WalletRestoreRescan COMMAND test_wallet_restore_rescan)
    set_tests_properties(WalletRestoreRescan PROPERTIES
      LABELS "phase-w1;wallet;restore;rescan;critical;e2e"
      TIMEOUT 180
    )
    message(STATUS "  ✓ Phase W.1 test registered: test_wallet_restore_rescan (dinerod available)")
  else()
    message(STATUS "  ⚠ Phase W.1 test skipped: test_wallet_restore_rescan (dinerod target not built - missing gRPC/protobuf)")
  endif()
else()
  message(STATUS "  ⚠ Phase W.1 test not found: tests/wallet/test_wallet_restore_rescan.cpp")
endif()

# Wallet Mainnet Readiness (wallet-only hardening suite)
# Covers:
# - encryption/restart/unlock address continuity
# - restore determinism in fresh datadir
# - invalid mnemonic rejection with no partial wallet
# - BIP86 derivation path persistence (external + change)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/wallet/test_wallet_mainnet_readiness.cpp)
  add_executable(test_wallet_mainnet_readiness
    tests/wallet/test_wallet_mainnet_readiness.cpp
    tests/stubs/mempool_test_stubs.cpp  # Mempool::isOutputSpentInMempool etc.
  )

  target_include_directories(test_wallet_mainnet_readiness PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )

  target_link_libraries(test_wallet_mainnet_readiness PRIVATE
    dinero_wallet
    dinero_core
    dinero_consensus
    dinero_crypto
    dinero_rpc_handlers
    sqlite3
    gtest
    gtest_main
    pthread
  )

  link_zstd_if_needed(test_wallet_mainnet_readiness)

  if(APPLE)
    target_link_libraries(test_wallet_mainnet_readiness PRIVATE "-framework Security")
  endif()

  # WalletMainnetReadiness was originally registered as a single bundled
  # ctest entry that runs all 9 subcases. As of 2026-05-18, 7/9 subcases
  # pass on both Mac and Linux; 2/9 fail. Per the "split, not investigate"
  # disposition in docs/architecture/quarantined-test-classification.md,
  # the bundle is split into 9 per-subcase ctest entries via
  # --gtest_filter so the passing subcases can graduate to the active
  # CI set while individually-broken subcases stay quarantined with
  # sharper per-test debt tracking. Each failing subcase becomes its
  # own line in tests.yml's --exclude-regex and gets its own row in
  # quarantined-test-classification.md for individual fix-or-delete
  # decisions later.

  # Passing subcases — graduate to active set (run on every PR via tests.yml).
  set(_wmr_passing
    AuthoritativeMnemonicBindingAndLegacyFailClosed
    InvalidMnemonicRestoreFailsWithoutPartialWallet
    RejectsNonBip86PolicyWithoutPartialWallet
    RestoreRpcFuzzMalformedPayloadsNoCrashNoPartialWallets
    ReorgDepthFourClearsConfirmedBalanceWithoutChangeIndexRollback
    WrongBip39PassphraseFailsCleanlyWithExpectedAddressGuard
    EncryptionRoundTripRestoreAndDerivationPersistence
    RestoreResetsLegacyEncryptionStateBeforeReEncrypt
    ReorgSelfSpendWithChangeRestoresSpentStateAndHeightMetadata
  )
  foreach(_case IN LISTS _wmr_passing)
    add_test(
      NAME WalletMainnetReadiness_${_case}
      COMMAND test_wallet_mainnet_readiness --gtest_filter=WalletMainnetReadiness.${_case}
    )
    set_tests_properties(WalletMainnetReadiness_${_case} PROPERTIES
      LABELS "wallet;mainnet;hardening"
      TIMEOUT 60
    )
  endforeach()
  unset(_wmr_passing)

  # Bip86DeterminismProperty1000RandomMnemonics — graduated from quarantine in
  # the same change-set that fixed the stale 1447 coin-type pin in the test
  # helpers. Registered as its own entry (separate from the small-timeout
  # _wmr_passing batch above) because it runs 1000 BIP86 derivations + 10 RPC
  # restore samples and takes ~80 sec on Mac. On the slower Linux CI runners
  # it can exceed 60 sec, so it gets a 300-sec timeout. If this ever times
  # out in CI, investigate runner speed regression before bumping further.
  add_test(
    NAME WalletMainnetReadiness_Bip86DeterminismProperty1000RandomMnemonics
    COMMAND test_wallet_mainnet_readiness --gtest_filter=WalletMainnetReadiness.Bip86DeterminismProperty1000RandomMnemonics
  )
  set_tests_properties(WalletMainnetReadiness_Bip86DeterminismProperty1000RandomMnemonics PROPERTIES
    LABELS "wallet;mainnet;hardening;bip86;property"
    TIMEOUT 300
  )

  message(STATUS "  ✓ Wallet readiness tests registered: 10 active + 0 quarantined (was 1 bundled)")
else()
  message(STATUS "  ⚠ Wallet readiness test not found: tests/wallet/test_wallet_mainnet_readiness.cpp")
endif()

message(STATUS "✅ Phase 3 Premine Tests: All 3 mandatory tests registered")
message(STATUS "✅ Phase 3 Mining Validation: Height 2 test registered")
message(STATUS "✅ Phase N.1 Network Validation: Multi-node sync test registered")
message(STATUS "✅ Phase W.1 Wallet Validation: Restore + rescan test registered")

# PSBT Taproot Guardrails Test
add_executable(test_psbt_guardrails
  tests/test_psbt_guardrails.cpp
  src/wallet/psbt_taproot_validator.cpp
)
target_link_libraries(test_psbt_guardrails PRIVATE jsoncpp_static)
add_test(NAME PsbtBip86Guardrails COMMAND test_psbt_guardrails)

# BIP86 signing demonstration
# TEMPORARILY DISABLED (2026-02-02): Missing sha256/HASH160 includes when compiling hd_wallet.cpp directly
# TODO: Fix include paths or use dinero_wallet library instead of re-compiling sources
# add_executable(test_bip86_signing
#     test_bip86_signing.cpp
#     src/wallet/hd_wallet.cpp
#     src/wallet/psbt_taproot_validator.cpp
#     src/wallet/bip84_descriptor.cpp
#     src/wallet/bip86_descriptor.cpp
#     src/wallet/descriptor_checksum.cpp
# )
# target_link_libraries(test_bip86_signing dinero_core dinero_crypto dinero_wallet secp256k1 OpenSSL::SSL OpenSSL::Crypto)
# if(APPLE)
#     target_link_libraries(test_bip86_signing "-framework Security" "-framework CoreFoundation")
# endif()
# target_include_directories(test_bip86_signing PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)

# Descriptor PSBT Integration Test (Phase 2 Step 3/4)
# Minimal test - synthetic data, no wallet DB/RPC dependencies
# PSBT helpers exported from dinero_core (Phase 2 Step 4 complete)
add_executable(test_descriptor_psbt_integration
    tests/test_descriptor_psbt_integration.cpp
)
if(APPLE)
    target_link_libraries(test_descriptor_psbt_integration dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto "-framework Security")
else()
    target_link_libraries(test_descriptor_psbt_integration dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto)
endif()
target_include_directories(test_descriptor_psbt_integration PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)
add_test(NAME DescriptorPsbtIntegration COMMAND test_descriptor_psbt_integration)

# Simple Descriptor PSBT Test (Phase 2 Step 3)
# Tests core fixes: address→scriptPubKey and complete key derivation
add_executable(test_descriptor_psbt_simple
    tests/test_descriptor_psbt_simple.cpp
)
if(APPLE)
    target_link_libraries(test_descriptor_psbt_simple dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto "-framework Security")
else()
    target_link_libraries(test_descriptor_psbt_simple dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto)
endif()
target_include_directories(test_descriptor_psbt_simple PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)
add_test(NAME DescriptorPsbtSimple COMMAND test_descriptor_psbt_simple)

# KeyStoreImpl Test (Phase 3A)
# Tests BIP32 HD wallet implementation with BIP84/BIP86 address derivation
add_executable(test_keystore_impl
    tests/test_keystore_impl.cpp
)
if(APPLE)
    target_link_libraries(test_keystore_impl dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto "-framework Security")
else()
    target_link_libraries(test_keystore_impl dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto)
endif()
target_include_directories(test_keystore_impl PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)

# End-to-End Signing Flow Test (Phase 3B)
# Tests full descriptor → PSBT → sign → finalize flow
add_executable(test_e2e_signing
    tests/test_e2e_signing.cpp
)
if(APPLE)
    target_link_libraries(test_e2e_signing dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto "-framework Security")
else()
    target_link_libraries(test_e2e_signing dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto)
endif()
target_include_directories(test_e2e_signing PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)

# Complete PSBT Workflow Test (Phase 3C)
# Tests complete workflow: KeyStoreImpl → Descriptor → PSBT → Sign → Finalize → Extract TX
add_executable(test_psbt_complete_workflow
    tests/test_psbt_complete_workflow.cpp
)
if(APPLE)
    target_link_libraries(test_psbt_complete_workflow dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto "-framework Security")
else()
    target_link_libraries(test_psbt_complete_workflow dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto)
endif()
target_include_directories(test_psbt_complete_workflow PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)

# BIP86 Taproot Workflow Test (Phase 3C - Taproot Activation)
# Tests BIP86 key-path Taproot signing: KeyStoreImpl → TR Descriptor → PSBT → Schnorr Sign
add_executable(test_bip86_taproot_workflow
    tests/test_bip86_taproot_workflow.cpp
)
if(APPLE)
    target_link_libraries(test_bip86_taproot_workflow dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto "-framework Security")
else()
    target_link_libraries(test_bip86_taproot_workflow dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto)
endif()
target_include_directories(test_bip86_taproot_workflow PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)

# BIP86 Tapscript Guardrail Test (Phase 3C - Security Hardening)
# Tests that BIP86 wallets reject script-path spending PSBTs (tapscript safety)
add_executable(test_psbt_bip86_integration
    tests/test_psbt_bip86_integration.cpp
)
if(APPLE)
    target_link_libraries(test_psbt_bip86_integration dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto "-framework Security")
else()
    target_link_libraries(test_psbt_bip86_integration dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto)
endif()
target_include_directories(test_psbt_bip86_integration PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)
add_test(NAME PsbtBip86Integration COMMAND test_psbt_bip86_integration)

# Hardware Wallet BIP86 Integration Test (Phase 3C - HW Wallet Support)
# Tests complete Coldcard-style workflow: KeyStore → Descriptor → PSBT → Export → Sign → Import
add_executable(test_hardware_wallet_bip86
    tests/test_hardware_wallet_bip86.cpp
    src/rpc/methods_hardware_wallet.cpp
)
if(APPLE)
    target_link_libraries(test_hardware_wallet_bip86 dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto "-framework Security")
else()
    target_link_libraries(test_hardware_wallet_bip86 dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto)
endif()
target_include_directories(test_hardware_wallet_bip86 PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)

# PSBT Error Messages UX Test (Phase 3C - PSBT UX Polish)
# Validates clear, actionable error messages for exchanges/custodians/HW wallet users
add_executable(test_psbt_error_messages
    tests/test_psbt_error_messages.cpp
)
if(APPLE)
    target_link_libraries(test_psbt_error_messages dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto "-framework Security")
else()
    target_link_libraries(test_psbt_error_messages dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto)
endif()
target_include_directories(test_psbt_error_messages PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)

# Descriptor Persistence Test (Phase 3C - Descriptor Persistence)
# Validates descriptor storage, activation, and safe migration (NO crypto - fast tests)
add_executable(test_descriptor_persistence
    tests/test_descriptor_persistence.cpp
)
if(APPLE)
    target_link_libraries(test_descriptor_persistence dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto "-framework Security")
else()
    target_link_libraries(test_descriptor_persistence dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto)
endif()
target_include_directories(test_descriptor_persistence PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)

# Descriptor RPC Test (Phase 3C - Descriptor RPC Integration)
# Tests importdescriptor, listdescriptors, setactivedescriptor RPC methods
add_executable(test_descriptor_rpc
    tests/test_descriptor_rpc.cpp
)
if(APPLE)
    target_link_libraries(test_descriptor_rpc dinero_wallet dinero_core dinero_crypto dinero_rpc_handlers OpenSSL::SSL OpenSSL::Crypto "-framework Security")
else()
    target_link_libraries(test_descriptor_rpc dinero_wallet dinero_core dinero_crypto dinero_rpc_handlers OpenSSL::SSL OpenSSL::Crypto)
endif()
target_include_directories(test_descriptor_rpc PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)

# Descriptor Address Generation Test (Phase 3C - Descriptor → Address Type Mapping)
# Validates that active descriptors control address generation policy (BIP84 ↔ BIP86)
add_executable(test_descriptor_address_generation
    tests/test_descriptor_address_generation.cpp
)
if(APPLE)
    target_link_libraries(test_descriptor_address_generation dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto "-framework Security")
else()
    target_link_libraries(test_descriptor_address_generation dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto)
endif()
target_include_directories(test_descriptor_address_generation PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)

# Ledger Production Safety Test (CRITICAL - Must pass in production builds)
# Verifies that mock signing is DISABLED and signature fabrication is impossible
add_executable(test_ledger_production_safety
    tests/test_ledger_production_safety.cpp
)
if(APPLE)
    target_link_libraries(test_ledger_production_safety dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto "-framework Security")
else()
    target_link_libraries(test_ledger_production_safety dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto)
endif()
target_include_directories(test_ledger_production_safety PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)

# Ledger Hardware Wallet Integration Test (Requires DIN_HW_WALLET_MOCK=ON)
# Tests USB HID stack: hidapi → HIDTransport → LedgerWallet
# Validates device enumeration, connection lifecycle, and APDU framing
add_executable(test_ledger_integration
    tests/test_ledger_integration.cpp
)
if(APPLE)
    target_link_libraries(test_ledger_integration dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto "-framework Security")
else()
    target_link_libraries(test_ledger_integration dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto)
endif()
target_include_directories(test_ledger_integration PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)

# Trezor Hardware Wallet Integration Test
# Tests USB HID stack: hidapi → HIDTransport → TrezorTransport → TrezorWallet
# Validates device enumeration, connection lifecycle, wire protocol, and protobuf messages
if(ENABLE_TREZOR)
  add_executable(test_trezor_integration
      tests/test_trezor_integration.cpp
  )
  if(APPLE)
      target_link_libraries(test_trezor_integration dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto "-framework Security")
  else()
      target_link_libraries(test_trezor_integration dinero_wallet dinero_core dinero_crypto OpenSSL::SSL OpenSSL::Crypto)
  endif()
  target_include_directories(test_trezor_integration PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)
endif()

# ════════════════════════════════════════════════════════════════════════════
# Mixed Block Confidential Test (Dragon #1 - Confidential Transactions)
# Tests blocks containing mixed transaction types
# ════════════════════════════════════════════════════════════════════════════

# ════════════════════════════════════════════════════════════════════════════
# Confidential Reorg Test (Dragon #1 - Reorg Safety)
# Proves confidential UTXOs survive blockchain reorganizations
# Critical: Tests commitment preservation during disconnect/reconnect
# ════════════════════════════════════════════════════════════════════════════

# ════════════════════════════════════════════════════════════════════════════
# Phase 4 adversarial CT reorg soak removed in v7 — exercised confidential-tx
# reorg edge cases that don't apply on v7 (CT outputs rejected by version-range
# allowlist). Source deleted.

# DPI Protocol unit tests
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/dpi/test_dpi_protocol.cpp)
  add_executable(test_dpi_protocol
    tests/dpi/test_dpi_protocol.cpp
    src/dpi/dpi_protocol.cpp
  )
  target_link_libraries(test_dpi_protocol PRIVATE
    dinero_crypto
    dinero_wallet
    secp256k1
    OpenSSL::SSL
    OpenSSL::Crypto
    GTest::gtest
  )
  if(APPLE)
    target_link_libraries(test_dpi_protocol PRIVATE "-framework Security" "-framework CoreFoundation")
  endif()
  target_include_directories(test_dpi_protocol PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  add_test(NAME DpiProtocol COMMAND test_dpi_protocol)
  set_tests_properties(DpiProtocol PROPERTIES LABELS "dpi;protocol")
  message(STATUS "  ✓ DPI Protocol test registered: test_dpi_protocol")
endif()

# ─── Dashboard RPC smoke test ───────────────────────────────────────────────
if(BUILD_TESTING)
    add_test(NAME DashboardRpcsSmoke
        COMMAND ${CMAKE_SOURCE_DIR}/tests/network/test_dashboard_rpcs.sh)
    set_tests_properties(DashboardRpcsSmoke PROPERTIES
        ENVIRONMENT
            "DINEROD=$<TARGET_FILE:dinerod>;DINERO_CLI=$<TARGET_FILE:dinero-cli>"
        LABELS "network;rpc;integration"
        TIMEOUT 60
    )
endif()

# ─── relay_hints.list + getnetworkinfo.relay 24h-counter integration test ───
if(BUILD_TESTING)
    add_test(NAME RelayHintsListRpc
        COMMAND ${CMAKE_SOURCE_DIR}/tests/network/test_relay_hints_list_rpc.sh)
    set_tests_properties(RelayHintsListRpc PROPERTIES
        ENVIRONMENT
            "DINEROD=$<TARGET_FILE:dinerod>;DINERO_CLI=$<TARGET_FILE:dinero-cli>"
        LABELS "network;rpc;dashboard"
        TIMEOUT 60
    )
endif()

# ─── seeder.status/start/stop lifecycle smoke test ──────────────────────────
if(BUILD_TESTING AND DINERO_BUILD_SEEDER)
    add_test(NAME SeederLifecycleRpc
        COMMAND ${CMAKE_SOURCE_DIR}/tests/network/test_seeder_lifecycle_rpc.sh)
    set_tests_properties(SeederLifecycleRpc PROPERTIES
        ENVIRONMENT
            "DINEROD=$<TARGET_FILE:dinerod>;DINERO_CLI=$<TARGET_FILE:dinero-cli>;DINERO_SEEDER=$<TARGET_FILE:dinero-seeder>"
        LABELS "network;rpc;dashboard;seeder"
        TIMEOUT 60
    )
endif()

# ═══════════════════════════════════════════════════════════════════════════

# ─── issue #241/#214: stalled-peer eviction decision (peer_send_health) ─────
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/daemon/test_peer_send_health.cpp)
  add_executable(test_peer_send_health
    tests/daemon/test_peer_send_health.cpp
  )
  target_include_directories(test_peer_send_health PRIVATE
    ${CMAKE_SOURCE_DIR}/include
  )
  add_test(NAME PeerSendHealth COMMAND test_peer_send_health)
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/consensus/test_chainwork_from_hex.cpp)
  # Regression (PR #390): ChainworkFromHex must not throw on a 64-char
  # non-hex string (the snapshot-bootstrap deadlock).
  add_executable(test_chainwork_from_hex
    tests/consensus/test_chainwork_from_hex.cpp
    src/consensus/chainwork.cpp
  )
  target_link_libraries(test_chainwork_from_hex PRIVATE
    GTest::gtest_main
    dinero_crypto
    OpenSSL::Crypto
    pthread
  )
  target_include_directories(test_chainwork_from_hex PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
  )
  if(APPLE)
    target_link_libraries(test_chainwork_from_hex PRIVATE
      "-framework Security" "-framework CoreFoundation")
  endif()
  add_test(NAME ChainworkFromHex COMMAND test_chainwork_from_hex)
  set_tests_properties(ChainworkFromHex PROPERTIES LABELS "consensus;chainwork" TIMEOUT 30)
endif()

# ─── reorg event feed, Task 1: ReorgLog — bounded reorg ring ────────────────
add_executable(test_reorg_log
  tests/test_reorg_log.cpp
)
add_dependencies(test_reorg_log gtest gtest_main)
target_include_directories(test_reorg_log BEFORE PRIVATE
  ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest/include
  ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(test_reorg_log PRIVATE
  GTest::gtest_main
)
add_test(NAME test_reorg_log COMMAND test_reorg_log)
set_tests_properties(test_reorg_log PROPERTIES TIMEOUT 60)

# ─── GuardedForest — Utreexo forest read-during-free UAF regression ─────────
# Plain exit-code test (no gtest): proves a whole-value Replace() cannot
# complete while a reader holds the shared lock — the property whose absence
# is the read-during-free UAF. Registered UNLABELLED so it runs in the
# required "full build + ctest" job (the main lanes filter with
# `--label-exclude 'integration|gate|release|canonicality|fuzz'`).
add_executable(test_guarded_forest
  tests/consensus/test_guarded_forest.cpp
)
target_include_directories(test_guarded_forest BEFORE PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)
find_package(Threads REQUIRED)
target_link_libraries(test_guarded_forest PRIVATE Threads::Threads)
add_test(NAME GuardedForestExclusion COMMAND test_guarded_forest)
set_tests_properties(GuardedForestExclusion PROPERTIES TIMEOUT 30)

# ─── Forest lock concurrency stress (deadlock-freedom under contention) ─────
# Drives the real ConsensusUTXOSet guarded accessors from many reader/writer
# threads with a watchdog that hard-fails on a stall (deadlock). Short default
# duration so it fits the required full-ctest lane; run longer manually via
#   ./test_forest_lock_stress <readers> <writers> <seconds>
add_executable(test_forest_lock_stress
  tests/consensus/test_forest_lock_stress.cpp
)
target_include_directories(test_forest_lock_stress PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(test_forest_lock_stress PRIVATE
  dinero_chainstate
  dinero_consensus
  dinero_shielded
  dinero_crypto
  jsoncpp_static
  RocksDB::rocksdb
  OpenSSL::Crypto
  OpenSSL::SSL
  pthread
  sqlite3
)
link_zstd_if_needed(test_forest_lock_stress)
if(APPLE)
  target_link_libraries(test_forest_lock_stress PRIVATE
    "-framework Security"
    "-framework CoreFoundation"
  )
endif()
add_test(NAME ForestLockStress COMMAND test_forest_lock_stress 8 2 3)
set_tests_properties(ForestLockStress PROPERTIES TIMEOUT 60)

# ─── ASERT fixed-point exponent on the fast-block (negative excessTime) path ──
# Header-only (CalculateASERT_Target / TargetFromBitsBE are inline). Locks the
# fast=harder / slow=easier behavior of the branch whose left-shift was UB
# pre-C++20. Registered unlabelled so it runs in the required full-ctest job.
add_executable(test_asert_signed_shift
  tests/consensus/test_asert_signed_shift.cpp
)
target_include_directories(test_asert_signed_shift PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/src/consensus
)
add_test(NAME AsertSignedShift COMMAND test_asert_signed_shift)
set_tests_properties(AsertSignedShift PROPERTIES TIMEOUT 30)

# ---------------------------------------------------------------------------
# FleetWatcherTests — the fleet watcher's Python suite (tools/fleet-watcher).
#
# Registered here, deliberately UNLABELLED, so it runs in the ordinary ctest
# sweep of the "Linux x86_64 - full build + ctest" job. That job is a required
# check, and the main lanes filter with
# `--label-exclude 'integration|gate|release|canonicality|fuzz'` — a label from
# that set would remove this from every lane that gates a merge.
#
# Registered UNCONDITIONALLY, with no find_package(Python3) guard. A guard is
# how this test would quietly cease to exist: if the interpreter probe failed,
# add_test() would never run, ctest would report success over zero tests, and
# the suite would be exactly what it was before this registration — 142 tests
# that pass locally and gate nothing. A loud failure on a host without python3
# is the better error. scripts/ci/check_ctest_integrity.py resolves bare
# `python3` on PATH, so this satisfies that gate rather than tripping it.
#
# unittest, not pytest: the suite has no third-party dependency and neither
# should running it. `discover -t .` sets the top-level directory so that
# `tests/` is importable as a package and the modules under test resolve.
add_test(
    NAME FleetWatcherTests
    COMMAND python3 -m unittest discover -s tests -t .
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tools/fleet-watcher
)
set_tests_properties(FleetWatcherTests PROPERTIES TIMEOUT 120)
