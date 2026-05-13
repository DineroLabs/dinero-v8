#pragma once
/**
 * GLOBAL VARIABLE BAN ENFORCEMENT
 *
 * This header uses preprocessor tricks to make it a compile-time error
 * to use banned global variables. Include this in common headers or
 * at the top of modules that should not access legacy globals.
 *
 * MECHANISM:
 * - Defines macros that replace g_* identifiers with compile errors
 * - Whitelist intentional globals (g_rpcRegistry, g_logger, g_secp)
 *
 * USAGE:
 * Add to your CMakeLists.txt:
 *   add_compile_definitions(DIN_BAN_G_PREFIX=1)
 *
 * Then include this header in files being refactored:
 *   #include "core/ban_globals.hpp"
 *
 * EXEMPTIONS:
 * If a specific file needs temporary access to globals during migration,
 * define DIN_ALLOW_LEGACY_GLOBALS before including:
 *   #define DIN_ALLOW_LEGACY_GLOBALS
 *   #include "core/ban_globals.hpp"
 */

#ifndef DIN_ALLOW_LEGACY_GLOBALS
#ifdef DIN_BAN_G_PREFIX

// Compiler-specific diagnostics
#if defined(__clang__)
  #pragma clang diagnostic push
  #pragma clang diagnostic warning "-Wdeprecated"
  _Pragma("message(\"Global 'g_' usage banned in this module - use DaemonContext services\")")
#elif defined(__GNUC__)
  #pragma GCC diagnostic push
  #pragma GCC diagnostic warning "-Wdeprecated"
  _Pragma("message \"Global 'g_' usage banned in this module - use DaemonContext services\"")
#elif defined(_MSC_VER)
  #pragma message("Global 'g_' usage banned in this module - use DaemonContext services")
#endif

// ════════════════════════════════════════════════════════════════
// BANNED GLOBALS (cause compile error if used)
// ════════════════════════════════════════════════════════════════

// These macros replace the identifier with an intentionally long,
// descriptive error message that will fail to compile.

#define g_mempool \
    DO_NOT_USE_g_mempool__Use_dinero_legacy_g_mempool_OR_inject_IMempoolService

#define g_blockchain \
    DO_NOT_USE_g_blockchain__Use_dinero_legacy_g_blockchain_OR_inject_IChainstateService

#define g_wallet_manager \
    DO_NOT_USE_g_wallet_manager__Use_dinero_legacy_g_wallet_manager_OR_inject_IWalletService

#define g_chain_db_direct \
    DO_NOT_USE_g_chain_db_direct__Use_dinero_legacy_g_chain_db_direct_OR_inject_ChainDB_ptr

#define g_utxo_set_direct \
    DO_NOT_USE_g_utxo_set_direct__Use_dinero_legacy_g_utxo_set_direct_OR_inject_UTXOIndex_ptr

#define g_peer_manager \
    DO_NOT_USE_g_peer_manager__Use_dinero_legacy_g_peer_manager_OR_inject_IP2PService

#define g_mining_coordinator \
    DO_NOT_USE_g_mining_coordinator__Use_dinero_legacy_g_mining_coordinator_OR_inject_IMiningService

#define g_data_dir \
    DO_NOT_USE_g_data_dir__Use_dinero_legacy_g_data_dir_OR_inject_IConfigService

#define g_p2p \
    DO_NOT_USE_g_p2p__Use_ctx_p2p_OR_inject_IP2PService

// ════════════════════════════════════════════════════════════════
// WHITELISTED GLOBALS (allowed - intentional design)
// ════════════════════════════════════════════════════════════════

// These are NOT banned because they are intentionally global:
// - g_rpcRegistry: Global RPC method registry (by design)
// - g_logger: Logging is acceptable as global
// - g_secp: Crypto library context (singleton by design)
// - g_daemon_start_time: Daemon uptime tracking
// - g_external_ip: Network configuration

// Note: No macros for these - they compile normally

#endif // DIN_BAN_G_PREFIX
#endif // !DIN_ALLOW_LEGACY_GLOBALS
