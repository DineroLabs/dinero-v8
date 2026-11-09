# These targets previously compiled the mempool or one of its helpers.
# Keep declarations explicit, including consumers gated on other platforms.
target_link_libraries(dinero_core PUBLIC dinero_mempool)
target_link_libraries(dinero_rpc_handlers PRIVATE dinero_mempool)
foreach(_consumer
  dinerod
  nodecore_ffi
  swap_e2e_fixture_daemon
  test_block_assembler_smoke
  test_block_assembly_w1
  test_block_assembly_w1_integration
  test_block_relay_pool_owner
  test_block_template
  test_checkpoint_acceptance_daemon
  test_codebase_verification
  test_consensus_integration
  test_consensus_validation_tripwires
  test_covenant_system_lifecycle
  test_csn_proof_refresh
  test_ct_fee_policy
  test_ct_selection_policy
  test_daemon_service_release
  test_fee_bump_engine
  test_fee_bump_rpc
  test_fee_estimation_g4
  test_mempool_address_capture
  test_mempool_address_queries
  test_mempool_admission_owner
  test_mempool_block_owner
  test_mempool_callback_owner
  test_mempool_input_owners
  test_mempool_maintenance_owner
  test_mempool_output_coins
  test_mempool_package_bounds
  test_mempool_policy
  test_mempool_policy_owner
  test_mempool_prepared_refresh
  test_mempool_proof_staleness
  test_mempool_ranked_rpc
  test_mempool_raw_rpc
  test_mempool_refresh_owner
  test_mempool_relay_reader
  test_mempool_replacement_policy
  test_mempool_retained_load
  test_mempool_service_owner
  test_mempool_transaction_body
  test_mempool_typed_ingress
  test_mempool_typed_package
  test_mempool_typed_relay
  test_mining_chain_guard
  test_mining_comprehensive
  test_mining_job_policy
  test_mining_package_selection
  test_mining_pool_owner
  test_mining_selection_snapshot
  test_mining_smoke
  test_mining_template_result
  test_orchard_coupled_cycle_daemon
  test_orchard_wallet_cycle_daemon
  test_p2p_service_rate_limits
  test_pool_tip_consumers
  test_rbf_cpfp_detector
  test_rbf_policy_service
  test_relay_refresh_owner
  test_slow_reason_analyzer
  test_template_maturity
  test_tx_inclusion_analyzer
  test_tx_relay_g3
  test_utreexo_safety_gate
  test_validation_queue_handoff
  test_wallet_fee_bump_hooks
  test_wallet_historical_rpc
  test_wallet_input_coins
  test_wallet_mainnet_readiness
  test_wallet_sync_integration
)
  if(TARGET ${_consumer})
    target_link_libraries(${_consumer} PRIVATE dinero_mempool)
  endif()
endforeach()

# These fixtures now reach the real pool through dinero_core. Remove the
# obsolete provider so static-link order cannot silently select a no-op pool.
# The standalone wallet target and wallet-owned gate files are unchanged.
foreach(_consumer sep_wallet_fixture_compat test_block_relay_g2
    test_compact_block_binary test_headers_wire_v1)
  if(TARGET ${_consumer})
    get_target_property(_sources ${_consumer} SOURCES)
    list(FILTER _sources EXCLUDE REGEX "(^|/)tests/stubs/mempool_test_stubs\\.cpp$")
    set_property(TARGET ${_consumer} PROPERTY SOURCES "${_sources}")
  endif()
endforeach()
