#pragma once
/**
 * Phase 3 wave 1B (Path C) — Pedersen value-commitment generator.
 *
 * The shielded value commitment is `cv = rcv · G + v · V`
 * where:
 *   - G is the standard secp256k1 generator (used for blinding)
 *   - V is a DST-derived secp256k1 generator (used for value)
 *
 * Architectural note vs original memo: the supply-integrity memo
 * sketched two custom generators (V, R). Implementation collapsed to
 * one (V), with G as the blind generator. Reasons:
 *   1. Sapling itself uses Jubjub's standard generator for blinding +
 *      one custom generator for value; this matches Sapling's actual
 *      design rather than the more conservative original sketch.
 *   2. libsecp256k1-zkp's `secp256k1_pedersen_commit` API takes
 *      exactly this shape: `commit = blind·G + value·gen_h`.
 *   3. Security is unchanged. The required property is "V has no
 *      known discrete-log relation to the blinding generator." G has
 *      no known discrete log under any honestly-derived V, so the
 *      hiding+binding properties hold.
 *
 * V derivation (deterministic, nothing-up-my-sleeve):
 *   seed = SHA-256("DIN/v7/shielded/cv/V/v1")
 *   V    = secp256k1_generator_generate(ctx, seed)
 *
 * libsecp256k1-zkp's `secp256k1_generator_generate` is a try-and-
 * increment construction over the seed; it's deterministic and the
 * resulting point has no known discrete log under G. Test vectors
 * pin the exact bytes so any future implementation divergence
 * surfaces at constant-comparison time.
 */

#include "consensus/shielded/commitment_tree.h"

#include <string>

namespace dinero::consensus::shielded {

/// Domain separation tag for V. ANY change here is a chain split.
constexpr const char* kPedersenVDST = "DIN/v7/shielded/cv/V/v1";

/// V generator x-coordinate (32 bytes, big-endian). Lazy-derived from
/// `kPedersenVDST` on first call via libsecp256k1's
/// `secp256k1_generator_generate`. Idempotent.
const Hash& PedersenGeneratorV();

/// True once the V generator has been initialized to its canonical
/// derived bytes. Wave 1B returns true; this gate exists to catch
/// link-order / static-init bugs at runtime instead of corrupting
/// commitments.
bool PedersenGeneratorsReady();

/// Startup precondition. Returns true when the generators are available;
/// otherwise returns false and, when `error` is non-null, fills it with an
/// operator-facing explanation (and clears it on success).
///
/// Callers MUST refuse to start the node when this returns false. Shielded
/// consensus validation fails closed without the generators (see
/// ValidateShieldedBundle), so a node in this state does not merely lose
/// shielded functionality — it rejects every shielded bundle at or above the
/// input-binding activation height and forks itself off the network while
/// otherwise appearing healthy. Derivation runs under std::call_once, so the
/// failure is permanent for the process and cannot be retried; failing fast at
/// startup is the only loud outcome available.
bool CheckPedersenGeneratorsStartupPrecondition(std::string* error);

#ifdef DINERO_ENABLE_TEST_HOOKS
/// TEST-ONLY seam. Forces PedersenGeneratorsReady() to report failure so the
/// fail-closed consensus paths and the startup precondition can be exercised —
/// derivation is a std::call_once singleton over a compile-time DST, so the
/// failure cannot be produced legitimately.
///
/// Compiled ONLY when DINERO_ENABLE_TEST_HOOKS is defined, which is set on
/// individual test targets in tests/CMakeLists.txt and never on the production
/// libraries or the daemon.
void SetPedersenGeneratorsUnavailableForTest(bool unavailable);
#endif

}  // namespace dinero::consensus::shielded
