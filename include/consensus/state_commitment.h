#pragma once
/**
 * state_commitment_v1 — coinbase commitment to post-block shielded state.
 *
 * Placement decision (frozen): the commitment lives in a coinbase OP_RETURN
 * output, not in the block header.
 *
 *   - It is still cryptographically bound to the header, through the
 *     transaction merkle root: commitment <- coinbase <- merkle_root <-
 *     PoW-validated header. Same trust chain the DNRF filter commitment uses.
 *   - The 128-byte header stays frozen, so every CPU/GPU/SV2 mining
 *     implementation, template, and wire assumption is untouched. This remains
 *     a consensus fork; it is not a mining-format fork.
 *   - Rejected: the 12 reserved header bytes. 96 bits cannot hold a 256-bit
 *     commitment without truncation, and a mining attacker who grinds
 *     transactions faces only a 2^48 birthday bound — too thin for a value
 *     that protects money.
 *   - Avoided: growing the header. It would change PoW serialization, miners,
 *     GPU kernels, SV2 templates, and hardware assumptions without adding
 *     cryptographic strength over a properly proven coinbase commitment.
 *
 * Verification cost for a snapshot-bootstrapping node (headers, no blocks) is
 * one block: the snapshot base. That requires the snapshot to carry the base
 * block's coinbase transaction and its merkle branch, verified against the
 * PoW-authenticated header without fetching the block.
 *
 * FORMAT REALITY, per snapshot version — do not read the paragraph above as a
 * property v4 has:
 *   - v4 carries NO coinbase and NO merkle branch. Before the genesis->base
 *     replay completes, a v4 snapshot's shielded section is authenticated only
 *     by the snapshot's own checksum (and, for shipped snapshots, the registry
 *     file hash — which protects only snapshots the project shipped, not the
 *     any-node-can-publish goal).
 *   - v5 carries the coinbase + merkle branch and is verified at LOAD time:
 *     DNRS commitment -> proven coinbase -> tx merkle root -> selected
 *     best-work header. Mandatory for any base at or above the
 *     state-commitment activation height.
 *
 * No circularity: the committed value is post-block shielded state, and a
 * coinbase transaction cannot itself alter shielded state.
 *
 * Script format (39 bytes total):
 *   [0]     = 0x6a  OP_RETURN
 *   [1]     = 0x25  push 37 bytes
 *   [2..5]  = 0x444E5253  "DNRS" magic
 *   [6]     = 0x01  script encoding version
 *   [7..38] = 32-byte state_commitment_v1 root, big-endian as produced by
 *             ComputeShieldedRoot (SHR1 preimage, its own version byte)
 *
 * The DNRS magic is deliberately distinct from the SHR1 preimage tag: one
 * domain-separates the coinbase encoding, the other the digest preimage. A
 * value from one must never parse as the other.
 *
 * ACTIVATION. IsStateCommitmentActive() below is the single authority for
 * whether the commitment is enforced at a height; the per-network activation
 * height lives in chainparams: mainnet 111000, testnet dormant (UINT32_MAX),
 * regtest 1. See docs/activation/dnrs-111000.md for the operator-selected
 * cutover and deployment requirements. This gate is independent of Auth.
 */

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "primitives/transaction.h"
#include "primitives/uint256.h"

namespace dinero::consensus {

struct StateCommitment {
    /// "DNRS" — Dinero Shielded. Named after the DNRF filter-commitment
    /// precedent so the two read as siblings.
    static constexpr uint32_t MAGIC = 0x444E5253;
    static constexpr uint8_t MAGIC_BYTES[4] = {0x44, 0x4E, 0x52, 0x53};

    /// Versions the SCRIPT ENCODING. Distinct from SHIELDED_ROOT_VERSION,
    /// which versions the digest preimage; the two can move independently.
    static constexpr uint8_t VERSION = 0x01;

    /// 4 magic + 1 version + 32 root.
    static constexpr size_t PAYLOAD_SIZE = 37;

    /// OP_RETURN + push byte + payload.
    static constexpr size_t SCRIPT_SIZE = 39;

    static constexpr size_t OFFSET_OPRETURN = 0;
    static constexpr size_t OFFSET_PUSHLEN  = 1;
    static constexpr size_t OFFSET_MAGIC    = 2;
    static constexpr size_t OFFSET_VERSION  = 6;
    static constexpr size_t OFFSET_ROOT     = 7;

    /// Dormancy sentinel for state_commitment_activation_height. uint32_t on
    /// purpose: it must be the same type as the chainparams field and the
    /// snapshot header's block_height, because a silently-widened UINT32_MAX
    /// comparison behaves differently in two places — the exact
    /// one-sentinel-two-behaviors bug the single activation predicate below
    /// exists to rule out.
    static constexpr uint32_t kActivationHeightUnset = UINT32_MAX;
};

/// THE single authority for "is the state commitment enforced at this height".
/// Every layer — snapshot-format selection, the v5 proof requirement,
/// load-time fail-closed checks, replay mismatch enforcement, coinbase
/// commitment validation and mining, tests, and RPC status reporting — must
/// call this rather than hand-writing the comparison: two sites computing
/// dormancy differently disagree at exactly one height, and an enforcement
/// site that wrongly believes it is dormant fails OPEN.
///
/// Heights compared here can be ATTACKER-CONTROLLED (a snapshot file's claimed
/// base height). The sentinel guard means a claimed height of UINT32_MAX with
/// activation unset stays dormant-and-rejected-elsewhere rather than
/// spuriously active; with activation set, `>=` errs toward active, i.e. the
/// failure direction is closed, not open.
///
/// Both heights are parameters (not read from Params()) so tests can sweep
/// them independently — including the mixed quadrants where a coupling bug
/// between this activation and any other would hide.
constexpr bool IsStateCommitmentActive(uint32_t height,
                                       uint32_t activation_height)
{
    return activation_height != UINT32_MAX &&
           height >= activation_height;
}

/// Why a coinbase lookup did not yield exactly one usable commitment.
/// Distinguished so callers and tests can assert the precise failure rather
/// than collapsing every case to "absent".
enum class StateCommitmentStatus {
    Ok,         ///< exactly one well-formed commitment
    Missing,    ///< no output carries the DNRS tag
    Duplicate,  ///< more than one output carries it — never acceptable
    Malformed,  ///< tagged but truncated, wrong push length, or wrong version
};

struct StateCommitmentLookup {
    StateCommitmentStatus status{StateCommitmentStatus::Missing};
    size_t index{0};     ///< meaningful only when status == Ok
    uint256 root;        ///< meaningful only when status == Ok
};

/// Canonical encoder. The single place the byte layout is written.
std::vector<uint8_t> BuildStateCommitmentScript(const uint256& root);

/// Parse one scriptPubKey. Returns nullopt unless it is exactly the canonical
/// 39-byte form with the right magic and version.
std::optional<uint256> ParseStateCommitmentScript(const std::vector<uint8_t>& script);

/// Every output index whose script carries the DNRS tag, well-formed or not.
/// Used to detect duplicates, which a "last one wins" scan would hide.
std::vector<size_t> FindStateCommitmentCandidates(const Transaction& coinbase);

/// Exactly-one lookup. Duplicates are an error, not a preference for the last.
StateCommitmentLookup FindStateCommitment(const Transaction& coinbase);

}  // namespace dinero::consensus
