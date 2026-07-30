#pragma once

#include <string>
#include <cstdint>
#include <vector>
#include <map>

namespace dinero {

// Network type enumeration
enum class Chain : uint8_t {
    MAINNET,
    TESTNET,
    REGTEST
};

// Genesis block parameters (exact immutable values from genesis mining)
struct GenesisParams {
    uint32_t nVersion;
    uint32_t nTime;
    uint32_t nBits;
    uint32_t nNonce;
    std::string genesisHashHex;
    std::string merkleRootHex;
    std::string genesisCoinbaseHex;  // EXACT raw tx bytes as hex (no whitespace)
};

// Chain consensus and network parameters
struct ChainParams {
    std::string name;          // "mainnet" | "testnet" | "regtest"
    std::string hrp;           // "din" | "tdin" | "rdin"
    uint32_t magic;            // P2P magic bytes

    // Default port configuration
    uint16_t rpc_port;         // JSON-RPC port
    uint16_t http_port;        // HTTP API port
    uint16_t ws_port;          // WebSocket port
    uint16_t p2p_port;         // P2P network port

    // Network identification
    std::string genesis_hash;  // Genesis block hash
    std::string network_id;    // Network identifier for logging

    // Mining and consensus parameters
    uint32_t pow_limit_bits;   // Proof of work difficulty limit
    uint32_t target_spacing;   // Block time target in seconds
    uint32_t retarget_interval; // Difficulty adjustment interval

    // Policy parameters
    uint64_t dust_threshold;   // Minimum output value (una)
    uint64_t min_relay_fee;    // Minimum relay fee rate (sat/kB)
    uint32_t max_block_size;   // Maximum block size in bytes
    uint32_t coinbase_maturity; // Number of confirmations before coinbase can be spent

    // Address prefixes
    uint8_t pubkey_address_prefix;    // P2PKH address version
    uint8_t script_address_prefix;    // P2SH address version

    // Development and testing flags
    bool allow_min_difficulty;       // Allow minimum difficulty blocks
    bool require_standard_txs;       // Require standard transactions only
    bool mine_blocks_on_demand;      // Mine blocks on demand (regtest)

    // ═══════════════════════════════════════════════════════════════════════════
    // Witness commitment enforcement
    // ═══════════════════════════════════════════════════════════════════════════
    // What this controls:
    //   - If enforce_witness_commitment = true AND height >= enforcement_height:
    //     * Blocks with a serialized witness marker MUST include valid DINW
    //     * Blocks without a witness marker are not required to include DINW
    //     * Invalid/missing commitment → block rejected
    //   - A recognized DINW v1 commitment is validated at every full-rules
    //     height, including before mandatory enforcement.
    //
    // What this does NOT do:
    //   - ❌ NOT segwit activation
    //   - ❌ NOT a header change
    //   - ❌ NOT mandatory for all blocks
    //
    // Deployed v8 behavior made DINW mandatory for witness-bearing blocks at
    // height 10,670 on every chain. These fields are the sole activation source;
    // changing them changes consensus and requires historical compatibility
    // analysis plus real BlockValidator boundary tests.
    // ═══════════════════════════════════════════════════════════════════════════
    bool enforce_witness_commitment;           // Enforce witness commitment requirement
    uint32_t witness_commitment_enforcement_height;  // Height to start enforcement

    // ═══════════════════════════════════════════════════════════════════════════
    // Phase 11e: Bitcoin Magic Translation (OFF by default, NEVER activate casually)
    // ═══════════════════════════════════════════════════════════════════════════
    // ⚠️  CRITICAL: This is NOT Bitcoin compatibility activation!
    //     This is interpretation plumbing only - the switch is OFF by default.
    //
    // What this controls:
    //   - If translation enabled AND height >= translation_height:
    //     * DINW commitments are interpreted as Bitcoin-style (0xaa21a9ed)
    //     * NO block mutation, NO history rewrite, NO header changes
    //     * Pure validation-time interpretation only
    //
    // What this does NOT do:
    //   - ❌ NOT Bitcoin SegWit activation
    //   - ❌ NOT changing block headers or txids
    //   - ❌ NOT affecting mining (still creates DINW)
    //   - ❌ NOT rewriting historical blocks
    //   - ❌ NOT changing consensus rules retroactively
    //
    // Why this exists:
    //   - Future Bitcoin tool compatibility (wallet, explorers)
    //   - Zero-cost translation (interpretation, not migration)
    //   - Preserves DINW as truth, Bitcoin magic as lens
    //
    // When to activate (strict requirements):
    //   - Dinero wants Bitcoin wallet/tool compatibility AND
    //   - SegWit semantics are fully enforced AND
    //   - Taproot rules are active AND
    //   - Mempool policy matches Bitcoin AND
    //   - Ecosystem explicitly requests compatibility
    //   - If ANY requirement unmet → keep switch OFF
    //
    // Network defaults (SAFE - no translation):
    //   - mainnet:  translate=false, height=UINT32_MAX (never)
    //   - testnet:  translate=false, height=UINT32_MAX (never)
    //   - regtest:  translate=false, height=UINT32_MAX (configurable)
    //
    // Status: Phase 11e.1 (translation plumbing only, NOT activated)
    // Locked by: tests/consensus/test_witness_magic_translation.cpp
    // ═══════════════════════════════════════════════════════════════════════════
    bool enable_witness_magic_translation;           // Translate DINW → Bitcoin magic
    uint32_t witness_magic_translation_height;       // Height to start translation

    // ═══════════════════════════════════════════════════════════════════════════
    // Dragon Kill-Switch: Confidential Transaction Emergency Disable
    // ═══════════════════════════════════════════════════════════════════════════
    // ⚠️  CRITICAL: Emergency safety mechanism for confidential transactions
    //
    // What this controls:
    //   - If disable_confidential_transactions = true:
    //     * All confidential transactions are REJECTED (mempool + blocks)
    //     * Existing confidential UTXOs remain spendable (transparent outputs only)
    //     * No range proof verification attempted
    //     * Logs "ZK kill-switch engaged" on activation
    //
    // When to use:
    //   - Critical bug discovered in Bulletproofs implementation
    //   - Cryptographic vulnerability in range proofs
    //   - Consensus emergency requiring immediate ZK disable
    //   - Testing/debugging confidential transaction issues
    //
    // Network defaults (ZK ENABLED - normal operation):
    //   - mainnet:  disable=false (confidential txs allowed)
    //   - testnet:  disable=false (confidential txs allowed)
    //   - regtest:  disable=false (configurable via RPC/config)
    //
    // Runtime modification:
    //   - Can be toggled via RPC: setconfidentialenable <true|false>
    //   - Can be set via config: disableconfidential=1
    //   - Change takes effect immediately (no restart required)
    //
    // Status: Dragon #4 Safety Mechanism
    // Locked by: tests/reorg/test_confidential_reorg.cpp (ZKKillSwitch test)
    // ═══════════════════════════════════════════════════════════════════════════
    bool disable_confidential_transactions = false;  // Kill-switch: disable all CT

    // ═══════════════════════════════════════════════════════════════════════════
    // Confidential Transaction Activation Height
    // ═══════════════════════════════════════════════════════════════════════════
    // Height at which confidential transactions become valid on this network.
    // Before this height, all confidential transactions are rejected.
    //
    // Network defaults:
    //   - mainnet:  height 2 (after genesis)
    //   - testnet:  height 0 (immediately)
    //   - regtest:  height 0 (immediately)
    // ═══════════════════════════════════════════════════════════════════════════
    uint32_t confidential_activation_height = 0;

    // ===========================================================================
    // Shielded Pool Activation Height (Phase 1 — disabled on mainnet/testnet)
    // ===========================================================================
    // Block height at which shielded transactions (v5) become consensus-valid
    // on this network. Below this height, every non-empty shielded bundle is
    // rejected by ValidateShieldedBundle with NotActive — even though the v5
    // tx version, p2p serialization, and RPC handlers compile.
    //
    // Default UINT32_MAX = effectively never. Mainnet and testnet keep this
    // as the default until the spec/code drift is resolved (anchor depth,
    // value range proof, address-binding tag in commitment formula). Regtest
    // sets 0 so unit tests continue to exercise the full pipeline.
    //
    // This is NOT a soft fork: the v5 acceptance is gated at consensus, so
    // changing this height changes block validity. Operators MUST coordinate
    // a release before lowering this on a live network.
    // ===========================================================================
    uint32_t shielded_activation_height = UINT32_MAX;

    // ===========================================================================
    // CONSENSUS: shielded public-input-binding activation height (CONFIRMED-CRIT-05).
    // Blocks at or above this height verify shielded spend/output proofs with the
    // public-input-bound Spartan rule (z=(1,io,W) split). Blocks below it use the
    // pre-fix unbound rule, retained only to validate pre-activation history.
    // Changing this changes block validity — operators MUST coordinate a release.
    // ===========================================================================
    uint32_t shielded_input_binding_activation_height = UINT32_MAX;

    // ===========================================================================
    // CONSENSUS: shielded cv-binding activation height (audit Critical #1).
    // Blocks at or above this height require shielded spend/output proofs whose
    // circuit binds the Pedersen value commitment cv to the in-circuit note
    // value (cv == val·V + rcv·G), closing the mint-from-nothing inflation hole.
    // Such proofs carry distinct version bytes (0x03 spend / 0x04 output);
    // blocks below it keep verifying legacy proofs (0x01/0x02) under the old
    // verifying key, so the ~10 pre-activation notes remain spendable.
    //
    // The struct default is UINT32_MAX (never activate); each chain opts in from
    // chainparams_impl.cpp. MAINNET activated at 61000 (paired with the epoch
    // reset — see below); testnet/regtest leave it dormant. Any new activation
    // height MUST be chosen by a human and coordinated across the fleet before it
    // ships — a wrong boundary splits the chain.
    // ===========================================================================
    uint32_t shielded_cv_binding_activation_height = UINT32_MAX;

    // Shielded epoch reset (hard-fork cutover). At this height the shielded pool
    // is reset to a fresh empty epoch (tree/anchor-history/nullifiers discarded),
    // making all pre-cutover notes unspendable. MUST equal
    // shielded_cv_binding_activation_height (cv-binding activates from block 1 of
    // the new epoch) — SelectParams enforces this. Discarding the weak pool at the
    // reset is what closes the [input_binding, cv) mint window instead of carrying
    // it forward. The struct default is UINT32_MAX (dormant); MAINNET set 61000
    // and the cutover has happened, so the mainnet window is closed. Any new
    // height MUST be chosen by a human + fleet-coordinated before it ships.
    uint32_t shielded_epoch_reset_height = UINT32_MAX;

    // Genesis block parameters
    GenesisParams genesis;

    // ===========================================================================
    // ANTI-SELF-CHAIN SAFEGUARDS
    // ===========================================================================
    // These parameters prevent nodes from accidentally creating their own chain

    // Minimum cumulative chainwork required to consider a chain valid
    // Protects against accepting chains with trivial work (accidental self-chains)
    std::string nMinimumChainWork;  // Hex string of uint256

    // Block hash to assume valid (skip signature verification up to this point)
    // Provides a performance optimization and additional chain anchoring
    std::string defaultAssumeValid;  // Hex string of block hash
    uint32_t assumeValidHeight;      // Height of the assumevalid block

    // DNS seed domains for peer discovery
    // Nodes query these domains to find initial peers
    std::vector<std::string> vSeeds;

    // Hardcoded seed nodes (IP:port) as fallback
    // Used when DNS seeds fail or for initial bootstrap
    std::vector<std::string> vFixedSeeds;

    // Checkpoints: height -> block hash
    // Prevents reorganization past these points
    std::map<uint32_t, std::string> vCheckpoints;
};

// ============================================================================
// Canonical accessors (implemented ONLY in consensus/chainparams_impl.cpp)
// DO NOT implement these in headers or other compilation units
// ============================================================================

/**
 * Select the active chain parameters
 * Must be called before any network operations
 */
void SelectParams(Chain chain);

// ============================================================================
// IMPLEMENTATION DETAIL NAMESPACE
// ============================================================================
// This namespace prevents ODR violations when linking multiple libraries
// that may have different Params() implementations (e.g., production vs test)
namespace detail {
    /**
     * Internal implementation of Params()
     * Production: Returns full ChainParams from chainparams_impl.cpp
     * Test stub: Returns minimal ChainParams from params_stub.cpp
     */
    const ChainParams& ParamsImpl();
}

/**
 * Get the currently active chain parameters
 * Returns reference to the active chain configuration
 *
 * NOTE: This is an inline wrapper to prevent duplicate symbol errors
 * when linking both dinero_crypto (with params_stub.cpp) and
 * dinero_consensus (with chainparams_impl.cpp) together.
 */
inline const ChainParams& Params() {
    return detail::ParamsImpl();
}

/**
 * Get the currently active chain
 * Returns the active chain enum value
 */
Chain GetActiveChain();

/**
 * Convert chain enum to string
 */
std::string ChainToString(Chain chain);

/**
 * Convert string to chain enum
 * Throws std::invalid_argument for unknown chains
 */
Chain StringToChain(const std::string& chain_str);

/**
 * Check if parameters have been initialized
 */
bool IsChainSelected();

/**
 * TEST-ONLY: Get mutable reference to chain params
 * WARNING: Only use in tests! Production code MUST use Params() (const).
 * This function exists to allow tests to modify runtime parameters
 * like the ZK kill-switch.
 */
ChainParams& MutableParams();

/**
 * Compute a checksum of consensus-critical chain parameters
 * Used for auditing and detecting parameter drift between nodes
 * @param params The chain parameters to checksum
 * @return SHA256 hash of critical consensus parameters as hex string
 */
std::string ConsensusChecksum(const ChainParams& params);

} // namespace dinero
