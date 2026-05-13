# ═══════════════════════════════════════════════════════════════════════════
# Architecture Enforcement: Lightning L2 ↛ L1 Separation
# ═══════════════════════════════════════════════════════════════════════════
# POLICY: Lightning L2 code (ChannelManagerCore, state machine) MUST NOT link
#         against L1 infrastructure (chainstate, wallet, daemon, mempool).
#
# WHY: Lightning is a pure state machine that queries blockchain state through
#      oracle interfaces. Direct L1 linkage would:
#      - Break compile-time separation
#      - Make L2 untestable without full node
#      - Create circular dependencies
#      - Violate architectural layering
#
# ENFORCEMENT: This file is included by CMakeLists.txt and will FAIL the build
#              if Lightning targets attempt to link L1 libraries.
# ═══════════════════════════════════════════════════════════════════════════

# Define all L1 (Layer 1) targets that Lightning MUST NOT link against
set(DINERO_L1_TARGETS
    dinero_core           # Consensus, blockchain validation
    dinero_chainstate     # Block index, UTXO set
    dinero_wallet         # Wallet manager, key derivation
    dinero_daemon         # Daemon context, RPC handlers
    dinero_p2p            # Peer-to-peer networking
    dinero_mempool        # Transaction pool
    dinero_mining         # Block assembly, PoW
    dinero_storage        # RocksDB direct access
)

# Helper function: Assert a target does not link against L1
#
# Usage:
#   assert_no_l1_linkage(test_channel_manager_state)
#
# Failure: CMake configure fails with clear error message
function(assert_no_l1_linkage target)
    # Skip if target doesn't exist (e.g., optional builds)
    if(NOT TARGET ${target})
        return()
    endif()

    # Get all libraries this target links against
    get_target_property(LINK_LIBS ${target} LINK_LIBRARIES)

    if(LINK_LIBS)
        foreach(lib ${LINK_LIBS})
            # Check if linked library is in forbidden L1 list
            if(lib IN_LIST DINERO_L1_TARGETS)
                message(FATAL_ERROR
                    "\n"
                    "═══════════════════════════════════════════════════════════════\n"
                    "  ❌ ARCHITECTURE VIOLATION DETECTED\n"
                    "═══════════════════════════════════════════════════════════════\n"
                    "\n"
                    "Target: ${target}\n"
                    "Forbidden linkage: ${lib} (L1 infrastructure)\n"
                    "\n"
                    "Lightning L2 targets MUST NOT link against L1 libraries.\n"
                    "\n"
                    "Allowed dependencies for Lightning L2:\n"
                    "  ✓ STL (standard library)\n"
                    "  ✓ Crypto libraries (secp256k1, OpenSSL)\n"
                    "  ✓ Serialization (msgpack, protobuf)\n"
                    "  ✓ Database interfaces (ILightningDB)\n"
                    "  ✓ Oracle interfaces (IChainOracle, IWalletOracle)\n"
                    "\n"
                    "Forbidden L1 targets:\n"
                    "  ✗ dinero_core, dinero_chainstate, dinero_wallet\n"
                    "  ✗ dinero_daemon, dinero_mempool, dinero_p2p\n"
                    "\n"
                    "Fix: Use oracle interfaces instead of direct L1 access.\n"
                    "See: docs/architecture/lightning_l2_separation.md\n"
                    "\n"
                    "═══════════════════════════════════════════════════════════════\n"
                )
            endif()
        endforeach()
    endif()

    # Success: No L1 linkage detected
    message(STATUS "✓ Architecture guard passed: ${target} (L2 pure)")
endfunction()

# Helper function: Enforce L2 purity on all Lightning targets
#
# Call this after defining Lightning library and test targets
function(enforce_lightning_l2_purity)
    if(TARGET dinero_lightning)
        assert_no_l1_linkage(dinero_lightning)
    endif()

    if(TARGET test_channel_manager_state)
        assert_no_l1_linkage(test_channel_manager_state)
    endif()

    # Add more Lightning test targets here as they're created
    if(TARGET test_channel_lifecycle)
        assert_no_l1_linkage(test_channel_lifecycle)
    endif()

    if(TARGET test_lightning_state_machine)
        assert_no_l1_linkage(test_lightning_state_machine)
    endif()
endfunction()
