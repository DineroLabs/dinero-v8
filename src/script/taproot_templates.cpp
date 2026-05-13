/**
 * Phase 24.5: Taproot Template Engine Implementation
 *
 * Implements template expansion for script_tests.json Taproot test vectors.
 * Uses secp256k1 extrakeys module for BIP-341 key tweaking.
 */

#include "script/taproot_templates.h"
#include "consensus/script_interpreter.h"  // For TaggedHash functions
#include "crypto/evp_secp256k1.h"
#include "crypto/tagged_hash.h"
#include "common/sha256d.h"

#include <secp256k1.h>
#include <secp256k1_extrakeys.h>

#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstring>

namespace dinero {
namespace script {

static secp256k1_context* GetSecp256k1Context() {
    return dinero::crypto::GetSecp256k1ContextNone();
}

// ============================================================================
// Utility Functions
// ============================================================================

std::vector<uint8_t> HexToBytes(const std::string& hex) {
    std::vector<uint8_t> result;
    size_t start = 0;

    // Skip optional 0x prefix
    if (hex.size() >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        start = 2;
    }

    for (size_t i = start; i + 1 < hex.size(); i += 2) {
        char hi = hex[i];
        char lo = hex[i + 1];

        auto hexval = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return 0;
        };

        result.push_back((hexval(hi) << 4) | hexval(lo));
    }

    return result;
}

std::string BytesToHex(const std::vector<uint8_t>& bytes) {
    std::ostringstream oss;
    for (uint8_t b : bytes) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    }
    return oss.str();
}

// ============================================================================
// Single SHA256 Helper (not double)
// ============================================================================
static std::vector<uint8_t> SHA256_Single(const std::vector<uint8_t>& data) {
    Dinero::Common::sha256 hasher;
    hasher.update(data.data(), data.size());
    return hasher.finalize();
}

// BIP-340 Tagged Hash: delegates to canonical crypto::TaggedHash
static std::vector<uint8_t> TaggedHashLocal(const std::string& tag, const std::vector<uint8_t>& data) {
    return dinero::crypto::TaggedHash(tag, data);
}

// ============================================================================
// Compact Size Encoding (Bitcoin varint)
// ============================================================================
static void WriteCompactSize(std::vector<uint8_t>& out, uint64_t n) {
    if (n < 253) {
        out.push_back(static_cast<uint8_t>(n));
    } else if (n <= 0xFFFF) {
        out.push_back(253);
        out.push_back(n & 0xFF);
        out.push_back((n >> 8) & 0xFF);
    } else if (n <= 0xFFFFFFFF) {
        out.push_back(254);
        for (int i = 0; i < 4; i++) {
            out.push_back((n >> (8 * i)) & 0xFF);
        }
    } else {
        out.push_back(255);
        for (int i = 0; i < 8; i++) {
            out.push_back((n >> (8 * i)) & 0xFF);
        }
    }
}

// ============================================================================
// TapLeaf Hash Computation
// ============================================================================
std::vector<uint8_t> ComputeTapLeafHash(uint8_t leaf_version,
                                        const std::vector<uint8_t>& script) {
    // TapLeaf = tagged_hash("TapLeaf", leaf_version || compact_size(script.size()) || script)
    std::vector<uint8_t> data;
    data.push_back(leaf_version);
    WriteCompactSize(data, script.size());
    data.insert(data.end(), script.begin(), script.end());

    return TaggedHashLocal("TapLeaf", data);
}

// ============================================================================
// TapBranch Hash Computation
// ============================================================================
std::vector<uint8_t> ComputeTapBranchHash(const std::vector<uint8_t>& left,
                                          const std::vector<uint8_t>& right) {
    // Sort lexicographically
    std::vector<uint8_t> data;
    data.reserve(64);

    if (left < right) {
        data.insert(data.end(), left.begin(), left.end());
        data.insert(data.end(), right.begin(), right.end());
    } else {
        data.insert(data.end(), right.begin(), right.end());
        data.insert(data.end(), left.begin(), left.end());
    }

    return TaggedHashLocal("TapBranch", data);
}

// ============================================================================
// TapTweak Hash Computation
// ============================================================================
std::vector<uint8_t> ComputeTapTweakHash(const std::vector<uint8_t>& internal_key,
                                         const std::vector<uint8_t>& merkle_root) {
    // TapTweak = tagged_hash("TapTweak", internal_key || merkle_root)
    std::vector<uint8_t> data;
    data.reserve(internal_key.size() + merkle_root.size());
    data.insert(data.end(), internal_key.begin(), internal_key.end());
    if (!merkle_root.empty()) {
        data.insert(data.end(), merkle_root.begin(), merkle_root.end());
    }

    return TaggedHashLocal("TapTweak", data);
}

// ============================================================================
// X-Only Pubkey Tweaking using secp256k1
// ============================================================================
bool TweakXOnlyPubkey(const std::vector<uint8_t>& internal_key,
                      const std::vector<uint8_t>& tweak,
                      std::vector<uint8_t>& output_key,
                      int& output_parity) {
    if (internal_key.size() != 32 || tweak.size() != 32) {
        return false;
    }

    secp256k1_context* ctx = GetSecp256k1Context();
    if (!ctx) return false;

    // Parse x-only internal key
    secp256k1_xonly_pubkey xonly_internal;
    if (!secp256k1_xonly_pubkey_parse(ctx, &xonly_internal, internal_key.data())) {
        return false;
    }

    // Tweak the key: Q = P + tweak * G
    secp256k1_pubkey output_pubkey;
    if (!secp256k1_xonly_pubkey_tweak_add(ctx, &output_pubkey, &xonly_internal, tweak.data())) {
        return false;
    }

    // Convert result to x-only and get parity
    secp256k1_xonly_pubkey xonly_output;
    if (!secp256k1_xonly_pubkey_from_pubkey(ctx, &xonly_output, &output_parity, &output_pubkey)) {
        return false;
    }

    // Serialize output key
    output_key.resize(32);
    secp256k1_xonly_pubkey_serialize(ctx, output_key.data(), &xonly_output);

    return true;
}

// ============================================================================
// Control Block Construction
// ============================================================================
std::vector<uint8_t> BuildControlBlock(uint8_t leaf_version,
                                       int output_parity,
                                       const std::vector<uint8_t>& internal_key,
                                       const std::vector<std::vector<uint8_t>>& merkle_path) {
    std::vector<uint8_t> control_block;

    // First byte: leaf_version | (output_parity ? 0x01 : 0x00)
    uint8_t first_byte = (leaf_version & 0xfe) | (output_parity & 0x01);
    control_block.push_back(first_byte);

    // Next 32 bytes: internal key
    control_block.insert(control_block.end(), internal_key.begin(), internal_key.end());

    // Remaining: merkle path (each branch is 32 bytes)
    for (const auto& branch : merkle_path) {
        control_block.insert(control_block.end(), branch.begin(), branch.end());
    }

    return control_block;
}

// ============================================================================
// Standard Test Internal Key
// ============================================================================
std::vector<uint8_t> GetTestInternalKey() {
    // BIP-341 "nothing up my sleeve" test key
    // This is the x-coordinate of the NUMS point H = lift_x(SHA256("TapTweak"))
    // For testing we use a well-known key that anyone can verify
    // Using the generator point's x-coordinate is a common test choice
    //
    // Standard test key from Bitcoin Core:
    // 0x0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798 (compressed)
    // X-only: 79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798
    return HexToBytes("79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798");
}

// ============================================================================
// Placeholder Detection
// ============================================================================
bool HasTaprootPlaceholders(const std::string& str) {
    return str.find("#TAPROOTOUTPUT#") != std::string::npos ||
           str.find("#CONTROLBLOCK#") != std::string::npos ||
           str.find("#SCRIPT#") != std::string::npos;
}

// ============================================================================
// Witness Placeholder Expansion
// ============================================================================
std::string ExpandWitnessPlaceholder(const std::string& witness_element,
                                     const TaprootTemplateResult& result) {
    if (witness_element == "#CONTROLBLOCK#") {
        return BytesToHex(result.control_block);
    }

    // Handle #SCRIPT# followed by assembly (e.g., "#SCRIPT# HASH256 DUP")
    if (witness_element.find("#SCRIPT#") == 0) {
        return BytesToHex(result.script);
    }

    return witness_element;
}

// ============================================================================
// ScriptPubKey Placeholder Expansion
// ============================================================================
std::string ExpandScriptPubKeyPlaceholder(const std::string& script_str,
                                          const TaprootTemplateResult& result) {
    std::string output = script_str;

    // Replace #TAPROOTOUTPUT# with the computed output key
    size_t pos = output.find("#TAPROOTOUTPUT#");
    if (pos != std::string::npos) {
        output.replace(pos, 15, BytesToHex(result.output_key));
    }

    return output;
}

// ============================================================================
// Main Template Expansion
// ============================================================================
TaprootTemplateResult ExpandTaprootTemplate(const TaprootTemplateContext& ctx) {
    TaprootTemplateResult result;

    // Use provided internal key or default test key
    std::vector<uint8_t> internal_key = ctx.internal_key;
    if (internal_key.empty()) {
        internal_key = GetTestInternalKey();
    }

    if (internal_key.size() != 32) {
        result.error = "Invalid internal key size";
        return result;
    }

    // Store script in result
    result.script = ctx.script;

    // Compute TapLeaf hash
    result.tapleaf_hash = ComputeTapLeafHash(ctx.leaf_version, ctx.script);

    // Compute Merkle root
    // For single leaf (no branches), merkle_root = tapleaf_hash
    if (ctx.merkle_branches.empty()) {
        result.merkle_root = result.tapleaf_hash;
    } else {
        // Multi-leaf tree: compute merkle root from branches
        std::vector<uint8_t> current = result.tapleaf_hash;
        for (const auto& branch : ctx.merkle_branches) {
            current = ComputeTapBranchHash(current, branch);
        }
        result.merkle_root = current;
    }

    // Compute tweak
    std::vector<uint8_t> tweak = ComputeTapTweakHash(internal_key, result.merkle_root);

    // Tweak the internal key
    if (!TweakXOnlyPubkey(internal_key, tweak, result.output_key, result.output_parity)) {
        result.error = "Key tweaking failed";
        return result;
    }

    // Build control block
    result.control_block = BuildControlBlock(
        ctx.leaf_version,
        result.output_parity,
        internal_key,
        ctx.merkle_branches
    );

    result.success = true;
    return result;
}

} // namespace script
} // namespace dinero
