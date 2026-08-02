# Phase E.3: Integration & Testing
# Wallet Lifecycle + Fee Estimation + Transaction Assembly + Security Regression

message(STATUS "Configuring Phase E.3 integration tests...")

# Test 1: Wallet Lifecycle (E.3.1)
# NOTE: Disabled - requires ENABLE_LIGHTNING for LightningService symbols
# TODO: Re-enable when lightning module is fully integrated
# add_executable(test_wallet_lifecycle_e3
#     test_wallet_lifecycle_e3.cpp
# )
#
# target_link_libraries(test_wallet_lifecycle_e3 PRIVATE
#     dinero_consensus
#     dinero_chainstate
#     dinero_wallet
#     dinero_crypto
#     lightning_core_static
#     bulletproofs_ffi
#     ${ROCKSDB_LIBRARY}
#     secp256k1
#     sqlite3
#     argon2
#     jsoncpp_static
#     GTest::gtest
# )
#
# target_include_directories(test_wallet_lifecycle_e3 PRIVATE
#     ${CMAKE_SOURCE_DIR}/include
#     ${CMAKE_SOURCE_DIR}/src
#     ${CMAKE_BINARY_DIR}
#     ${CMAKE_CURRENT_SOURCE_DIR}
#     ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include
#     ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
#     ${CMAKE_SOURCE_DIR}/third_party/argon2/include
# )
#
# target_compile_definitions(test_wallet_lifecycle_e3 PRIVATE
#     CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
# )
#
# # Add Apple frameworks if on macOS
# if(APPLE)
#     target_link_libraries(test_wallet_lifecycle_e3 PRIVATE
#         "-framework Security"
#         "-framework CoreFoundation"
#     )
# endif()
#
# # Register test with CTest
# add_test(NAME WalletLifecycleE3 COMMAND test_wallet_lifecycle_e3)

# ═══════════════════════════════════════════════════════════════════════
# Phase F.5: End-to-End Mining Policy Integration Tests
# ═══════════════════════════════════════════════════════════════════════

message(STATUS "Configuring Phase F.5 end-to-end tests...")

# E2E Test Harness Library (requires curl)
if(NOT TARGET CURL::libcurl)
  find_package(CURL QUIET)
endif()
if(CURL_FOUND)
    add_library(e2e_test_harness STATIC
        e2e_test_harness.cpp
    )

    target_link_libraries(e2e_test_harness PUBLIC
        jsoncpp_static
        CURL::libcurl
    )

    target_include_directories(e2e_test_harness PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
    )
else()
    message(STATUS "  ⊘ E2E test harness skipped (libcurl not found)")
endif()

# Mining Policy E2E Tests (requires curl for e2e_test_harness)
if(CURL_FOUND AND EXISTS ${CMAKE_SOURCE_DIR}/tests/integration/test_mining_policy_e2e.cpp)
    add_executable(test_mining_policy_e2e
        test_mining_policy_e2e.cpp
    )

    target_link_libraries(test_mining_policy_e2e PRIVATE
        e2e_test_harness
        jsoncpp_static
        CURL::libcurl
        GTest::gtest
    )

    target_include_directories(test_mining_policy_e2e PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
    )

    # Registration is UNCONDITIONAL (issue #428). The previous gate was a
    # configure-time find_program(DINEROD_BINARY dinerod PATHS <src> <build>),
    # which on a clean checkout always fails — configure runs before build, so
    # ${CMAKE_BINARY_DIR}/dinerod does not exist yet. CI therefore never
    # REGISTERED this test or ColdStartConsensus: they did not fail, they
    # silently did not exist, which is how #413 survived for months. It also
    # made coverage build-order dependent: the tests appeared only if some
    # dinerod happened to be findable when cmake last ran.
    #
    # dinerod is an in-tree target, so its path is known at configure time
    # regardless of build order. Depend on the target and hand the harness the
    # absolute path via the DINEROD environment variable — no PATH lookup, no
    # reliance on the process working directory.
    add_dependencies(test_mining_policy_e2e dinerod)
    add_test(NAME MiningPolicyE2E COMMAND test_mining_policy_e2e)
    set_tests_properties(MiningPolicyE2E PROPERTIES
        LABELS "e2e;mining;policy;f5;e2e-daemon"
        ENVIRONMENT "DINEROD=$<TARGET_FILE:dinerod>"
        TIMEOUT 120
    )
    message(STATUS "  ✓ MiningPolicyE2E registered (daemon: $<TARGET_FILE:dinerod>)")
endif()

# Shared RPC authcookie resolution regression for WebSocket/HTTP auth.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_ws_cookie_path_resolution.cpp)
    add_executable(test_ws_cookie_path_resolution
        test_ws_cookie_path_resolution.cpp
        ${CMAKE_SOURCE_DIR}/src/daemon/auth_cookie.cpp
        ${CMAKE_SOURCE_DIR}/src/daemon/rpc_authcookie.cpp
        ${CMAKE_SOURCE_DIR}/src/common/logger.cpp
    )

    target_include_directories(test_ws_cookie_path_resolution PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
    )

    add_test(NAME WsCookiePathResolution COMMAND test_ws_cookie_path_resolution)
    set_tests_properties(WsCookiePathResolution PROPERTIES
        LABELS "rpc-smoke;rpc;websocket;auth"
        TIMEOUT 30
    )
    message(STATUS "  ✓ WsCookiePathResolution registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_parallel_block_download_targeted.sh)
    add_test(
        NAME ParallelBlockDownloadTargeted
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_parallel_block_download_targeted.sh
    )
    set_tests_properties(ParallelBlockDownloadTargeted PROPERTIES
        LABELS "integration;p2p;block-download"
        ENVIRONMENT "DINEROD=$<TARGET_FILE:dinerod>"
        TIMEOUT 180
    )
    message(STATUS "  ✓ ParallelBlockDownloadTargeted registered (daemon: $<TARGET_FILE:dinerod>)")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_lagging_peer_catchup_no_orphans.sh)
    add_test(
        NAME LaggingPeerCatchupNoOrphans
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_lagging_peer_catchup_no_orphans.sh
    )
    set_tests_properties(LaggingPeerCatchupNoOrphans PROPERTIES
        LABELS "integration;p2p;catchup;orphan"
        TIMEOUT 240
    )
    message(STATUS "  ✓ LaggingPeerCatchupNoOrphans registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_cross_seed_restart_reorg_parity.sh)
    add_test(
        NAME CrossSeedRestartReorgParity
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_cross_seed_restart_reorg_parity.sh
    )
    set_tests_properties(CrossSeedRestartReorgParity PROPERTIES
        LABELS "integration;p2p;reorg;restart;parity"
        ENVIRONMENT "DINEROD=$<TARGET_FILE:dinerod>"
        TIMEOUT 240
    )
    message(STATUS "  ✓ CrossSeedRestartReorgParity registered (daemon: $<TARGET_FILE:dinerod>)")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_connecttip_restart_equivalence.sh)
    add_test(
        NAME ConnectTipRestartEquivalence
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_connecttip_restart_equivalence.sh
    )
    set_tests_properties(ConnectTipRestartEquivalence PROPERTIES
        LABELS "integration;restart;chainstate;canonicality;phase1-canonical-recovery"
        TIMEOUT 420
    )
    message(STATUS "  ✓ ConnectTipRestartEquivalence registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_d3_full_user_tx_missing_undo.sh)
    add_test(
        NAME D3FullUserTxMissingUndo
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_d3_full_user_tx_missing_undo.sh
    )
    set_tests_properties(D3FullUserTxMissingUndo PROPERTIES
        LABELS "integration;restart;chainstate;reorg;undo;phase1-canonical-recovery"
        TIMEOUT 360
    )
    message(STATUS "  ✓ D3FullUserTxMissingUndo registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_undo_metadata_restamp.sh)
    add_test(
        NAME UndoMetadataRestamp
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_undo_metadata_restamp.sh
    )
    set_tests_properties(UndoMetadataRestamp PROPERTIES
        LABELS "integration;restart;chainstate;undo;metadata;phase1-canonical-recovery"
        TIMEOUT 300
    )
    message(STATUS "  ✓ UndoMetadataRestamp registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_reindex_chainstate_utreexo_equivalence.sh)
    add_test(
        NAME ReindexChainstateUtreexoEquivalence
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_reindex_chainstate_utreexo_equivalence.sh
    )
    set_tests_properties(ReindexChainstateUtreexoEquivalence PROPERTIES
        LABELS "integration;reindex;chainstate;utreexo;canonicality;phase1-canonical-recovery"
        TIMEOUT 300
    )
    message(STATUS "  ✓ ReindexChainstateUtreexoEquivalence registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_reindex_promotion_restart_equivalence.sh)
    add_test(
        NAME ReindexPromotionRestartEquivalence
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_reindex_promotion_restart_equivalence.sh
    )
    set_tests_properties(ReindexPromotionRestartEquivalence PROPERTIES
        LABELS "integration;restart;reindex;canonicality;phase1-canonical-recovery"
        TIMEOUT 480
    )
    message(STATUS "  ✓ ReindexPromotionRestartEquivalence registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_invalidity_restart_sticky.sh)
    add_test(
        NAME InvalidityRestartSticky
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_invalidity_restart_sticky.sh
    )
    set_tests_properties(InvalidityRestartSticky PROPERTIES
        LABELS "integration;restart;invalidity;canonicality;phase1-canonical-recovery"
        TIMEOUT 240
    )
    message(STATUS "  ✓ InvalidityRestartSticky registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_invalidity_crash_restart_equivalence.sh)
    add_test(
        NAME InvalidityCrashRestartEquivalence
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_invalidity_crash_restart_equivalence.sh
    )
    set_tests_properties(InvalidityCrashRestartEquivalence PROPERTIES
        LABELS "integration;restart;invalidity;canonicality;phase1-canonical-recovery"
        TIMEOUT 240
    )
    message(STATUS "  ✓ InvalidityCrashRestartEquivalence registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_reconsider_crash_restart_equivalence.sh)
    add_test(
        NAME ReconsiderCrashRestartEquivalence
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_reconsider_crash_restart_equivalence.sh
    )
    set_tests_properties(ReconsiderCrashRestartEquivalence PROPERTIES
        LABELS "integration;restart;invalidity;canonicality;phase1-canonical-recovery"
        TIMEOUT 360
    )
    message(STATUS "  ✓ ReconsiderCrashRestartEquivalence registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_invalidity_import_equivalence.sh)
    add_test(
        NAME InvalidityImportEquivalence
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_invalidity_import_equivalence.sh
    )
    set_tests_properties(InvalidityImportEquivalence PROPERTIES
        LABELS "integration;restart;invalidity;canonicality;phase1-canonical-recovery"
        TIMEOUT 240
    )
    message(STATUS "  ✓ InvalidityImportEquivalence registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_script_validity_durability.sh)
    add_test(
        NAME ScriptValidityDurability
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_script_validity_durability.sh
    )
    set_tests_properties(ScriptValidityDurability PROPERTIES
        LABELS "integration;restart;canonicality"
        ENVIRONMENT "DINEROD=$<TARGET_FILE:dinerod>"
        TIMEOUT 300
    )
    message(STATUS "  ✓ ScriptValidityDurability registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_script_validity_crash_ordering.sh)
    add_test(
        NAME ScriptValidityCrashOrdering
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_script_validity_crash_ordering.sh
    )
    set_tests_properties(ScriptValidityCrashOrdering PROPERTIES
        LABELS "integration;restart;canonicality"
        ENVIRONMENT "DINEROD=$<TARGET_FILE:dinerod>"
        TIMEOUT 300
    )
    message(STATUS "  ✓ ScriptValidityCrashOrdering registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_mine_after_invalidate.sh)
    add_test(
        NAME MineAfterInvalidate
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_mine_after_invalidate.sh
    )
    set_tests_properties(MineAfterInvalidate PROPERTIES
        LABELS "integration;restart;canonicality"
        ENVIRONMENT "DINEROD=$<TARGET_FILE:dinerod>"
        TIMEOUT 300
    )
    message(STATUS "  ✓ MineAfterInvalidate registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_mine_after_invalidate_restart.sh)
    add_test(
        NAME MineAfterInvalidateRestart
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_mine_after_invalidate_restart.sh
    )
    set_tests_properties(MineAfterInvalidateRestart PROPERTIES
        LABELS "integration;restart;canonicality"
        ENVIRONMENT "DINEROD=$<TARGET_FILE:dinerod>"
        TIMEOUT 420
    )
    message(STATUS "  ✓ MineAfterInvalidateRestart registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_fork_block_status_restart.sh)
    add_test(
        NAME ForkBlockStatusRestart
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_fork_block_status_restart.sh
    )
    set_tests_properties(ForkBlockStatusRestart PROPERTIES
        LABELS "integration;restart;canonicality"
        ENVIRONMENT "DINEROD=$<TARGET_FILE:dinerod>"
        TIMEOUT 300
    )
    message(STATUS "  ✓ ForkBlockStatusRestart registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_script_validity_reorg.sh)
    add_test(
        NAME ScriptValidityReorg
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_script_validity_reorg.sh
    )
    set_tests_properties(ScriptValidityReorg PROPERTIES
        LABELS "integration;restart;canonicality"
        ENVIRONMENT "DINEROD=$<TARGET_FILE:dinerod>"
        TIMEOUT 300
    )
    message(STATUS "  ✓ ScriptValidityReorg registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_rpc_error_envelope.sh)
    add_test(
        NAME RpcErrorEnvelope
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_rpc_error_envelope.sh
    )
    set_tests_properties(RpcErrorEnvelope PROPERTIES
        LABELS "integration;canonicality"
        ENVIRONMENT "DINEROD=$<TARGET_FILE:dinerod>"
        TIMEOUT 300
    )
    message(STATUS "  ✓ RpcErrorEnvelope registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_tip_persist_restart_equivalence.sh)
    add_test(
        NAME TipPersistRestartEquivalence
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_tip_persist_restart_equivalence.sh
    )
    set_tests_properties(TipPersistRestartEquivalence PROPERTIES
        LABELS "integration;restart;chainstate;utreexo;canonicality;phase1-canonical-recovery"
        TIMEOUT 240
    )
    message(STATUS "  ✓ TipPersistRestartEquivalence registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_recovery_marker_restart_equivalence.sh)
    add_test(
        NAME RecoveryMarkerRestartEquivalence
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_recovery_marker_restart_equivalence.sh
    )
    set_tests_properties(RecoveryMarkerRestartEquivalence PROPERTIES
        LABELS "integration;restart;recovery-marker;canonicality;phase1-canonical-recovery"
        TIMEOUT 240
    )
    message(STATUS "  ✓ RecoveryMarkerRestartEquivalence registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_reorg_marker_aligned_restart_equivalence.sh)
    add_test(
        NAME ReorgMarkerAlignedRestartEquivalence
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_reorg_marker_aligned_restart_equivalence.sh
    )
    set_tests_properties(ReorgMarkerAlignedRestartEquivalence PROPERTIES
        LABELS "integration;restart;recovery-marker;canonicality;phase1-canonical-recovery"
        TIMEOUT 240
    )
    message(STATUS "  ✓ ReorgMarkerAlignedRestartEquivalence registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_interrupted_reorg_fail_safe.sh)
    add_test(
        NAME InterruptedReorgFailSafe
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_interrupted_reorg_fail_safe.sh
    )
    set_tests_properties(InterruptedReorgFailSafe PROPERTIES
        LABELS "integration;restart;recovery-marker;canonicality;phase1-canonical-recovery"
        TIMEOUT 180
    )
    message(STATUS "  ✓ InterruptedReorgFailSafe registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_header_cf_restart_equivalence.sh)
    add_test(
        NAME HeaderCFRestartEquivalence
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_header_cf_restart_equivalence.sh
    )
    set_tests_properties(HeaderCFRestartEquivalence PROPERTIES
        LABELS "integration;restart;headers;canonicality;phase1-canonical-recovery"
        TIMEOUT 240
    )
    message(STATUS "  ✓ HeaderCFRestartEquivalence registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_header_filter_replay_equivalence.sh)
    add_test(
        NAME HeaderFilterReplayEquivalence
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_header_filter_replay_equivalence.sh
    )
    set_tests_properties(HeaderFilterReplayEquivalence PROPERTIES
        LABELS "integration;replay;headers;filters;canonicality;phase1-canonical-recovery"
        TIMEOUT 300
    )
    message(STATUS "  ✓ HeaderFilterReplayEquivalence registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_position_index_restart_equivalence.sh)
    add_test(
        NAME PositionIndexRestartEquivalence
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_position_index_restart_equivalence.sh
    )
    set_tests_properties(PositionIndexRestartEquivalence PROPERTIES
        LABELS "integration;restart;utreexo;position-index;canonicality;phase1-canonical-recovery"
        TIMEOUT 240
    )
    message(STATUS "  ✓ PositionIndexRestartEquivalence registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_header_backlog_restart_equivalence.sh)
    add_test(
        NAME HeaderBacklogRestartEquivalence
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_header_backlog_restart_equivalence.sh
    )
    set_tests_properties(HeaderBacklogRestartEquivalence PROPERTIES
        LABELS "integration;restart;headers;checkpoint;canonicality;phase1-canonical-recovery"
        ENVIRONMENT "DINERO_BUILD_DIR=${CMAKE_BINARY_DIR}"
        TIMEOUT 240
    )
    message(STATUS "  ✓ HeaderBacklogRestartEquivalence registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_compact_block_relay_e2e.sh)
    add_test(
        NAME CompactBlockRelayE2E
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_compact_block_relay_e2e.sh
    )
    set_tests_properties(CompactBlockRelayE2E PROPERTIES
        LABELS "integration;p2p;compact-blocks"
        TIMEOUT 240
    )
    message(STATUS "  ✓ CompactBlockRelayE2E registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_peer_metadata_runtime_identity.sh)
    add_test(
        NAME PeerMetadataRuntimeIdentity
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_peer_metadata_runtime_identity.sh
    )
    set_tests_properties(PeerMetadataRuntimeIdentity PROPERTIES
        LABELS "integration;p2p;peer-metadata"
        TIMEOUT 180
    )
    message(STATUS "  ✓ PeerMetadataRuntimeIdentity registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_chain_identity_sync.sh)
    add_test(
        NAME ChainIdentitySync
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_chain_identity_sync.sh
    )
    set_tests_properties(ChainIdentitySync PROPERTIES
        LABELS "chain-identity-smoke;consensus;chain-identity"
        TIMEOUT 30
    )
    message(STATUS "  ✓ ChainIdentitySync registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_testnet_cold_start_genesis.sh)
    add_test(
        NAME TestnetColdStartGenesis
        COMMAND ${CMAKE_COMMAND} -E env
            "DINEROD=$<TARGET_FILE:dinerod>"
            ${CMAKE_CURRENT_SOURCE_DIR}/test_testnet_cold_start_genesis.sh
    )
    set_tests_properties(TestnetColdStartGenesis PROPERTIES
        LABELS "chain-identity-smoke;consensus;chain-identity;testnet"
        TIMEOUT 120
    )
    message(STATUS "  ✓ TestnetColdStartGenesis registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_p2pservice_network_control.sh)
    add_test(
        NAME P2PServiceNetworkControl
        COMMAND ${CMAKE_COMMAND} -E env
            "DINEROD=$<TARGET_FILE:dinerod>"
            ${CMAKE_CURRENT_SOURCE_DIR}/test_p2pservice_network_control.sh
    )
    set_tests_properties(P2PServiceNetworkControl PROPERTIES
        LABELS "p2p-network-control-smoke;p2p;rpc;network-control"
        TIMEOUT 180
    )
    message(STATUS "  ✓ P2PServiceNetworkControl registered")
endif()

# AssumeUTXO replay engine keystone e2e (plan 2026-06-10 Task 8):
# real genesis->base replay retires the trust marker (spec Required Test 4),
# poisoned snapshot goes fatal_mismatch and survives restart (Required Test 1),
# operator reset recovers, different-base re-entry refused, and a persisted
# stall recovers across restarts through the real worker.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_assumeutxo_replay_e2e.sh)
    add_test(
        NAME AssumeUtxoReplayE2E
        COMMAND ${CMAKE_COMMAND} -E env
            "DINEROD=$<TARGET_FILE:dinerod>"
            bash ${CMAKE_CURRENT_SOURCE_DIR}/test_assumeutxo_replay_e2e.sh
    )
    # NOTE: deliberately NOT labeled "integration" — the CI ctest lane runs
    # --label-exclude 'integration|...', so that label would silently drop the
    # keystone replay e2e from CI (same trap documented for P2PStalenessRecovery
    # below). This test gates spec Required Tests 1+4 end-to-end.
    set_tests_properties(AssumeUtxoReplayE2E PROPERTIES
        LABELS "assumeutxo;replay;e2e"
        TIMEOUT 600
    )
    message(STATUS "  ✓ AssumeUtxoReplayE2E registered")
endif()

# issue #353 bug 2: AssumeUTXO promotion race. Source keeps mining PAST base and
# stays connected; the consumer's slowed genesis->base replay
# (DINERO_DEBUG_BG_VALIDATION_DELAY_MS) lets forward sync race the tip past base.
# Asserts the fix's hold line fires, promotion still writes the pre-base coin to
# the coin CF (gettxout non-null — the thing the bug breaks), catch-up to base+K,
# and clean mode exit. Neuter-proven (remove the ActivateBestChain cap -> A2 null).
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_assumeutxo_promotion_race.sh)
    add_test(
        NAME AssumeUtxoPromotionRace
        COMMAND ${CMAKE_COMMAND} -E env
            "DINEROD=$<TARGET_FILE:dinerod>"
            bash ${CMAKE_CURRENT_SOURCE_DIR}/test_assumeutxo_promotion_race.sh
    )
    # Deliberately NOT labeled "integration" — the CI ctest lane runs
    # --label-exclude 'integration|...', which would silently drop this from CI
    # (same trap documented for AssumeUtxoReplayE2E above).
    set_tests_properties(AssumeUtxoPromotionRace PROPERTIES
        LABELS "assumeutxo;replay;e2e;promotion-race"
        TIMEOUT 900
    )
    message(STATUS "  ✓ AssumeUtxoPromotionRace registered")
endif()

# Mobile-profile contract: assumeutxo_forward_connect=1 connects forward from
# the snapshot base immediately (no #361 hold), and promotion lands in
# advanced-tip mode (coin CF reconciled against the LIVE set, completion via
# durable marker, tip never regressed). Same topology + race lever as
# AssumeUtxoPromotionRace; asserts the forward-connect contract instead.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_assumeutxo_forward_connect.sh)
    add_test(
        NAME AssumeUtxoForwardConnect
        COMMAND ${CMAKE_COMMAND} -E env
            "DINEROD=$<TARGET_FILE:dinerod>"
            bash ${CMAKE_CURRENT_SOURCE_DIR}/test_assumeutxo_forward_connect.sh
    )
    # NOT labeled "integration" — same CI label-exclude trap as above.
    set_tests_properties(AssumeUtxoForwardConnect PROPERTIES
        LABELS "assumeutxo;replay;e2e;forward-connect"
        TIMEOUT 900
    )
    message(STATUS "  ✓ AssumeUtxoForwardConnect registered")
endif()

# issue #214: two-node integration repro of in-daemon staleness recovery.
# CONTROL (recovery off -> stays stuck) vs EXPERIMENT (recovery on -> re-getheaders
# and catches up). Needs the DINERO_TEST_* regtest hooks built into dinerod.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_p2p_staleness_recovery.sh)
    add_test(
        NAME P2PStalenessRecovery
        COMMAND ${CMAKE_COMMAND} -E env
            "DINEROD=$<TARGET_FILE:dinerod>"
            ${CMAKE_CURRENT_SOURCE_DIR}/test_p2p_staleness_recovery.sh
    )
    # NOTE: deliberately NOT labeled "integration" — the CI ctest lane runs
    # --label-exclude 'integration|...', so that label would silently drop this
    # test from CI. Mirror P2PServiceNetworkControl's label set, which DOES run.
    set_tests_properties(P2PStalenessRecovery PROPERTIES
        LABELS "p2p-network-control-smoke;p2p;staleness-recovery;network-control"
        TIMEOUT 240
    )
    message(STATUS "  ✓ P2PStalenessRecovery (#214) registered")
endif()

if(UNIX AND NOT APPLE AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_relay_nat_netns.sh)
    find_program(DINERO_BASH_EXECUTABLE bash)
    if(DINERO_BASH_EXECUTABLE)
        add_test(
            NAME RelayNatNetnsHarness
            COMMAND ${CMAKE_COMMAND} -E env
                "DINEROD=$<TARGET_FILE:dinerod>"
                ${DINERO_BASH_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_relay_nat_netns.sh
        )
        set_tests_properties(RelayNatNetnsHarness PROPERTIES
            LABELS "integration;p2p;relay;nat;netns"
            TIMEOUT 300
            SKIP_RETURN_CODE 77
        )
        message(STATUS "  ✓ RelayNatNetnsHarness registered (set DINERO_RUN_NETNS_NAT=1 and run as root)")
    else()
        message(STATUS "  ⊘ RelayNatNetnsHarness skipped (bash not found)")
    endif()
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_rpc_stop_clean_shutdown.sh)
    add_test(
        NAME RpcStopCleanShutdown
        COMMAND ${CMAKE_COMMAND} -E env
            "DINEROD=$<TARGET_FILE:dinerod>"
            ${CMAKE_CURRENT_SOURCE_DIR}/test_rpc_stop_clean_shutdown.sh
    )
    set_tests_properties(RpcStopCleanShutdown PROPERTIES
        LABELS "rpc-shutdown-smoke;rpc;shutdown"
        TIMEOUT 120
    )
    message(STATUS "  ✓ RpcStopCleanShutdown registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_dpi_header_filter_proof_flow.sh)
    add_test(
        NAME DpiHeaderFilterProofFlow
        COMMAND ${CMAKE_COMMAND} -E env
            "DINEROD=$<TARGET_FILE:dinerod>"
            ${CMAKE_CURRENT_SOURCE_DIR}/test_dpi_header_filter_proof_flow.sh
    )
    set_tests_properties(DpiHeaderFilterProofFlow PROPERTIES
        LABELS "dpi-light-wallet-smoke;wallet;utreexo;dpi;filters"
        TIMEOUT 240
    )
    message(STATUS "  ✓ DpiHeaderFilterProofFlow registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_address_balance_mempool_overlay.sh)
    add_test(
        NAME AddressBalanceMempoolOverlay
        COMMAND ${CMAKE_COMMAND} -E env
            "DINEROD=$<TARGET_FILE:dinerod>"
            ${CMAKE_CURRENT_SOURCE_DIR}/test_address_balance_mempool_overlay.sh
    )
    set_tests_properties(AddressBalanceMempoolOverlay PROPERTIES
        LABELS "mempool-rawtx-smoke;rpc;mempool;wallet;address-index"
        TIMEOUT 180
    )
    message(STATUS "  ✓ AddressBalanceMempoolOverlay registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_address_index_cross_hrp.sh)
    add_test(
        NAME AddressIndexCrossHRP
        COMMAND ${CMAKE_COMMAND} -E env
            "DINEROD=$<TARGET_FILE:dinerod>"
            ${CMAKE_CURRENT_SOURCE_DIR}/test_address_index_cross_hrp.sh
    )
    set_tests_properties(AddressIndexCrossHRP PROPERTIES
        LABELS "rpc-smoke;rpc;wallet;address-index;bech32"
        TIMEOUT 180
    )
    message(STATUS "  ✓ AddressIndexCrossHRP registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_p2mr_address_index_rpc_smoke.sh)
    add_test(
        NAME P2MRAddressIndexRpcSmoke
        COMMAND ${CMAKE_COMMAND} -E env
            "DINEROD=$<TARGET_FILE:dinerod>"
            ${CMAKE_CURRENT_SOURCE_DIR}/test_p2mr_address_index_rpc_smoke.sh
    )
    set_tests_properties(P2MRAddressIndexRpcSmoke PROPERTIES
        LABELS "p2mr-address-smoke;rpc;wallet;address-index;p2mr"
        TIMEOUT 240
    )
    message(STATUS "  ✓ P2MRAddressIndexRpcSmoke registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_address_parent_child_mempool_lifecycle.sh)
    add_test(
        NAME AddressParentChildMempoolLifecycle
        COMMAND ${CMAKE_COMMAND} -E env
            "DINEROD=$<TARGET_FILE:dinerod>"
            ${CMAKE_CURRENT_SOURCE_DIR}/test_address_parent_child_mempool_lifecycle.sh
    )
    set_tests_properties(AddressParentChildMempoolLifecycle PROPERTIES
        LABELS "mempool-package-smoke;rpc;mempool;wallet;address-index;parent-child"
        TIMEOUT 180
    )
    message(STATUS "  ✓ AddressParentChildMempoolLifecycle registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_taproot_signrawtransaction_rbf_overlay.sh)
    add_test(
        NAME TaprootSignRawTransactionRbfOverlay
        COMMAND ${CMAKE_COMMAND} -E env
            "DINEROD=$<TARGET_FILE:dinerod>"
            ${CMAKE_CURRENT_SOURCE_DIR}/test_taproot_signrawtransaction_rbf_overlay.sh
    )
    set_tests_properties(TaprootSignRawTransactionRbfOverlay PROPERTIES
        LABELS "mempool-rawtx-smoke;wallet;mempool;rbf;taproot"
        TIMEOUT 240
    )
    message(STATUS "  ✓ TaprootSignRawTransactionRbfOverlay registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_taproot_signrawtransaction_rbf_confirmation.sh)
    add_test(
        NAME TaprootSignRawTransactionRbfConfirmation
        COMMAND ${CMAKE_COMMAND} -E env
            "DINEROD=$<TARGET_FILE:dinerod>"
            ${CMAKE_CURRENT_SOURCE_DIR}/test_taproot_signrawtransaction_rbf_confirmation.sh
    )
    set_tests_properties(TaprootSignRawTransactionRbfConfirmation PROPERTIES
        LABELS "mempool-rawtx-smoke;wallet;mempool;rbf;taproot"
        TIMEOUT 240
    )
    message(STATUS "  ✓ TaprootSignRawTransactionRbfConfirmation registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_parent_child_rbf_replacement_with_mempool_parent.sh)
    add_test(
        NAME ParentChildRbfReplacementWithMempoolParent
        COMMAND ${CMAKE_COMMAND} -E env
            "DINEROD=$<TARGET_FILE:dinerod>"
            ${CMAKE_CURRENT_SOURCE_DIR}/test_parent_child_rbf_replacement_with_mempool_parent.sh
    )
    set_tests_properties(ParentChildRbfReplacementWithMempoolParent PROPERTIES
        LABELS "mempool-package-smoke;wallet;mempool;rbf;parent-child"
        TIMEOUT 240
    )
    message(STATUS "  ✓ ParentChildRbfReplacementWithMempoolParent registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_wallet_createrawtransaction_scriptpubkey_outputs.sh)
    add_test(
        NAME WalletCreateRawTransactionScriptPubKeyOutputs
        COMMAND ${CMAKE_COMMAND} -E env
            "DINEROD=$<TARGET_FILE:dinerod>"
            ${CMAKE_CURRENT_SOURCE_DIR}/test_wallet_createrawtransaction_scriptpubkey_outputs.sh
    )
    set_tests_properties(WalletCreateRawTransactionScriptPubKeyOutputs PROPERTIES
        LABELS "mempool-rawtx-smoke;wallet;rawtx;taproot;scriptpubkey"
        TIMEOUT 180
    )
    message(STATUS "  ✓ WalletCreateRawTransactionScriptPubKeyOutputs registered")
endif()

add_test(
    NAME CovenantWalletMultinodeLifecycle
    COMMAND ${CMAKE_COMMAND} -E env
        "DINEROD=$<TARGET_FILE:dinerod>"
        ${CMAKE_CURRENT_SOURCE_DIR}/test_covenant_wallet_multinode_lifecycle.sh
)
set_tests_properties(CovenantWalletMultinodeLifecycle PROPERTIES
    LABELS "integration;covenant;wallet;rpc;multinode;relay;restart;reorg;regtest;serial"
    TIMEOUT 360
    RUN_SERIAL TRUE
)
message(STATUS "  ✓ CovenantWalletMultinodeLifecycle registered unconditionally")

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_wallet_listunspent_excludes_mempool_spent.sh)
    add_test(
        NAME WalletListUnspentExcludesMempoolSpent
        COMMAND ${CMAKE_COMMAND} -E env
            "DINEROD=$<TARGET_FILE:dinerod>"
            ${CMAKE_CURRENT_SOURCE_DIR}/test_wallet_listunspent_excludes_mempool_spent.sh
    )
    set_tests_properties(WalletListUnspentExcludesMempoolSpent PROPERTIES
        LABELS "rpc-smoke;rpc;wallet;mempool;rawtx;listunspent"
        TIMEOUT 180
    )
    message(STATUS "  ✓ WalletListUnspentExcludesMempoolSpent registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_rbf_policy_reporting.sh)
    add_test(
        NAME RbfPolicyReporting
        COMMAND ${CMAKE_COMMAND} -E env
            "DINEROD=$<TARGET_FILE:dinerod>"
            ${CMAKE_CURRENT_SOURCE_DIR}/test_rbf_policy_reporting.sh
    )
    set_tests_properties(RbfPolicyReporting PROPERTIES
        LABELS "mempool-rawtx-smoke;rpc;mempool;rbf"
        TIMEOUT 120
    )
    message(STATUS "  ✓ RbfPolicyReporting registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_filter_commitment_activation_boundary.sh)
    add_test(
        NAME FilterCommitmentActivationBoundary
        COMMAND ${CMAKE_COMMAND} -E env
            "DINEROD=$<TARGET_FILE:dinerod>"
            ${CMAKE_CURRENT_SOURCE_DIR}/test_filter_commitment_activation_boundary.sh
    )
    set_tests_properties(FilterCommitmentActivationBoundary PROPERTIES
        LABELS "filter-commitment-smoke;mining;filters;activation;boundary"
        TIMEOUT 900
    )
    message(STATUS "  ✓ FilterCommitmentActivationBoundary registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_external_miner_coinbasetxn_filter_commitment.sh)
    add_test(
        NAME ExternalMinerCoinbaseTxnFilterCommitment
        COMMAND ${CMAKE_COMMAND} -E env
            "DINEROD=$<TARGET_FILE:dinerod>"
            ${CMAKE_CURRENT_SOURCE_DIR}/test_external_miner_coinbasetxn_filter_commitment.sh
    )
    set_tests_properties(ExternalMinerCoinbaseTxnFilterCommitment PROPERTIES
        LABELS "filter-commitment-smoke;mining;filters;external-miner;coinbase"
        TIMEOUT 1200
    )
    message(STATUS "  ✓ ExternalMinerCoinbaseTxnFilterCommitment registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_restart_churn_boringness.sh)
    add_test(
        NAME RestartChurnBoringnessGate
        COMMAND ${CMAKE_COMMAND} -E env PROFILE=gate ${CMAKE_CURRENT_SOURCE_DIR}/test_restart_churn_boringness.sh
    )
    set_tests_properties(RestartChurnBoringnessGate PROPERTIES
        LABELS "integration;soak;restart;churn"
        TIMEOUT 900
    )
    message(STATUS "  ✓ RestartChurnBoringnessGate registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_bridge_csn_historical_spend_relay.sh)
    add_test(
        NAME BridgeCsnHistoricalSpendRelay
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_bridge_csn_historical_spend_relay.sh
    )
    set_tests_properties(BridgeCsnHistoricalSpendRelay PROPERTIES
        LABELS "integration;p2p;utreexo;bridge;csn;historical-spend"
        TIMEOUT 240
    )
    message(STATUS "  ✓ BridgeCsnHistoricalSpendRelay registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_csn_bridge_assisted_spend_flow.sh)
    add_test(
        NAME CsnBridgeAssistedSpendFlow
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_csn_bridge_assisted_spend_flow.sh
    )
    set_tests_properties(CsnBridgeAssistedSpendFlow PROPERTIES
        LABELS "integration;wallet;utreexo;bridge;csn;spend"
        TIMEOUT 300
    )
    message(STATUS "  ✓ CsnBridgeAssistedSpendFlow registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_csn_spend_reorg_reconciliation.sh)
    add_test(
        NAME CsnSpendReorgReconciliation
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_csn_spend_reorg_reconciliation.sh
    )
    set_tests_properties(CsnSpendReorgReconciliation PROPERTIES
        LABELS "integration;wallet;utreexo;bridge;csn;reorg"
        TIMEOUT 360
    )
    message(STATUS "  ✓ CsnSpendReorgReconciliation registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/utreexo_checkpoint_mutator.cpp)
    add_executable(utreexo_checkpoint_mutator
        utreexo_checkpoint_mutator.cpp
        ${CMAKE_SOURCE_DIR}/src/storage/chain_db.cpp
        ${CMAKE_SOURCE_DIR}/src/primitives/block.cpp
        ${CMAKE_SOURCE_DIR}/src/primitives/transaction.cpp
        ${CMAKE_SOURCE_DIR}/src/primitives/transaction_serializer.cpp
        ${CMAKE_SOURCE_DIR}/src/primitives/uint256.cpp
        ${CMAKE_SOURCE_DIR}/src/consensus/undo.cpp
        ${CMAKE_SOURCE_DIR}/src/crypto/sha256.cpp
        ${CMAKE_SOURCE_DIR}/src/common/logger.cpp
        ${CMAKE_SOURCE_DIR}/src/common/json_logger.cpp
        ${CMAKE_SOURCE_DIR}/src/common/sha256d.cpp
        ${CMAKE_SOURCE_DIR}/src/consensus/chainwork.cpp
    )
    target_link_libraries(utreexo_checkpoint_mutator PRIVATE
        dinero_consensus
        dinero_crypto
        RocksDB::rocksdb
        jsoncpp_static
        OpenSSL::Crypto
        OpenSSL::SSL
        secp256k1
    )
    link_zstd_if_needed(utreexo_checkpoint_mutator)
    target_include_directories(utreexo_checkpoint_mutator PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
    )
    if(APPLE)
        target_link_libraries(utreexo_checkpoint_mutator PRIVATE
            "-framework Security"
            "-framework CoreFoundation"
        )
    endif()
    message(STATUS "  ✓ utreexo_checkpoint_mutator built")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/header_store_backlog_injector.cpp)
    add_executable(header_store_backlog_injector
        header_store_backlog_injector.cpp
        ${CMAKE_SOURCE_DIR}/src/consensus/header_chain.cpp
        ${CMAKE_SOURCE_DIR}/src/consensus/header_store.cpp
    )
    target_link_libraries(header_store_backlog_injector PRIVATE
        dinero_consensus
        dinero_wallet
        dinero_crypto
        RocksDB::rocksdb
        secp256k1
    )
    link_zstd_if_needed(header_store_backlog_injector)
    target_include_directories(header_store_backlog_injector PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
    )
    if(APPLE)
        target_link_libraries(header_store_backlog_injector PRIVATE
            "-framework Security"
            "-framework CoreFoundation"
        )
    endif()
    message(STATUS "  ✓ header_store_backlog_injector built")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/shielded_tip_marker_mutator.cpp)
    add_executable(shielded_tip_marker_mutator
        shielded_tip_marker_mutator.cpp
        ${CMAKE_SOURCE_DIR}/src/storage/chain_db.cpp
        ${CMAKE_SOURCE_DIR}/src/primitives/block.cpp
        ${CMAKE_SOURCE_DIR}/src/primitives/transaction.cpp
        ${CMAKE_SOURCE_DIR}/src/primitives/transaction_serializer.cpp
        ${CMAKE_SOURCE_DIR}/src/primitives/uint256.cpp
        ${CMAKE_SOURCE_DIR}/src/consensus/undo.cpp
        ${CMAKE_SOURCE_DIR}/src/common/logger.cpp
        ${CMAKE_SOURCE_DIR}/src/common/json_logger.cpp
        ${CMAKE_SOURCE_DIR}/src/common/sha256d.cpp
        ${CMAKE_SOURCE_DIR}/src/consensus/chainwork.cpp
    )
    target_link_libraries(shielded_tip_marker_mutator PRIVATE
        dinero_consensus
        dinero_crypto
        RocksDB::rocksdb
        jsoncpp_static
        OpenSSL::Crypto
        OpenSSL::SSL
        secp256k1
    )
    link_zstd_if_needed(shielded_tip_marker_mutator)
    target_include_directories(shielded_tip_marker_mutator PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
    )
    if(APPLE)
        target_link_libraries(shielded_tip_marker_mutator PRIVATE
            "-framework Security"
            "-framework CoreFoundation"
        )
    endif()
    message(STATUS "  ✓ shielded_tip_marker_mutator built")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/shielded_tip_marker_probe.cpp)
    add_executable(shielded_tip_marker_probe
        shielded_tip_marker_probe.cpp
        ${CMAKE_SOURCE_DIR}/src/storage/chain_db.cpp
        ${CMAKE_SOURCE_DIR}/src/storage/block_storage.cpp
        ${CMAKE_SOURCE_DIR}/src/storage/disk_space_monitor.cpp
        ${CMAKE_SOURCE_DIR}/src/primitives/block.cpp
        ${CMAKE_SOURCE_DIR}/src/primitives/transaction.cpp
        ${CMAKE_SOURCE_DIR}/src/primitives/transaction_serializer.cpp
        ${CMAKE_SOURCE_DIR}/src/primitives/uint256.cpp
        ${CMAKE_SOURCE_DIR}/src/consensus/undo.cpp
        ${CMAKE_SOURCE_DIR}/src/common/logger.cpp
        ${CMAKE_SOURCE_DIR}/src/common/json_logger.cpp
        ${CMAKE_SOURCE_DIR}/src/common/sha256d.cpp
        ${CMAKE_SOURCE_DIR}/src/consensus/chainwork.cpp
    )
    target_link_libraries(shielded_tip_marker_probe PRIVATE
        dinero_consensus
        dinero_crypto
        dinero_shielded
        RocksDB::rocksdb
        jsoncpp_static
        sqlite3
        OpenSSL::Crypto
        OpenSSL::SSL
        secp256k1
    )
    link_zstd_if_needed(shielded_tip_marker_probe)
    target_include_directories(shielded_tip_marker_probe PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
    )
    if(APPLE)
        target_link_libraries(shielded_tip_marker_probe PRIVATE
            "-framework Security"
            "-framework CoreFoundation"
        )
    endif()
    message(STATUS "  ✓ shielded_tip_marker_probe built")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/shielded_tx_builder.cpp)
    add_executable(shielded_tx_builder
        shielded_tx_builder.cpp
        ${CMAKE_SOURCE_DIR}/src/primitives/transaction.cpp
        ${CMAKE_SOURCE_DIR}/src/primitives/transaction_serializer.cpp
        ${CMAKE_SOURCE_DIR}/src/primitives/uint256.cpp
        ${CMAKE_SOURCE_DIR}/src/common/sha256d.cpp
    )
    target_link_libraries(shielded_tx_builder PRIVATE
        dinero_consensus
        dinero_crypto
        dinero_shielded
        jsoncpp_static
        OpenSSL::Crypto
        OpenSSL::SSL
        secp256k1
    )
    target_include_directories(shielded_tx_builder PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
    )
    if(APPLE)
        target_link_libraries(shielded_tx_builder PRIVATE
            "-framework Security"
            "-framework CoreFoundation"
        )
    endif()
    message(STATUS "  ✓ shielded_tx_builder built")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_csn_contaminated_checkpoint_recovery.sh)
    add_test(
        NAME CsnContaminatedCheckpointRecovery
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_csn_contaminated_checkpoint_recovery.sh
    )
    set_tests_properties(CsnContaminatedCheckpointRecovery PROPERTIES
        LABELS "integration;utreexo;csn;checkpoint;recovery;upgrade"
        TIMEOUT 420
    )
    message(STATUS "  ✓ CsnContaminatedCheckpointRecovery registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_shielded_rpc_shield_e2e.sh)
    add_test(
        NAME ShieldedRpcShieldEndToEnd
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_shielded_rpc_shield_e2e.sh
    )
    set_tests_properties(ShieldedRpcShieldEndToEnd PROPERTIES
        ENVIRONMENT "DINEROD=$<TARGET_FILE:dinerod>"
        LABELS "integration;shielded;rpc;wallet;phase3-wave3b"
        TIMEOUT 240
    )
    message(STATUS "  ✓ ShieldedRpcShieldEndToEnd registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_shielded_rpc_unshield_e2e.sh)
    add_test(
        NAME ShieldedRpcUnshieldEndToEnd
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_shielded_rpc_unshield_e2e.sh
    )
    set_tests_properties(ShieldedRpcUnshieldEndToEnd PROPERTIES
        ENVIRONMENT "DINEROD=$<TARGET_FILE:dinerod>"
        LABELS "integration;shielded;rpc;wallet;phase3-wave3c"
        TIMEOUT 240
    )
    message(STATUS "  ✓ ShieldedRpcUnshieldEndToEnd registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_shielded_rpc_transfer_e2e.sh)
    add_test(
        NAME ShieldedRpcTransferEndToEnd
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_shielded_rpc_transfer_e2e.sh
    )
    set_tests_properties(ShieldedRpcTransferEndToEnd PROPERTIES
        ENVIRONMENT "DINEROD=$<TARGET_FILE:dinerod>"
        LABELS "integration;shielded;rpc;wallet;phase3-wave3d"
        TIMEOUT 240
    )
    message(STATUS "  ✓ ShieldedRpcTransferEndToEnd registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_shielded_rpc_transfer_multi_e2e.sh)
    add_test(
        NAME ShieldedRpcTransferMultiEndToEnd
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_shielded_rpc_transfer_multi_e2e.sh
    )
    set_tests_properties(ShieldedRpcTransferMultiEndToEnd PROPERTIES
        ENVIRONMENT "DINEROD=$<TARGET_FILE:dinerod>"
        LABELS "integration;shielded;rpc;wallet;phase3-wave3e"
        TIMEOUT 240
    )
    message(STATUS "  ✓ ShieldedRpcTransferMultiEndToEnd registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_shielded_rpc_getaddress.sh)
    add_test(
        NAME ShieldedRpcGetAddress
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_shielded_rpc_getaddress.sh
    )
    set_tests_properties(ShieldedRpcGetAddress PROPERTIES
        ENVIRONMENT "DINEROD=$<TARGET_FILE:dinerod>"
        LABELS "integration;shielded;rpc;wallet;phase5-wave3c"
        TIMEOUT 120
    )
    message(STATUS "  ✓ ShieldedRpcGetAddress registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_shielded_rpc_transfer_addressed_e2e.sh)
    add_test(
        NAME ShieldedRpcTransferAddressedEndToEnd
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_shielded_rpc_transfer_addressed_e2e.sh
    )
    set_tests_properties(ShieldedRpcTransferAddressedEndToEnd PROPERTIES
        ENVIRONMENT "DINEROD=$<TARGET_FILE:dinerod>"
        LABELS "integration;shielded;rpc;wallet;phase5-wave3d"
        TIMEOUT 240
    )
    message(STATUS "  ✓ ShieldedRpcTransferAddressedEndToEnd registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_shielded_rpc_transfer_addressed_detect_e2e.sh)
    add_test(
        NAME ShieldedRpcTransferAddressedDetectEndToEnd
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_shielded_rpc_transfer_addressed_detect_e2e.sh
    )
    set_tests_properties(ShieldedRpcTransferAddressedDetectEndToEnd PROPERTIES
        ENVIRONMENT "DINEROD=$<TARGET_FILE:dinerod>"
        LABELS "integration;shielded;rpc;wallet;phase5-wave3e"
        TIMEOUT 240
    )
    message(STATUS "  ✓ ShieldedRpcTransferAddressedDetectEndToEnd registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_shielded_outputs_rpc_feed.sh)
    add_test(
        NAME ShieldedOutputsRpcFeed
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_shielded_outputs_rpc_feed.sh
    )
    set_tests_properties(ShieldedOutputsRpcFeed PROPERTIES
        ENVIRONMENT "DINEROD=$<TARGET_FILE:dinerod>"
        LABELS "integration;shielded;rpc;m2"
        TIMEOUT 240
    )
    message(STATUS "  ✓ ShieldedOutputsRpcFeed registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_shielded_tip_marker_restart_equivalence.sh)
    add_test(
        NAME ShieldedTipMarkerRestartEquivalence
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_shielded_tip_marker_restart_equivalence.sh
    )
    set_tests_properties(ShieldedTipMarkerRestartEquivalence PROPERTIES
        LABELS "integration;restart;shielded;canonicality;shielded-canonical-recovery"
        TIMEOUT 300
    )
    message(STATUS "  ✓ ShieldedTipMarkerRestartEquivalence registered")
endif()

# Forest checkpoint delta campaign phase 1: every-N checkpoint writer
# (docs/design/forest-checkpoint-deltas.md)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_utreexo_checkpoint_interval.sh)
    add_test(
        NAME UtreexoCheckpointInterval
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_utreexo_checkpoint_interval.sh
    )
    set_tests_properties(UtreexoCheckpointInterval PROPERTIES
        LABELS "integration;daemon;utreexo;forest-checkpoint-deltas"
        TIMEOUT 300
    )
    message(STATUS "  ✓ UtreexoCheckpointInterval registered")
endif()

# Forest checkpoint delta campaign phase 2: restart via checkpoint + UD
# sidecar replay, plus the unified-batch crash boundary
# (docs/design/forest-checkpoint-deltas.md)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_utreexo_delta_replay_restart.sh)
    add_test(
        NAME UtreexoDeltaReplayRestart
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_utreexo_delta_replay_restart.sh
    )
    set_tests_properties(UtreexoDeltaReplayRestart PROPERTIES
        LABELS "integration;daemon;utreexo;restart;forest-checkpoint-deltas"
        TIMEOUT 420
    )
    message(STATUS "  ✓ UtreexoDeltaReplayRestart registered")
endif()

# Forest checkpoint delta campaign phase 3: mixed-signal restart torture
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_utreexo_delta_torture.sh)
    add_test(
        NAME UtreexoDeltaTorture
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_utreexo_delta_torture.sh
    )
    set_tests_properties(UtreexoDeltaTorture PROPERTIES
        LABELS "integration;daemon;utreexo;restart;torture;forest-checkpoint-deltas"
        TIMEOUT 900
    )
    message(STATUS "  ✓ UtreexoDeltaTorture registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_shielded_daemon_restart_equivalence.sh)
    add_test(
        NAME ShieldedDaemonRestartEquivalence
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_shielded_daemon_restart_equivalence.sh
    )
    set_tests_properties(ShieldedDaemonRestartEquivalence PROPERTIES
        LABELS "integration;restart;shielded;daemon;canonicality;phase2-shielded-canonical-recovery"
        TIMEOUT 420
    )
    message(STATUS "  ✓ ShieldedDaemonRestartEquivalence registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_shielded_tip_persist_restart_equivalence.sh)
    add_test(
        NAME ShieldedTipPersistRestartEquivalence
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_shielded_tip_persist_restart_equivalence.sh
    )
    set_tests_properties(ShieldedTipPersistRestartEquivalence PROPERTIES
        LABELS "integration;restart;shielded;daemon;canonicality;phase2-shielded-canonical-recovery"
        TIMEOUT 420
    )
    message(STATUS "  ✓ ShieldedTipPersistRestartEquivalence registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_shielded_reorg_disconnect_restart_equivalence.sh)
    add_test(
        NAME ShieldedReorgDisconnectRestartEquivalence
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_shielded_reorg_disconnect_restart_equivalence.sh
    )
    set_tests_properties(ShieldedReorgDisconnectRestartEquivalence PROPERTIES
        LABELS "integration;restart;shielded;daemon;canonicality;phase2-shielded-canonical-recovery"
        TIMEOUT 480
    )
    message(STATUS "  ✓ ShieldedReorgDisconnectRestartEquivalence registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_shielded_reorg_second_restart_invalidity_equivalence.sh)
    add_test(
        NAME ShieldedReorgSecondRestartInvalidityEquivalence
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_shielded_reorg_second_restart_invalidity_equivalence.sh
    )
    set_tests_properties(ShieldedReorgSecondRestartInvalidityEquivalence PROPERTIES
        LABELS "integration;restart;shielded;daemon;canonicality;phase2-shielded-canonical-recovery"
        TIMEOUT 540
    )
    message(STATUS "  ✓ ShieldedReorgSecondRestartInvalidityEquivalence registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_bridge_csn_historical_range_soak.sh)
    add_test(
        NAME BridgeCsnHistoricalRangeSoak
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_bridge_csn_historical_range_soak.sh
    )
    set_tests_properties(BridgeCsnHistoricalRangeSoak PROPERTIES
        LABELS "integration;p2p;utreexo;bridge;csn;historical;soak"
        TIMEOUT 600
    )
    message(STATUS "  ✓ BridgeCsnHistoricalRangeSoak registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_csn_sync_live_spend_traffic.sh)
    add_test(
        NAME CsnSyncLiveSpendTraffic
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_csn_sync_live_spend_traffic.sh
    )
    set_tests_properties(CsnSyncLiveSpendTraffic PROPERTIES
        LABELS "integration;utreexo;bridge;csn;live-traffic;sync"
        TIMEOUT 540
    )
    message(STATUS "  ✓ CsnSyncLiveSpendTraffic registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_csn_archival_mainnet_replay.sh)
    add_test(
        NAME CsnArchivalMainnetReplay
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_csn_archival_mainnet_replay.sh
    )
    set_tests_properties(CsnArchivalMainnetReplay PROPERTIES
        LABELS "integration;utreexo;bridge;csn;external;mainnet;archival;soak"
        TIMEOUT 2700
    )
    message(STATUS "  ✓ CsnArchivalMainnetReplay registered")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_utreexo_mempool_canonical_separation.sh)
    add_test(
        NAME UtreexoMempoolCanonicalSeparation
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_utreexo_mempool_canonical_separation.sh
    )
    set_tests_properties(UtreexoMempoolCanonicalSeparation PROPERTIES
        LABELS "integration;utreexo;mempool;restart;wallet;canonical-state"
        TIMEOUT 300
    )
    message(STATUS "  ✓ UtreexoMempoolCanonicalSeparation registered")
endif()

# Phase 2 of the shielded reorg invertibility plan
# (docs/specs/shielded_reorg_invertibility_audit.md). Walks Connect /
# Disconnect / Connect across a regtest reorg and asserts the
# composite (utreexo + shielded tree + nullifiers + anchor history)
# state hash returns to its original value.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_shielded_reorg_invertibility.sh)
    add_test(
        NAME ShieldedReorgInvertibility
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_shielded_reorg_invertibility.sh
    )
    set_tests_properties(ShieldedReorgInvertibility PROPERTIES
        LABELS "integration;shielded;reorg;invertibility;canonical-state"
        TIMEOUT 300
    )
    message(STATUS "  ✓ ShieldedReorgInvertibility registered")
endif()

# Shielded epoch reset (hard-fork cutover) boundary acceptance test. Drives a
# regtest daemon (cutover forced low via --consensus-shielded-epoch-reset-height)
# across the reset with a populated pre-cutover pool, then asserts: pre-cutover
# notes are unspendable; reorg-across-H is a perfect inverse; restart rehydrates
# the identical state (the ChainDB nullifier purge persisted — no resurrection);
# and reindex reproduces the live shieldedStateHash (no live-vs-reindex split).
# Mines >100 blocks (wallet coinbase maturity) + reindex, so allow a long timeout.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_shielded_epoch_reset_boundary.sh)
    add_test(
        NAME ShieldedEpochResetBoundary
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_shielded_epoch_reset_boundary.sh
    )
    set_tests_properties(ShieldedEpochResetBoundary PROPERTIES
        LABELS "integration;shielded;reorg;restart;reindex;epoch-reset;canonical-state"
        TIMEOUT 600
    )
    message(STATUS "  ✓ ShieldedEpochResetBoundary registered")
endif()

# Shielded epoch reset — STATELESS / CSN path acceptance test. A bridge node
# mines across the cutover and a stateless (--utreexo-stateless) node syncs
# across H and restarts past it, asserting brick-safety: it does not stall at the
# cutover (the reset applies on the stateless connect path) and restarts cleanly
# past H (the frontier/anchor blobs persist; the resurrection guard does not
# brick it). Two-node sync + restart, so allow a long timeout.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_shielded_epoch_reset_stateless.sh)
    add_test(
        NAME ShieldedEpochResetStateless
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_shielded_epoch_reset_stateless.sh
    )
    set_tests_properties(ShieldedEpochResetStateless PROPERTIES
        LABELS "integration;shielded;stateless;csn;restart;epoch-reset;canonical-state"
        TIMEOUT 600
    )
    message(STATUS "  ✓ ShieldedEpochResetStateless registered")
endif()

# CSN shielded-spend sync regression: a stateless node must build the shielded
# tree/anchors/nullifiers during sync so it can validate a shielded spend
# (previously it early-returned before the shielded-apply section and failed
# anchor-invalid). Bridge mines a shield+unshield chain; the stateless node syncs
# it and its shieldedStateHash must match the full node's. Two-node, long timeout.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_csn_shielded_spend_sync.sh)
    add_test(
        NAME CSNShieldedSpendSync
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_csn_shielded_spend_sync.sh
    )
    set_tests_properties(CSNShieldedSpendSync PROPERTIES
        LABELS "integration;shielded;stateless;csn;canonical-state"
        TIMEOUT 600
    )
    message(STATUS "  ✓ CSNShieldedSpendSync registered")
endif()

# CSN ConnectTip stateless-recovery shielded-apply (#356). Deterministically
# reaches ConnectTip's crash-recovery/replay branch (guard current_commitment ==
# accumulator_root_before, INFO "Stateless replay path") via the regtest-only
# fault-injection hook (DINERO_DEBUG_ABORT_AFTER_STORE_HEIGHT): a CSN stores a
# shielded-bearing block H (an unshield/nullifier block) then _exit(70) before
# connecting it; on restart ActivateBestChain connects H through the recovery
# branch. Asserts the branch fired for H, and that the recovery-applied shielded
# state converges with the full node (shieldedStateHash) and accepts a subsequent
# shielded spend. Neuter-proven (reverting the branch's apply diverges/stalls).
# Two-node bridge+CSN, restart, long timeout.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_csn_recovery_shielded_apply.sh)
    add_test(
        NAME CSNRecoveryShieldedApply
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_csn_recovery_shielded_apply.sh
    )
    set_tests_properties(CSNRecoveryShieldedApply PROPERTIES
        LABELS "integration;shielded;stateless;csn;recovery;crash;canonical-state"
        TIMEOUT 900
    )
    message(STATUS "  ✓ CSNRecoveryShieldedApply registered")
endif()

# CSN shielded REORG invertibility (the e2e teeth). A stateless node must survive
# a reorg whose disconnected AND replayed branches carry real shielded spends and
# end byte-identical to a full node. Leg A: reorg invertibility (CSN sh==bridge);
# Leg B: double-spend of a note whose nullifier entered CSN state via a replayed
# block is rejected (nullifier-duplicate); Leg C: reorg across the epoch-reset
# height converges (gates the bookkeeping reset RecordRoot fix); Leg D: a second reorg
# disconnects replay-connected blocks with no "Missing undo data". Uses the
# deterministic shielded_tx_builder so a note's nullifier is reproducible across
# branches. Two-node bridge+CSN, long timeout.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_csn_shielded_reorg_invertibility.sh)
    add_test(
        NAME CSNShieldedReorgInvertibility
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_csn_shielded_reorg_invertibility.sh
    )
    set_tests_properties(CSNShieldedReorgInvertibility PROPERTIES
        LABELS "integration;shielded;stateless;csn;reorg;invertibility;canonical-state"
        TIMEOUT 900
    )
    message(STATUS "  ✓ CSNShieldedReorgInvertibility registered")
endif()

# Companion to ShieldedReorgInvertibility. Same property assertion
# (composite state hash invariance) but exercised through the copy-
# then-reindex code path that the in-place reindex test cannot
# reach. See test header comment for explicit scope: catches
# regressions in the new-binary reindex path on chains the new
# binary builds; does NOT pin the actual LA 2026-04-28 9291-style
# pre-fix-drift failure (audit gap #10 — needs a canned drifted
# fixture or a debug-only "inject v2 serialize" knob).
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_reindex_copied_datadir.sh)
    add_test(
        NAME ReindexCopiedDatadir
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_reindex_copied_datadir.sh
    )
    set_tests_properties(ReindexCopiedDatadir PROPERTIES
        LABELS "integration;reindex;reorg;invertibility;canonical-state"
        TIMEOUT 360
    )
    message(STATUS "  ✓ ReindexCopiedDatadir registered")
endif()

# Commit #6 of the offline --rebuild-undo-range series: end-to-end
# integration test pinning the operator-visible properties of
# `--rebuild-undo-range` against a real regtest dinerod.
#   * dry-run maps holes but writes nothing (rev*.dat sha256 unchanged)
#   * write mode rebuilds holes (manifest reports rebuilt; rev*.dat grew)
#   * rebuilt undo survives daemon restart + chain advancement
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_rebuild_undo_range.sh)
    add_test(
        NAME RebuildUndoRange
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_rebuild_undo_range.sh
    )
    set_tests_properties(RebuildUndoRange PROPERTIES
        LABELS "integration;reindex;undo-rebuild;chainstate-recovery"
        TIMEOUT 240
    )
    message(STATUS "  ✓ RebuildUndoRange registered")
endif()

# Commit #7 of the offline --rebuild-undo-range series: crash oracles
# pinning the interruption guarantees of the rebuilder's LIVE write
# path. Two named hooks fire on regtest only:
#   * rebuild_after_live_writeUndo_before_metadata_commit  → no
#     dishonest metadata; orphan bytes in rev*.dat are harmless
#   * rebuild_after_live_metadata_commit                   → durable
#     consistent undo; restart sees the rebuilt bytes cleanly
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_rebuild_undo_range_crash_oracles.sh)
    add_test(
        NAME RebuildUndoRangeCrashOracles
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_rebuild_undo_range_crash_oracles.sh
    )
    set_tests_properties(RebuildUndoRangeCrashOracles PROPERTIES
        LABELS "integration;reindex;undo-rebuild;chainstate-recovery;crash-oracle"
        TIMEOUT 240
    )
    message(STATUS "  ✓ RebuildUndoRangeCrashOracles registered")
endif()

# Phase 3a D3 expanded matrix. ShieldedReorgInvertibility runs in
# four registered configurations:
#   _AtomicPersistOff  — flag off (default ShieldedReorgInvertibility above
#                         already covers this; registered explicitly here so
#                         CI logs distinguish the variants by name)
#   _AtomicPersistOn   — flag on for the whole chain
#   _AtomicPersistToggleOnToOff — start with flag on, mid-chain restart with
#                                  flag off
#   _AtomicPersistToggleOffToOn — start with flag off, mid-chain restart with
#                                  flag on
# Each is a separate registered target so a regression in any one
# configuration trips CI loud independently of the others.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_shielded_reorg_invertibility.sh)
    add_test(
        NAME ShieldedReorgInvertibility_AtomicPersistOn
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_shielded_reorg_invertibility.sh
    )
    set_tests_properties(ShieldedReorgInvertibility_AtomicPersistOn PROPERTIES
        ENVIRONMENT "ATOMIC_PERSIST=1"
        LABELS "integration;shielded;reorg;invertibility;canonical-state;phase3a"
        TIMEOUT 300
    )
    message(STATUS "  ✓ ShieldedReorgInvertibility_AtomicPersistOn registered")

    add_test(
        NAME ShieldedReorgInvertibility_AtomicPersistToggleOnToOff
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_shielded_reorg_invertibility.sh
    )
    set_tests_properties(ShieldedReorgInvertibility_AtomicPersistToggleOnToOff PROPERTIES
        ENVIRONMENT "ATOMIC_PERSIST=1;ATOMIC_PERSIST_TOGGLE=1"
        LABELS "integration;shielded;reorg;invertibility;canonical-state;phase3a"
        TIMEOUT 300
    )
    message(STATUS "  ✓ ShieldedReorgInvertibility_AtomicPersistToggleOnToOff registered")

    add_test(
        NAME ShieldedReorgInvertibility_AtomicPersistToggleOffToOn
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_shielded_reorg_invertibility.sh
    )
    set_tests_properties(ShieldedReorgInvertibility_AtomicPersistToggleOffToOn PROPERTIES
        ENVIRONMENT "ATOMIC_PERSIST=0;ATOMIC_PERSIST_TOGGLE=1"
        LABELS "integration;shielded;reorg;invertibility;canonical-state;phase3a"
        TIMEOUT 300
    )
    message(STATUS "  ✓ ShieldedReorgInvertibility_AtomicPersistToggleOffToOn registered")
endif()

# Phase 3a flag-on coverage for the copy-then-reindex path. The
# baseline ReindexCopiedDatadir target above runs with the flag
# default-off; this sibling forces it on so a regression in the
# copy-reindex path under the new code trips CI loud.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_reindex_copied_datadir.sh)
    add_test(
        NAME ReindexCopiedDatadir_AtomicPersistOn
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_reindex_copied_datadir.sh
    )
    set_tests_properties(ReindexCopiedDatadir_AtomicPersistOn PROPERTIES
        ENVIRONMENT "ATOMIC_PERSIST=1"
        LABELS "integration;reindex;reorg;invertibility;canonical-state;phase3a"
        TIMEOUT 360
    )
    message(STATUS "  ✓ ReindexCopiedDatadir_AtomicPersistOn registered")
endif()

# Phase 3a D4 contract test — ConsensusWriteBatch destructor must
# never fire its leak panic during normal Commit-side operation.
# (The harder half — actually leak the batch and assert the
# expected response per build mode — needs a debug knob that lands
# in phase 3b alongside the §3.1 journal row.)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_consensus_write_batch_leak.sh)
    add_test(
        NAME ConsensusWriteBatchLeak
        COMMAND ${CMAKE_COMMAND} -E env
            "DINEROD=$<TARGET_FILE:dinerod>"
            ${CMAKE_CURRENT_SOURCE_DIR}/test_consensus_write_batch_leak.sh
    )
    set_tests_properties(ConsensusWriteBatchLeak PROPERTIES
        LABELS "core-heavy-smoke;consensus-batch-smoke;reorg;invertibility;phase3a"
        TIMEOUT 240
    )
    message(STATUS "  ✓ ConsensusWriteBatchLeak registered")
endif()

# Phase 3b step 5 — crash-injection coverage for the §1 atomic-unit
# law in ConnectTip's unified batch. Crashes at two boundaries
# (pre-batch and post-batch) and asserts every container's height
# is aligned at restart, with safe mode NOT active. Two ctests:
# baseline (flag-off) and AtomicPersistOn (flag-on) so a regression
# in the journal-row atomicity trips CI independently.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_unified_batch_atomicity.sh)
    add_test(
        NAME UnifiedBatchAtomicity
        COMMAND ${CMAKE_COMMAND} -E env
            "DINEROD=$<TARGET_FILE:dinerod>"
            ${CMAKE_CURRENT_SOURCE_DIR}/test_unified_batch_atomicity.sh
    )
    set_tests_properties(UnifiedBatchAtomicity PROPERTIES
        ENVIRONMENT "ATOMIC_PERSIST=0"
        LABELS "core-heavy-smoke;integration;restart;chainstate;canonicality;phase3b;atomic-unit"
        TIMEOUT 360
    )
    message(STATUS "  ✓ UnifiedBatchAtomicity registered")

    add_test(
        NAME UnifiedBatchAtomicity_AtomicPersistOn
        COMMAND ${CMAKE_COMMAND} -E env
            "DINEROD=$<TARGET_FILE:dinerod>"
            ${CMAKE_CURRENT_SOURCE_DIR}/test_unified_batch_atomicity.sh
    )
    set_tests_properties(UnifiedBatchAtomicity_AtomicPersistOn PROPERTIES
        ENVIRONMENT "ATOMIC_PERSIST=1"
        LABELS "core-heavy-smoke;integration;restart;chainstate;canonicality;phase3b;atomic-unit"
        TIMEOUT 360
    )
    message(STATUS "  ✓ UnifiedBatchAtomicity_AtomicPersistOn registered")
endif()

# Phase 3b precondition for step 6 (recovery-maze deletion). Crashes
# at the "after_unified_batch_before_frontier_write" hook, exercising
# the boundary between the unified WriteBatch commit (carrying the
# ShieldedTipMarker, frontier blob, and — flag-on — journal row) and
# the legacy std::ofstream flat-file frontier write. With option 1
# (frontier blob in the unified batch), startup loads the frontier
# from ChainDB and the legacy flat file is a fallback only — the
# marker-vs-frontier gap that motivated the test is closed
# structurally, so both flag-off and flag-on must pass.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_frontier_write_gap_recovery.sh)
    add_test(
        NAME FrontierWriteGapRecovery
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_frontier_write_gap_recovery.sh
    )
    set_tests_properties(FrontierWriteGapRecovery PROPERTIES
        ENVIRONMENT "ATOMIC_PERSIST=0"
        LABELS "integration;restart;chainstate;canonicality;phase3b;recovery-precondition"
        TIMEOUT 240
    )
    message(STATUS "  ✓ FrontierWriteGapRecovery registered")

    add_test(
        NAME FrontierWriteGapRecovery_AtomicPersistOn
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_frontier_write_gap_recovery.sh
    )
    set_tests_properties(FrontierWriteGapRecovery_AtomicPersistOn PROPERTIES
        ENVIRONMENT "ATOMIC_PERSIST=1"
        LABELS "integration;restart;chainstate;canonicality;phase3b;recovery-precondition"
        TIMEOUT 240
    )
    message(STATUS "  ✓ FrontierWriteGapRecovery_AtomicPersistOn registered")
endif()

# Audit gap #10 — synthesized LA-9291-style fixture. Uses the
# DINERO_FOREST_SERIALIZE_LEGACY_V2 debug knob in
# utreexo_accumulator.cpp to plant a v2-format on-disk forest
# checkpoint, then runs the new (v3 reader) binary's
# --reindex-chainstate over a copy of that datadir and asserts the
# composite reorg state hash is byte-equal. This is the
# closest reproducible regression for the actual LA failure
# without shipping a real mainnet datadir as a test fixture.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_reindex_legacy_v2_forest_fixture.sh)
    add_test(
        NAME ReindexLegacyV2ForestFixture
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_reindex_legacy_v2_forest_fixture.sh
    )
    set_tests_properties(ReindexLegacyV2ForestFixture PROPERTIES
        LABELS "integration;reindex;reorg;invertibility;canonical-state;gap10"
        TIMEOUT 360
    )
    message(STATUS "  ✓ ReindexLegacyV2ForestFixture registered")
endif()

# ═══════════════════════════════════════════════════════════════════════
# Utreexo Mining E2E Test - REMOVED (API changed to IConsensusUTXOSet)
# ═══════════════════════════════════════════════════════════════════════

# ═══════════════════════════════════════════════════════════════════════
# Cold-Start Consensus Validation Test
# ═══════════════════════════════════════════════════════════════════════

message(STATUS "Configuring cold-start consensus test...")

if(CURL_FOUND AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/cold_start_test.cpp)
    add_executable(test_cold_start
        cold_start_test.cpp
    )

    target_link_libraries(test_cold_start PRIVATE
        e2e_test_harness
        jsoncpp_static
        CURL::libcurl
        GTest::gtest
    )

    target_include_directories(test_cold_start PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/third_party/jsoncpp/include
    )

    # Registration is UNCONDITIONAL (issue #428) — see the MiningPolicyE2E block
    # above for why the old configure-time find_program gate could never fire in
    # CI.
    #
    # NOTE: this deliberately carries NO WORKING_DIRECTORY. PR #427 added one so
    # the harness's relative "./dinerod" would resolve; that mechanism is now
    # superseded. The harness takes the absolute daemon path from the DINEROD
    # environment variable below, so the test no longer depends on the process
    # working directory at all. Do not reintroduce a CWD assumption here.
    add_dependencies(test_cold_start dinerod)
    add_test(NAME ColdStartConsensus COMMAND test_cold_start)
    set_tests_properties(ColdStartConsensus PROPERTIES
        LABELS "e2e;consensus;cold-start;e2e-daemon"
        ENVIRONMENT "DINEROD=$<TARGET_FILE:dinerod>"
        TIMEOUT 180
    )
    message(STATUS "  ✓ ColdStartConsensus registered (daemon: $<TARGET_FILE:dinerod>)")
endif()

# Disable the POSIX-only .sh ctest entries registered above when building on
# native Windows MSVC. cmd.exe has no /bin/bash on PATH, so otherwise ctest
# reports each as BAD_COMMAND and the gate fills with noise. Tests still run
# on Linux / macOS / MinGW / Cygwin. Set DISABLED inside the same subdirectory
# they were registered in — TEST queries from a parent scope don't always
# resolve names added via add_subdirectory in older CMake.
if(WIN32 AND NOT MINGW AND NOT CYGWIN)
    set(_dinero_integration_posix_only
        ParallelBlockDownloadTargeted LaggingPeerCatchupNoOrphans
        CrossSeedRestartReorgParity ConnectTipRestartEquivalence
        D3FullUserTxMissingUndo UndoMetadataRestamp
        ReindexChainstateUtreexoEquivalence ReindexPromotionRestartEquivalence
        InvalidityRestartSticky InvalidityCrashRestartEquivalence
        ReconsiderCrashRestartEquivalence InvalidityImportEquivalence
        TipPersistRestartEquivalence RecoveryMarkerRestartEquivalence
        ReorgMarkerAlignedRestartEquivalence InterruptedReorgFailSafe
        HeaderCFRestartEquivalence HeaderFilterReplayEquivalence
        PositionIndexRestartEquivalence HeaderBacklogRestartEquivalence
        CompactBlockRelayE2E PeerMetadataRuntimeIdentity
        ChainIdentitySync P2PServiceNetworkControl
        RpcStopCleanShutdown DpiHeaderFilterProofFlow
        AddressBalanceMempoolOverlay AddressIndexCrossHRP
        AddressParentChildMempoolLifecycle
        TaprootSignRawTransactionRbfOverlay
        TaprootSignRawTransactionRbfConfirmation
        ParentChildRbfReplacementWithMempoolParent
        WalletCreateRawTransactionScriptPubKeyOutputs
        CovenantWalletMultinodeLifecycle
        WalletListUnspentExcludesMempoolSpent
        RbfPolicyReporting FilterCommitmentActivationBoundary
        ExternalMinerCoinbaseTxnFilterCommitment
        RestartChurnBoringnessGate
        BridgeCsnHistoricalSpendRelay CsnBridgeAssistedSpendFlow
        CsnSpendReorgReconciliation CsnContaminatedCheckpointRecovery
        ShieldedRpcShieldEndToEnd ShieldedRpcUnshieldEndToEnd
        ShieldedRpcTransferEndToEnd ShieldedRpcTransferMultiEndToEnd
        ShieldedRpcGetAddress ShieldedRpcTransferAddressedEndToEnd
        ShieldedRpcTransferAddressedDetectEndToEnd
        ShieldedTipMarkerRestartEquivalence
        ShieldedDaemonRestartEquivalence
        ShieldedTipPersistRestartEquivalence
        ShieldedReorgDisconnectRestartEquivalence
        ShieldedReorgSecondRestartInvalidityEquivalence
        BridgeCsnHistoricalRangeSoak CsnSyncLiveSpendTraffic
        CsnArchivalMainnetReplay UtreexoMempoolCanonicalSeparation
        ShieldedReorgInvertibility ReindexCopiedDatadir
        RebuildUndoRange RebuildUndoRangeCrashOracles
        ShieldedReorgInvertibility_AtomicPersistOn
        ShieldedReorgInvertibility_AtomicPersistToggleOnToOff
        ShieldedReorgInvertibility_AtomicPersistToggleOffToOn
        ReindexCopiedDatadir_AtomicPersistOn
        ConsensusWriteBatchLeak UnifiedBatchAtomicity
        UnifiedBatchAtomicity_AtomicPersistOn
        FrontierWriteGapRecovery FrontierWriteGapRecovery_AtomicPersistOn
        ReindexLegacyV2ForestFixture
    )
    set(_dinero_disabled_count 0)
    foreach(_t IN LISTS _dinero_integration_posix_only)
        if(TEST ${_t})
            set_tests_properties(${_t} PROPERTIES DISABLED TRUE)
            math(EXPR _dinero_disabled_count "${_dinero_disabled_count} + 1")
        endif()
    endforeach()
    message(STATUS "  ⊘ Disabled ${_dinero_disabled_count} POSIX-only .sh integration tests on native Windows")
endif()
