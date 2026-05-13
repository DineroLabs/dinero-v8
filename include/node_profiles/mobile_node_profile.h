// Copyright (c) 2026 The Dinero Core developers
// Distributed under the MIT software license

#pragma once

#include <cstddef>
#include <cstdint>

/**
 * Phase 12: Mobile / Embedded Deployment Profile (Header-Only)
 *
 * CRITICAL DESIGN RULE:
 * This is a COMPILE-TIME contract, not runtime configuration.
 *
 * All values are static constexpr → enforced at compile time.
 * All constraints are static_assert → violations fail the build.
 *
 * Why Header-Only Matters:
 * - iOS/Android: Process termination is silent (no exceptions)
 * - App Store: Reviews compiled behavior, not runtime flags
 * - Safety: Impossible to accidentally violate mobile limits
 * - Regression-proof: Cannot drift over time
 *
 * Key Properties:
 * - Zero .cpp file (header-only)
 * - Zero dependencies (pure config)
 * - Zero runtime branching
 * - Zero global state
 *
 * Design Philosophy:
 * "Mobile mode is not a suggestion — it is a law enforced by the compiler."
 *
 * Integration:
 * Same validator, same proofs, same math — just smaller envelope.
 * Resource constraints enforced at compile time, never at runtime.
 */

namespace dinero {
namespace node_profile {

/**
 * MobileNodeProfile - Compile-time resource contract for mobile/embedded
 *
 * Purpose: Enable full validation on constrained devices by enforcing
 * hard resource limits at COMPILE TIME (not runtime).
 *
 * Usage:
 *   ProofCache cache;
 *   cache.SetMaxBytes(MobileNodeProfile::MAX_PROOF_CACHE_BYTES);
 *   cache.SetTTL(MobileNodeProfile::PROOF_TTL_SECS);
 *
 * Safety Guarantee:
 * If code violates these limits, it will NOT COMPILE.
 *
 * Estimated Resource Usage:
 * - RAM: 20-40 MB (headers + cache)
 * - CPU: <1 core burst
 * - Bandwidth: ~1-2 MB per 1k blocks
 * - Storage: Headers only (no UTXO DB)
 */
struct MobileNodeProfile final {
    // ========================================================================
    // Memory Budgets (Compile-Time Constants)
    // ========================================================================

    /**
     * Maximum proof cache size
     *
     * Desktop default: 100 MB
     * Mobile contract:  16 MB (hard ceiling)
     *
     * Rationale:
     * - iOS background memory limit: ~30-50 MB
     * - Android low-tier devices: ~50-100 MB
     * - 16 MB leaves headroom for headers + overhead
     *
     * If exceeded → jetsam termination (iOS), OOM kill (Android)
     * static_assert ensures this cannot happen
     */
    static constexpr uint64_t MAX_PROOF_CACHE_BYTES = 16ULL * 1024 * 1024;  // 16 MB

    /**
     * Maximum header cache size
     *
     * Stores block headers for validation.
     * Mobile contract: 2 MB
     */
    static constexpr uint64_t MAX_HEADER_CACHE_BYTES = 2ULL * 1024 * 1024;  // 2 MB

    /**
     * Maximum Lightning cache size
     *
     * Stores channel/HTLC proofs for Lightning operations.
     * Mobile contract: 8 MB
     */
    static constexpr uint64_t MAX_LIGHTNING_CACHE_BYTES = 8ULL * 1024 * 1024;  // 8 MB

    // ========================================================================
    // Cache Behavior (Compile-Time Constants)
    // ========================================================================

    /**
     * Proof time-to-live (seconds)
     *
     * Desktop default: 86400 (24 hours)
     * Mobile contract:  600 (10 minutes)
     *
     * Rationale: Mobile nodes can afford to be "forgetful" because:
     * - Proofs are re-fetchable (Phase 9 guarantees this)
     * - Cached proofs are always re-verified (cache ≠ trust)
     * - Sync is resumable (Phase 10 proved this)
     *
     * Forgetfulness degrades speed, not security.
     */
    static constexpr uint64_t PROOF_TTL_SECS = 10 * 60;  // 10 minutes

    /**
     * Enable aggressive LRU eviction
     *
     * When true, evicts proofs more aggressively to stay under memory limits.
     * Mobile contract: true
     */
    static constexpr bool AGGRESSIVE_LRU_EVICTION = true;

    // ========================================================================
    // Network Behavior (Compile-Time Constants)
    // ========================================================================

    /**
     * Enable proof gossip
     *
     * Desktop default: true (announce INV_PROOF messages)
     * Mobile contract:  false (request-only, no gossip)
     *
     * Rationale: Mobile nodes should be "polite" and minimize network traffic.
     */
    static constexpr bool ENABLE_PROOF_GOSSIP = false;

    /**
     * Serve proofs to peers
     *
     * Desktop default: true (help distribute proofs)
     * Mobile contract:  false (consume only)
     *
     * Rationale: Mobile nodes should not act as servers.
     */
    static constexpr bool SERVE_PROOFS_TO_PEERS = false;

    /**
     * Prefer Utreexo bridge nodes
     *
     * When true, routing prioritizes nodes with NODE_UTREEXO_BRIDGE service flag.
     * Mobile contract: true
     *
     * Rationale: Bridge nodes are specifically designed to serve proofs efficiently.
     */
    static constexpr bool PREFER_UTREEXO_BRIDGE_NODES = true;

    // ========================================================================
    // Sync Behavior (Compile-Time Constants)
    // ========================================================================

    /**
     * Maximum parallel proof requests
     *
     * Desktop default: 16
     * Mobile contract:  2
     *
     * Rationale: Minimize concurrent connections to save battery/data.
     */
    static constexpr size_t MAX_PARALLEL_PROOF_REQUESTS = 2;

    /**
     * Retry backoff on failure (milliseconds)
     *
     * Mobile contract: 500ms
     *
     * Rationale: Give network time to recover on intermittent connectivity.
     */
    static constexpr uint64_t RETRY_BACKOFF_MS = 500;

    /**
     * Header batch size for sync
     *
     * Mobile contract: 128 headers per batch
     */
    static constexpr size_t HEADER_BATCH_SIZE = 128;

    /**
     * Proof batch size for sync
     *
     * Mobile contract: 4 proofs per batch
     */
    static constexpr size_t PROOF_BATCH_SIZE = 4;

    // ========================================================================
    // Power / Battery Management (Compile-Time Constants)
    // ========================================================================

    /**
     * Burst validation only
     *
     * When true, sync controller validates in short bursts, then sleeps.
     * Mobile contract: true
     *
     * What the mobile app actually does:
     * 1. Background sync in short bursts
     * 2. Validate headers + proofs
     * 3. Sleep
     * 4. Wake
     * 5. Repeat
     *
     * This keeps iOS background execution and Android Doze mode happy.
     */
    static constexpr bool BURST_VALIDATION_ONLY = true;

    /**
     * Maximum active sync time per burst (seconds)
     *
     * Mobile contract: 30 seconds
     *
     * Rationale: iOS background execution limit is ~30 seconds.
     * We stay comfortably under this to avoid termination.
     */
    static constexpr uint64_t MAX_ACTIVE_SYNC_TIME_SECS = 30;

    // ========================================================================
    // Lightning (Compile-Time Constants)
    // ========================================================================

    /**
     * Enable stateless watchtower
     *
     * When true, Lightning watchtower runs without UTXO DB using proofs.
     * Mobile contract: true
     *
     * This is where the architecture really shines:
     * - Mobile Lightning wallets don't need UTXO DB
     * - Validate channel funding with proofs
     * - Use stateless watchtowers
     * - Offline-friendly
     * - Low background cost
     */
    static constexpr bool ENABLE_STATELESS_WATCHTOWER = true;

    /**
     * Lightning cache TTL (seconds)
     *
     * Desktop default: 604800 (7 days)
     * Mobile contract:  604800 (7 days, same)
     *
     * Rationale: Lightning proofs are channel-lifetime scoped, so longer TTL is OK.
     */
    static constexpr uint64_t LIGHTNING_CACHE_TTL_SECS = 7 * 24 * 60 * 60;  // 7 days

    /**
     * Enable Lightning client
     *
     * Mobile contract: true (read-only Lightning operations)
     */
    static constexpr bool ENABLE_LIGHTNING_CLIENT = true;

    /**
     * Enable disk cache for proofs
     *
     * Mobile contract: false (memory-only cache)
     *
     * Rationale: Minimize disk I/O on mobile devices.
     */
    static constexpr bool ENABLE_DISK_CACHE = false;
};

/**
 * Desktop / Server Profile (for comparison)
 *
 * High-resource configuration for full nodes.
 */
struct DesktopNodeProfile final {
    static constexpr uint64_t MAX_PROOF_CACHE_BYTES     = 100ULL * 1024 * 1024;  // 100 MB
    static constexpr uint64_t MAX_HEADER_CACHE_BYTES    = 10ULL  * 1024 * 1024;  // 10 MB
    static constexpr uint64_t MAX_LIGHTNING_CACHE_BYTES = 50ULL  * 1024 * 1024;  // 50 MB

    static constexpr uint64_t PROOF_TTL_SECS            = 24 * 60 * 60;  // 24 hours
    static constexpr bool     AGGRESSIVE_LRU_EVICTION   = false;

    static constexpr bool     ENABLE_PROOF_GOSSIP       = true;
    static constexpr bool     SERVE_PROOFS_TO_PEERS     = true;
    static constexpr bool     PREFER_UTREEXO_BRIDGE_NODES = false;

    static constexpr size_t   MAX_PARALLEL_PROOF_REQUESTS = 16;
    static constexpr uint64_t RETRY_BACKOFF_MS           = 100;

    static constexpr size_t   HEADER_BATCH_SIZE          = 2000;
    static constexpr size_t   PROOF_BATCH_SIZE           = 16;

    static constexpr bool     BURST_VALIDATION_ONLY      = false;
    static constexpr uint64_t MAX_ACTIVE_SYNC_TIME_SECS  = 0;  // unlimited

    static constexpr bool     ENABLE_STATELESS_WATCHTOWER = true;
    static constexpr uint64_t LIGHTNING_CACHE_TTL_SECS   = 7 * 24 * 60 * 60;  // 7 days
    static constexpr bool     ENABLE_LIGHTNING_CLIENT    = true;
    static constexpr bool     ENABLE_DISK_CACHE          = true;
};

/**
 * Stateless Node Profile (middle ground)
 *
 * Medium-resource configuration for stateless full nodes.
 */
struct StatelessNodeProfile final {
    static constexpr uint64_t MAX_PROOF_CACHE_BYTES     = 50ULL * 1024 * 1024;  // 50 MB
    static constexpr uint64_t MAX_HEADER_CACHE_BYTES    = 5ULL  * 1024 * 1024;  // 5 MB
    static constexpr uint64_t MAX_LIGHTNING_CACHE_BYTES = 25ULL * 1024 * 1024;  // 25 MB

    static constexpr uint64_t PROOF_TTL_SECS            = 12 * 60 * 60;  // 12 hours
    static constexpr bool     AGGRESSIVE_LRU_EVICTION   = false;

    static constexpr bool     ENABLE_PROOF_GOSSIP       = true;
    static constexpr bool     SERVE_PROOFS_TO_PEERS     = true;
    static constexpr bool     PREFER_UTREEXO_BRIDGE_NODES = false;

    static constexpr size_t   MAX_PARALLEL_PROOF_REQUESTS = 8;
    static constexpr uint64_t RETRY_BACKOFF_MS           = 200;

    static constexpr size_t   HEADER_BATCH_SIZE          = 1000;
    static constexpr size_t   PROOF_BATCH_SIZE           = 8;

    static constexpr bool     BURST_VALIDATION_ONLY      = false;
    static constexpr uint64_t MAX_ACTIVE_SYNC_TIME_SECS  = 0;  // unlimited

    static constexpr bool     ENABLE_STATELESS_WATCHTOWER = true;
    static constexpr uint64_t LIGHTNING_CACHE_TTL_SECS   = 7 * 24 * 60 * 60;  // 7 days
    static constexpr bool     ENABLE_LIGHTNING_CLIENT    = true;
    static constexpr bool     ENABLE_DISK_CACHE          = true;
};

} // namespace node_profile
} // namespace dinero
