# Phase 30: Reorg Torture Tests
# NOTE: Disabled until consensus/chain_manager.h is implemented (uses interface only for now)
# TODO: Re-enable when ChainManager implementation is complete

# ═══════════════════════════════════════════════════════════════════════════
# CRITICAL: Balance Invariant Test
# Validates: wallet balance after reorg == balance after fresh rescan
# This is the fund safety invariant - if this fails, fund loss is possible
# ═══════════════════════════════════════════════════════════════════════════
# add_executable(test_balance_invariant test_balance_invariant.cpp)
#
# target_link_libraries(test_balance_invariant
#     dinero_consensus
#     dinero_chainstate
#     dinero_wallet
#     dinero_mining_core
#     dinero_common
#     dinero_crypto
#     rocksdb
#     jsoncpp_static
#     GTest::gtest
#     GTest::gtest_main
# )
#
# target_include_directories(test_balance_invariant PRIVATE
#     ${CMAKE_SOURCE_DIR}/include
#     ${CMAKE_SOURCE_DIR}/src
#     ${CMAKE_BINARY_DIR}
# )
#
# add_test(NAME BalanceInvariantTest COMMAND test_balance_invariant)

# add_executable(test_deep_reorg test_deep_reorg.cpp)
#
# target_link_libraries(test_deep_reorg
#     dinero_consensus
#     dinero_chainstate
#     dinero_wallet
#     dinero_mining_core
#     dinero_common
#     dinero_crypto
#     rocksdb
#     jsoncpp_static
# )
#
# target_include_directories(test_deep_reorg PRIVATE
#     ${CMAKE_SOURCE_DIR}/include
#     ${CMAKE_SOURCE_DIR}/src
#     ${CMAKE_BINARY_DIR}
# )
