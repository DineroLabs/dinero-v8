// Copyright (c) 2026 The Dinero Core developers
// Distributed under the MIT software license

#pragma once

#include "node_profiles/mobile_node_profile.h"

/**
 * Phase 12: Compile-Time Safety Guards for Mobile Profile
 *
 * CRITICAL PURPOSE:
 * Enforce mobile resource constraints at COMPILE TIME.
 *
 * If someone later:
 * - Bumps cache size to 64 MB
 * - Increases parallelism to 8
 * - Extends TTL to hours
 * - Enables proof serving
 *
 * → The build FAILS, not the phone.
 *
 * Why This Matters:
 * - iOS jetsam kills silently (no exceptions, no stack trace)
 * - App Store reviews compiled behavior (not runtime flags)
 * - Regression prevention (cannot accidentally violate limits)
 * - CI becomes enforcement mechanism
 *
 * Design Philosophy:
 * "Mobile safety is not optional — it is enforced by the compiler."
 *
 * These guards make Phase 12 NOT an experiment.
 */

namespace dinero {
namespace node_profile {

// ============================================================================
// Memory Safety Guards
// ============================================================================

/**
 * Proof cache must not exceed mobile memory budget
 *
 * iOS background limit: ~30-50 MB total
 * Android low-tier: ~50-100 MB total
 * Mobile contract: 16 MB for proofs (leaves headroom)
 *
 * If exceeded → jetsam termination (iOS), OOM kill (Android)
 */
static_assert(
    MobileNodeProfile::MAX_PROOF_CACHE_BYTES <= 16ULL * 1024 * 1024,
    "MobileNodeProfile: proof cache exceeds mobile memory budget (16 MB max)"
);

/**
 * Header cache must fit within mobile limits
 */
static_assert(
    MobileNodeProfile::MAX_HEADER_CACHE_BYTES <= 2ULL * 1024 * 1024,
    "MobileNodeProfile: header cache exceeds mobile memory budget (2 MB max)"
);

/**
 * Lightning cache must fit within mobile limits
 */
static_assert(
    MobileNodeProfile::MAX_LIGHTNING_CACHE_BYTES <= 8ULL * 1024 * 1024,
    "MobileNodeProfile: Lightning cache exceeds mobile memory budget (8 MB max)"
);

/**
 * Total memory budget sanity check
 *
 * Proof cache (16 MB) + Headers (2 MB) + Lightning (8 MB) = 26 MB
 * Plus overhead (~4-8 MB) = ~30-34 MB total
 *
 * This must stay under iOS background limit (~30-50 MB)
 */
static_assert(
    MobileNodeProfile::MAX_PROOF_CACHE_BYTES +
    MobileNodeProfile::MAX_HEADER_CACHE_BYTES +
    MobileNodeProfile::MAX_LIGHTNING_CACHE_BYTES <= 30ULL * 1024 * 1024,
    "MobileNodeProfile: total cache size exceeds mobile memory budget (30 MB max)"
);

// ============================================================================
// Cache Behavior Guards
// ============================================================================

/**
 * TTL must be short enough for mobile background execution
 *
 * Mobile nodes can afford to be "forgetful" because:
 * - Proofs are re-fetchable (Phase 9)
 * - Cache ≠ trust (always re-verified)
 * - Sync is resumable (Phase 10)
 *
 * 15 minutes max (conservative upper bound)
 */
static_assert(
    MobileNodeProfile::PROOF_TTL_SECS <= 15 * 60,
    "MobileNodeProfile: proof cache TTL too large for mobile (15 minutes max)"
);

/**
 * Lightning cache TTL can be longer (channel-lifetime scoped)
 *
 * 14 days max (conservative upper bound for long-lived channels)
 */
static_assert(
    MobileNodeProfile::LIGHTNING_CACHE_TTL_SECS <= 14 * 24 * 60 * 60,
    "MobileNodeProfile: Lightning cache TTL exceeds reasonable channel lifetime (14 days max)"
);

/**
 * Aggressive LRU eviction must be enabled
 *
 * Mobile nodes need aggressive memory management to stay under limits.
 */
static_assert(
    MobileNodeProfile::AGGRESSIVE_LRU_EVICTION == true,
    "MobileNodeProfile: aggressive LRU eviction must be enabled"
);

// ============================================================================
// Network Behavior Guards
// ============================================================================

/**
 * Proof gossip must be disabled
 *
 * Mobile nodes should be "polite" - request-only, no unsolicited gossip.
 */
static_assert(
    MobileNodeProfile::ENABLE_PROOF_GOSSIP == false,
    "MobileNodeProfile: proof gossip must be disabled (request-only mode)"
);

/**
 * Serving proofs must be disabled
 *
 * Mobile nodes should not act as servers (battery, data, stability).
 */
static_assert(
    MobileNodeProfile::SERVE_PROOFS_TO_PEERS == false,
    "MobileNodeProfile: serving proofs must be disabled (consume-only mode)"
);

/**
 * Bridge node preference must be enabled
 *
 * Bridge nodes are optimized for serving proofs efficiently.
 */
static_assert(
    MobileNodeProfile::PREFER_UTREEXO_BRIDGE_NODES == true,
    "MobileNodeProfile: bridge node preference should be enabled"
);

/**
 * Disk cache must be disabled
 *
 * Mobile nodes use memory-only cache to minimize disk I/O.
 */
static_assert(
    MobileNodeProfile::ENABLE_DISK_CACHE == false,
    "MobileNodeProfile: disk cache must be disabled (memory-only)"
);

// ============================================================================
// CPU / Concurrency Guards
// ============================================================================

/**
 * Parallelism must be limited for mobile CPUs
 *
 * Desktop: 16 parallel requests
 * Mobile: 2 parallel requests (battery, thermal, simplicity)
 */
static_assert(
    MobileNodeProfile::MAX_PARALLEL_PROOF_REQUESTS <= 2,
    "MobileNodeProfile: parallelism too high for mobile CPUs (2 max)"
);

/**
 * Retry backoff must allow time for intermittent connectivity
 *
 * Mobile networks are flaky - give them time to recover.
 */
static_assert(
    MobileNodeProfile::RETRY_BACKOFF_MS >= 500,
    "MobileNodeProfile: retry backoff too aggressive for mobile networks (500ms min)"
);

// ============================================================================
// Power / Battery Guards
// ============================================================================

/**
 * Burst validation must be enabled
 *
 * iOS background execution limit: ~30 seconds
 * Android Doze mode: periodic wake windows
 *
 * Burst mode is mandatory for mobile OS compliance.
 */
static_assert(
    MobileNodeProfile::BURST_VALIDATION_ONLY == true,
    "MobileNodeProfile: burst validation must be enabled (mobile OS requirement)"
);

/**
 * Burst time must fit within iOS background execution window
 *
 * iOS limit: ~30 seconds before termination
 * Mobile contract: 30 seconds max (stay at limit)
 */
static_assert(
    MobileNodeProfile::MAX_ACTIVE_SYNC_TIME_SECS <= 30,
    "MobileNodeProfile: burst time exceeds iOS background limit (30 seconds max)"
);

/**
 * Burst time must be non-zero (sanity check)
 */
static_assert(
    MobileNodeProfile::MAX_ACTIVE_SYNC_TIME_SECS > 0,
    "MobileNodeProfile: burst time must be non-zero"
);

// ============================================================================
// Lightning Guards
// ============================================================================

/**
 * Stateless watchtower must be enabled
 *
 * This is the killer feature for mobile Lightning:
 * - No UTXO DB required
 * - Validate with proofs only
 * - Offline-friendly
 * - Low background cost
 */
static_assert(
    MobileNodeProfile::ENABLE_STATELESS_WATCHTOWER == true,
    "MobileNodeProfile: stateless watchtower should be enabled (key mobile feature)"
);

/**
 * Lightning client must be enabled
 *
 * Mobile nodes should support read-only Lightning operations.
 */
static_assert(
    MobileNodeProfile::ENABLE_LIGHTNING_CLIENT == true,
    "MobileNodeProfile: Lightning client should be enabled"
);

// ============================================================================
// Batch Size Guards
// ============================================================================

/**
 * Header batch size must be reasonable for mobile
 *
 * Too small: Too many network round-trips
 * Too large: Memory pressure
 *
 * Mobile contract: 128 headers per batch (~10 KB)
 */
static_assert(
    MobileNodeProfile::HEADER_BATCH_SIZE >= 64 &&
    MobileNodeProfile::HEADER_BATCH_SIZE <= 256,
    "MobileNodeProfile: header batch size out of mobile-optimal range (64-256)"
);

/**
 * Proof batch size must be conservative for mobile
 *
 * Proofs are large (can be 1-10 KB each).
 * Mobile contract: 4 proofs per batch
 */
static_assert(
    MobileNodeProfile::PROOF_BATCH_SIZE >= 2 &&
    MobileNodeProfile::PROOF_BATCH_SIZE <= 8,
    "MobileNodeProfile: proof batch size out of mobile-optimal range (2-8)"
);

// ============================================================================
// Hierarchy Guards (Profile Comparison)
// ============================================================================

/**
 * Mobile profile must be strictly more constrained than desktop
 */
static_assert(
    MobileNodeProfile::MAX_PROOF_CACHE_BYTES < DesktopNodeProfile::MAX_PROOF_CACHE_BYTES,
    "MobileNodeProfile: cache must be smaller than desktop"
);

static_assert(
    MobileNodeProfile::MAX_PARALLEL_PROOF_REQUESTS < DesktopNodeProfile::MAX_PARALLEL_PROOF_REQUESTS,
    "MobileNodeProfile: parallelism must be lower than desktop"
);

static_assert(
    MobileNodeProfile::PROOF_TTL_SECS < DesktopNodeProfile::PROOF_TTL_SECS,
    "MobileNodeProfile: TTL must be shorter than desktop"
);

/**
 * Desktop profile must allow serving and gossip
 */
static_assert(
    DesktopNodeProfile::ENABLE_PROOF_GOSSIP == true,
    "DesktopNodeProfile: proof gossip should be enabled (help network)"
);

static_assert(
    DesktopNodeProfile::SERVE_PROOFS_TO_PEERS == true,
    "DesktopNodeProfile: serving proofs should be enabled (help network)"
);

// ============================================================================
// Final Sanity Check
// ============================================================================

/**
 * All profiles must use same validation
 *
 * This is enforced by architecture (no profile-specific validators).
 * This static_assert just documents the invariant.
 */
static_assert(
    MobileNodeProfile::ENABLE_LIGHTNING_CLIENT == DesktopNodeProfile::ENABLE_LIGHTNING_CLIENT,
    "All profiles must support Lightning (Phase 11 requirement)"
);

} // namespace node_profile
} // namespace dinero

/**
 * Usage in Code:
 *
 * Simply include this header in any mobile-targeted compilation:
 *
 * #include "node_profiles/mobile_node_guards.h"
 *
 * If any guard fails → build breaks → violation cannot ship.
 *
 * Example Error Message:
 *   error: static_assert failed: "MobileNodeProfile: proof cache exceeds
 *          mobile memory budget (16 MB max)"
 *
 * This makes Phase 12 regression-proof.
 */
