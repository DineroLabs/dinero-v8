#pragma once

#include "wallet/policy_descriptor.h"
#include "wallet/taproot_control_block.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dinero {

// ============================================================================
// Template Parameters
// ============================================================================

/**
 * PROTECTED template: user key-path + panic leaf + recovery leaf
 *
 * Tree structure (depth 2):
 *   key-path: user_pubkey (tweaked with merkle root)
 *   leaf_A (panic):    <panic_pubkey> OP_CHECKSIG <panic_window> OP_CSV OP_DROP
 *   leaf_B (recovery): <recovery_pubkey> OP_CHECKSIG <recovery_delay> OP_CSV OP_DROP
 */
struct ProtectedTemplateParams {
    std::vector<uint8_t> user_pubkey;       // 32-byte x-only (key-path internal key)
    std::vector<uint8_t> panic_pubkey;      // 32-byte x-only
    uint32_t panic_window_blocks;            // CSV for panic leaf (e.g., 6 blocks ~1h)
    std::vector<uint8_t> recovery_pubkey;   // 32-byte x-only
    uint32_t recovery_delay_blocks;          // CSV for recovery leaf (e.g., 25920 ~6mo)

    /**
     * Serialize params for PolicyDescriptor.params field.
     * Format: user(32) || panic(32) || panic_window(4 LE) || recovery(32) || recovery_delay(4 LE)
     */
    std::vector<uint8_t> Serialize() const;
    static ProtectedTemplateParams Deserialize(const std::vector<uint8_t>& data);
};

/**
 * ESCROW template: buyer key-path + attestor release leaf + timeout refund leaf
 *
 * Tree structure (depth 2):
 *   key-path: buyer_pubkey (tweaked with merkle root)
 *   leaf_A (release): <seller> OP_CHECKSIG <att1> OP_CHECKSIGADD ... <k> OP_NUMEQUAL
 *   leaf_B (timeout): <buyer> OP_CHECKSIG <timeout> OP_CSV OP_DROP
 *
 * Note: v1 key-path = buyer_pubkey only. MuSig2(buyer, seller) in v2.
 */
struct EscrowTemplateParams {
    std::vector<uint8_t> buyer_pubkey;                      // 32-byte x-only
    std::vector<uint8_t> seller_pubkey;                     // 32-byte x-only
    std::vector<std::vector<uint8_t>> attestor_pubkeys;     // 32-byte x-only each
    uint8_t attestor_threshold;                              // k-of-n threshold
    uint32_t timeout_blocks;                                 // CSV for buyer refund

    std::vector<uint8_t> Serialize() const;
    static EscrowTemplateParams Deserialize(const std::vector<uint8_t>& data);
};

// ============================================================================
// Template Build Result
// ============================================================================

struct TemplateTreeResult {
    /// Tweaked output key (32-byte x-only)
    std::array<uint8_t, 32> output_key;

    /// scriptPubKey: OP_1 <32-byte output_key>
    std::vector<uint8_t> scriptPubKey;

    /// Per-leaf info for script-path spending
    struct LeafInfo {
        std::string label;                  // "panic", "recovery", "release", "timeout"
        std::vector<uint8_t> script;        // Tapscript
        std::array<uint8_t, 32> leaf_hash;  // TaggedHash("TapLeaf", ...)
        TaprootControlBlock control_block;  // For witness construction
    };
    std::vector<LeafInfo> leaves;

    /// Policy descriptor for this output
    PolicyDescriptor policy;

    /// Deterministic policy ID
    std::array<uint8_t, 32> policy_id;
};

// ============================================================================
// Template Builder
// ============================================================================

class TaprootTemplateBuilder {
public:
    /**
     * Build PROTECTED template tree.
     *
     * key-path: user_pubkey tweaked with merkle_root(panic_leaf, recovery_leaf)
     * leaf_A: <panic_pubkey> OP_CHECKSIG <panic_window> OP_CHECKSEQUENCEVERIFY OP_DROP
     * leaf_B: <recovery_pubkey> OP_CHECKSIG <recovery_delay> OP_CHECKSEQUENCEVERIFY OP_DROP
     *
     * @param params Protected template parameters
     * @return TemplateTreeResult with output key, leaves, and policy
     */
    static TemplateTreeResult BuildProtected(const ProtectedTemplateParams& params);

    /**
     * Build ESCROW template tree.
     *
     * key-path: buyer_pubkey tweaked with merkle_root(release_leaf, timeout_leaf)
     * leaf_A: <seller> OP_CHECKSIG <att1> OP_CHECKSIGADD ... <k> OP_NUMEQUAL
     * leaf_B: <buyer> OP_CHECKSIG <timeout> OP_CHECKSEQUENCEVERIFY OP_DROP
     *
     * @param params Escrow template parameters
     * @return TemplateTreeResult with output key, leaves, and policy
     */
    static TemplateTreeResult BuildEscrow(const EscrowTemplateParams& params);

    /// Compute tweaked output key from internal key and merkle root
    static std::vector<uint8_t> ComputeOutputKey(
        const std::vector<uint8_t>& internal_key,
        const std::array<uint8_t, 32>& merkle_root
    );

private:
    /// Build Tapscript: <pubkey> OP_CHECKSIG <delay> OP_CHECKSEQUENCEVERIFY OP_DROP
    static std::vector<uint8_t> BuildCSVScript(
        const std::vector<uint8_t>& pubkey,
        uint32_t delay_blocks
    );

    /// Build Tapscript: <seller> OP_CHECKSIG <att1> OP_CHECKSIGADD ... <k> OP_NUMEQUAL
    static std::vector<uint8_t> BuildAttestorScript(
        const std::vector<uint8_t>& seller_pubkey,
        const std::vector<std::vector<uint8_t>>& attestor_pubkeys,
        uint8_t threshold
    );

    /// Compute TaggedHash("TapLeaf", leaf_version || compact_size(script) || script)
    static std::array<uint8_t, 32> ComputeLeafHash(
        const std::vector<uint8_t>& script,
        uint8_t leaf_version = 0xc0
    );

    /// Compute TaggedHash("TapBranch", sorted(left, right))
    static std::array<uint8_t, 32> ComputeBranchHash(
        const std::array<uint8_t, 32>& left,
        const std::array<uint8_t, 32>& right
    );

    /// Encode integer for Tapscript (minimal encoding)
    static std::vector<uint8_t> EncodeScriptNum(uint32_t value);
};

} // namespace dinero
