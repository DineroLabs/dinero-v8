#include "wallet/taproot_template_builder.h"
#include "crypto/tagged_hash.h"
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace dinero {

using crypto::TaggedHashArray;

static std::vector<uint8_t> EncodeCompactSize(uint64_t n) {
    std::vector<uint8_t> out;
    if (n < 0xfd) {
        out.push_back(static_cast<uint8_t>(n));
    } else if (n <= 0xffff) {
        out.push_back(0xfd);
        out.push_back(n & 0xff);
        out.push_back((n >> 8) & 0xff);
    } else if (n <= 0xffffffffULL) {
        out.push_back(0xfe);
        out.push_back(n & 0xff);
        out.push_back((n >> 8) & 0xff);
        out.push_back((n >> 16) & 0xff);
        out.push_back((n >> 24) & 0xff);
    }
    return out;
}

/// Compute tweaked output key + parity using secp256k1 directly
struct OutputKeyWithParity {
    std::array<uint8_t, 32> key;
    bool parity;
};

static bool ComputeOutputKeyInternal(
    const std::vector<uint8_t>& internal_key,
    const std::array<uint8_t, 32>& merkle_root,
    OutputKeyWithParity& out) {

    if (internal_key.size() != 32) return false;

    secp256k1_context* ctx = secp256k1_context_create(
        SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
    if (!ctx) return false;

    // Parse internal key
    secp256k1_xonly_pubkey internal_pk;
    if (!secp256k1_xonly_pubkey_parse(ctx, &internal_pk, internal_key.data())) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    // Compute tweak = TaggedHash("TapTweak", internal_key || merkle_root)
    std::vector<uint8_t> tweak_preimage;
    tweak_preimage.reserve(64);
    tweak_preimage.insert(tweak_preimage.end(), internal_key.begin(), internal_key.end());
    tweak_preimage.insert(tweak_preimage.end(), merkle_root.begin(), merkle_root.end());
    auto tweak = TaggedHashArray("TapTweak", tweak_preimage.data(), tweak_preimage.size());

    // Apply tweak
    secp256k1_pubkey output_pk;
    if (!secp256k1_xonly_pubkey_tweak_add(ctx, &output_pk, &internal_pk, tweak.data())) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    // Extract x-only key + parity
    secp256k1_xonly_pubkey output_xonly;
    int pk_parity = 0;
    if (!secp256k1_xonly_pubkey_from_pubkey(ctx, &output_xonly, &pk_parity, &output_pk)) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    if (!secp256k1_xonly_pubkey_serialize(ctx, out.key.data(), &output_xonly)) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    out.parity = (pk_parity != 0);
    secp256k1_context_destroy(ctx);
    return true;
}

// ============================================================================
// ProtectedTemplateParams Serialization
// ============================================================================

std::vector<uint8_t> ProtectedTemplateParams::Serialize() const {
    // user(32) || panic(32) || panic_window(4 LE) || recovery(32) || recovery_delay(4 LE)
    std::vector<uint8_t> out;
    out.reserve(32 + 32 + 4 + 32 + 4);

    out.insert(out.end(), user_pubkey.begin(), user_pubkey.end());
    out.insert(out.end(), panic_pubkey.begin(), panic_pubkey.end());

    out.push_back(panic_window_blocks & 0xff);
    out.push_back((panic_window_blocks >> 8) & 0xff);
    out.push_back((panic_window_blocks >> 16) & 0xff);
    out.push_back((panic_window_blocks >> 24) & 0xff);

    out.insert(out.end(), recovery_pubkey.begin(), recovery_pubkey.end());

    out.push_back(recovery_delay_blocks & 0xff);
    out.push_back((recovery_delay_blocks >> 8) & 0xff);
    out.push_back((recovery_delay_blocks >> 16) & 0xff);
    out.push_back((recovery_delay_blocks >> 24) & 0xff);

    return out;
}

ProtectedTemplateParams ProtectedTemplateParams::Deserialize(const std::vector<uint8_t>& data) {
    // 32 + 32 + 4 + 32 + 4 = 104 bytes
    if (data.size() < 104) {
        throw std::runtime_error("ProtectedTemplateParams: data too short");
    }

    ProtectedTemplateParams p;
    size_t off = 0;

    p.user_pubkey.assign(data.begin() + off, data.begin() + off + 32);
    off += 32;

    p.panic_pubkey.assign(data.begin() + off, data.begin() + off + 32);
    off += 32;

    p.panic_window_blocks = static_cast<uint32_t>(data[off])
        | (static_cast<uint32_t>(data[off + 1]) << 8)
        | (static_cast<uint32_t>(data[off + 2]) << 16)
        | (static_cast<uint32_t>(data[off + 3]) << 24);
    off += 4;

    p.recovery_pubkey.assign(data.begin() + off, data.begin() + off + 32);
    off += 32;

    p.recovery_delay_blocks = static_cast<uint32_t>(data[off])
        | (static_cast<uint32_t>(data[off + 1]) << 8)
        | (static_cast<uint32_t>(data[off + 2]) << 16)
        | (static_cast<uint32_t>(data[off + 3]) << 24);

    return p;
}

// ============================================================================
// EscrowTemplateParams Serialization
// ============================================================================

std::vector<uint8_t> EscrowTemplateParams::Serialize() const {
    // buyer(32) || seller(32) || n_attestors(1) || threshold(1) ||
    // attestors(32*n) || timeout(4 LE)
    std::vector<uint8_t> out;
    uint8_t n = static_cast<uint8_t>(attestor_pubkeys.size());
    out.reserve(32 + 32 + 1 + 1 + 32 * n + 4);

    out.insert(out.end(), buyer_pubkey.begin(), buyer_pubkey.end());
    out.insert(out.end(), seller_pubkey.begin(), seller_pubkey.end());
    out.push_back(n);
    out.push_back(attestor_threshold);

    for (const auto& pk : attestor_pubkeys) {
        out.insert(out.end(), pk.begin(), pk.end());
    }

    out.push_back(timeout_blocks & 0xff);
    out.push_back((timeout_blocks >> 8) & 0xff);
    out.push_back((timeout_blocks >> 16) & 0xff);
    out.push_back((timeout_blocks >> 24) & 0xff);

    return out;
}

EscrowTemplateParams EscrowTemplateParams::Deserialize(const std::vector<uint8_t>& data) {
    // Minimum: buyer(32) + seller(32) + n(1) + threshold(1) + timeout(4) = 70
    if (data.size() < 70) {
        throw std::runtime_error("EscrowTemplateParams: data too short");
    }

    EscrowTemplateParams p;
    size_t off = 0;

    p.buyer_pubkey.assign(data.begin() + off, data.begin() + off + 32);
    off += 32;

    p.seller_pubkey.assign(data.begin() + off, data.begin() + off + 32);
    off += 32;

    uint8_t n = data[off++];
    p.attestor_threshold = data[off++];

    if (data.size() < 70 + 32 * n) {
        throw std::runtime_error("EscrowTemplateParams: not enough attestor data");
    }

    p.attestor_pubkeys.resize(n);
    for (uint8_t i = 0; i < n; i++) {
        p.attestor_pubkeys[i].assign(data.begin() + off, data.begin() + off + 32);
        off += 32;
    }

    p.timeout_blocks = static_cast<uint32_t>(data[off])
        | (static_cast<uint32_t>(data[off + 1]) << 8)
        | (static_cast<uint32_t>(data[off + 2]) << 16)
        | (static_cast<uint32_t>(data[off + 3]) << 24);

    return p;
}

// ============================================================================
// Script Number Encoding (BIP62 minimal)
// ============================================================================

std::vector<uint8_t> TaprootTemplateBuilder::EncodeScriptNum(uint32_t value) {
    if (value == 0) {
        return {0x00};  // OP_0
    }
    if (value <= 16) {
        // OP_1 through OP_16
        return {static_cast<uint8_t>(0x50 + value)};
    }

    // CScriptNum-style minimal encoding (little-endian, sign bit in MSB)
    std::vector<uint8_t> result;
    uint32_t v = value;
    while (v > 0) {
        result.push_back(v & 0xff);
        v >>= 8;
    }
    // If MSB has sign bit set, add 0x00 to keep positive
    if (result.back() & 0x80) {
        result.push_back(0x00);
    }
    return result;
}

// ============================================================================
// Script Builders
// ============================================================================

std::vector<uint8_t> TaprootTemplateBuilder::BuildCSVScript(
    const std::vector<uint8_t>& pubkey,
    uint32_t delay_blocks) {

    // <pubkey> OP_CHECKSIG <delay> OP_CHECKSEQUENCEVERIFY OP_DROP
    //
    // Tapscript: push 32-byte key, CHECKSIG, push delay, CSV, DROP
    // The CHECKSIG consumes sig from witness, leaves 1/0 on stack.
    // CSV checks nSequence >= delay, then DROP removes the delay value.

    std::vector<uint8_t> script;
    script.reserve(32 + 1 + 1 + 6 + 1 + 1);

    // Push 32-byte pubkey
    script.push_back(0x20);  // OP_PUSHBYTES_32
    script.insert(script.end(), pubkey.begin(), pubkey.end());

    // OP_CHECKSIG
    script.push_back(0xac);

    // Push delay value
    auto delay_enc = EncodeScriptNum(delay_blocks);
    if (delay_enc.size() == 1 && delay_enc[0] >= 0x00 && delay_enc[0] <= 0x60) {
        // Already an opcode (OP_0 or OP_1..OP_16)
        script.push_back(delay_enc[0]);
    } else {
        // Push as data
        script.push_back(static_cast<uint8_t>(delay_enc.size()));
        script.insert(script.end(), delay_enc.begin(), delay_enc.end());
    }

    // OP_CHECKSEQUENCEVERIFY
    script.push_back(0xb2);

    // OP_DROP
    script.push_back(0x75);

    return script;
}

std::vector<uint8_t> TaprootTemplateBuilder::BuildAttestorScript(
    const std::vector<uint8_t>& seller_pubkey,
    const std::vector<std::vector<uint8_t>>& attestor_pubkeys,
    uint8_t threshold) {

    // <seller> OP_CHECKSIG <att1> OP_CHECKSIGADD <att2> OP_CHECKSIGADD ... <k> OP_NUMEQUAL
    //
    // BIP342 OP_CHECKSIGADD pattern:
    //   Stack before CHECKSIGADD: ... n sig pubkey
    //   Stack after:  ... (n + (sig_valid ? 1 : 0))
    //
    // So: seller CHECKSIG leaves 0 or 1
    //     att1 CHECKSIGADD adds 0 or 1
    //     ...
    //     k NUMEQUAL checks total == threshold

    std::vector<uint8_t> script;
    script.reserve(33 * (1 + attestor_pubkeys.size()) + attestor_pubkeys.size() + 3);

    // <seller_pubkey> OP_CHECKSIG
    script.push_back(0x20);
    script.insert(script.end(), seller_pubkey.begin(), seller_pubkey.end());
    script.push_back(0xac);  // OP_CHECKSIG

    // <att_i> OP_CHECKSIGADD for each attestor
    for (const auto& att_pk : attestor_pubkeys) {
        script.push_back(0x20);
        script.insert(script.end(), att_pk.begin(), att_pk.end());
        script.push_back(0xba);  // OP_CHECKSIGADD
    }

    // <threshold> OP_NUMEQUAL
    auto k_enc = EncodeScriptNum(threshold);
    if (k_enc.size() == 1 && k_enc[0] >= 0x00 && k_enc[0] <= 0x60) {
        script.push_back(k_enc[0]);
    } else {
        script.push_back(static_cast<uint8_t>(k_enc.size()));
        script.insert(script.end(), k_enc.begin(), k_enc.end());
    }
    script.push_back(0x9c);  // OP_NUMEQUAL

    return script;
}

// ============================================================================
// Merkle Tree Helpers
// ============================================================================

std::array<uint8_t, 32> TaprootTemplateBuilder::ComputeLeafHash(
    const std::vector<uint8_t>& script,
    uint8_t leaf_version) {

    // TaggedHash("TapLeaf", leaf_version || compact_size(script) || script)
    auto cs = EncodeCompactSize(script.size());

    std::vector<uint8_t> preimage;
    preimage.reserve(1 + cs.size() + script.size());
    preimage.push_back(leaf_version);
    preimage.insert(preimage.end(), cs.begin(), cs.end());
    preimage.insert(preimage.end(), script.begin(), script.end());

    return TaggedHashArray("TapLeaf", preimage.data(), preimage.size());
}

std::array<uint8_t, 32> TaprootTemplateBuilder::ComputeBranchHash(
    const std::array<uint8_t, 32>& left,
    const std::array<uint8_t, 32>& right) {

    // TaggedHash("TapBranch", sorted(left, right))
    // BIP341: lexicographically sort the two 32-byte hashes
    const std::array<uint8_t, 32>& first = (left < right) ? left : right;
    const std::array<uint8_t, 32>& second = (left < right) ? right : left;

    uint8_t preimage[64];
    std::memcpy(preimage, first.data(), 32);
    std::memcpy(preimage + 32, second.data(), 32);

    return TaggedHashArray("TapBranch", preimage, 64);
}

std::vector<uint8_t> TaprootTemplateBuilder::ComputeOutputKey(
    const std::vector<uint8_t>& internal_key,
    const std::array<uint8_t, 32>& merkle_root) {

    OutputKeyWithParity okp;
    if (!ComputeOutputKeyInternal(internal_key, merkle_root, okp)) {
        return {};
    }
    return std::vector<uint8_t>(okp.key.begin(), okp.key.end());
}

// ============================================================================
// BuildProtected
// ============================================================================

TemplateTreeResult TaprootTemplateBuilder::BuildProtected(
    const ProtectedTemplateParams& params) {

    if (params.user_pubkey.size() != 32 ||
        params.panic_pubkey.size() != 32 ||
        params.recovery_pubkey.size() != 32) {
        throw std::runtime_error("BuildProtected: all pubkeys must be 32-byte x-only");
    }

    TemplateTreeResult result;

    // --- Build leaf scripts ---
    auto panic_script = BuildCSVScript(params.panic_pubkey, params.panic_window_blocks);
    auto recovery_script = BuildCSVScript(params.recovery_pubkey, params.recovery_delay_blocks);

    // --- Compute leaf hashes ---
    auto panic_leaf_hash = ComputeLeafHash(panic_script);
    auto recovery_leaf_hash = ComputeLeafHash(recovery_script);

    // --- Compute merkle root ---
    // 2-leaf tree: merkle_root = TapBranch(panic_leaf_hash, recovery_leaf_hash)
    auto merkle_root = ComputeBranchHash(panic_leaf_hash, recovery_leaf_hash);

    // --- Compute output key with parity ---
    OutputKeyWithParity okp;
    if (!ComputeOutputKeyInternal(params.user_pubkey, merkle_root, okp)) {
        throw std::runtime_error("BuildProtected: output key computation failed");
    }

    result.output_key = okp.key;

    // --- scriptPubKey: OP_1 <32-byte output_key> ---
    result.scriptPubKey.reserve(34);
    result.scriptPubKey.push_back(0x51);  // OP_1
    result.scriptPubKey.push_back(0x20);  // push 32 bytes
    result.scriptPubKey.insert(result.scriptPubKey.end(),
        okp.key.begin(), okp.key.end());

    // --- Build control blocks ---
    // For a 2-leaf tree, each leaf's merkle path = [sibling_hash]
    std::array<uint8_t, 32> internal_key_arr;
    std::copy(params.user_pubkey.begin(), params.user_pubkey.end(), internal_key_arr.begin());

    // Panic leaf control block: path = [recovery_leaf_hash]
    TaprootControlBlock panic_cb;
    panic_cb.leaf_version = 0xc0;
    panic_cb.output_key_parity = okp.parity;
    panic_cb.internal_key = internal_key_arr;
    panic_cb.merkle_path.push_back(recovery_leaf_hash);

    // Recovery leaf control block: path = [panic_leaf_hash]
    TaprootControlBlock recovery_cb;
    recovery_cb.leaf_version = 0xc0;
    recovery_cb.output_key_parity = okp.parity;
    recovery_cb.internal_key = internal_key_arr;
    recovery_cb.merkle_path.push_back(panic_leaf_hash);

    // --- Populate leaves ---
    result.leaves.resize(2);

    result.leaves[0].label = "panic";
    result.leaves[0].script = std::move(panic_script);
    result.leaves[0].leaf_hash = panic_leaf_hash;
    result.leaves[0].control_block = std::move(panic_cb);

    result.leaves[1].label = "recovery";
    result.leaves[1].script = std::move(recovery_script);
    result.leaves[1].leaf_hash = recovery_leaf_hash;
    result.leaves[1].control_block = std::move(recovery_cb);

    // --- Build PolicyDescriptor ---
    result.policy.template_type = PolicyTemplate::PROTECTED;
    result.policy.template_version = 1;
    result.policy.params = params.Serialize();
    result.policy_id = result.policy.ComputePolicyId();

    return result;
}

// ============================================================================
// BuildEscrow
// ============================================================================

TemplateTreeResult TaprootTemplateBuilder::BuildEscrow(
    const EscrowTemplateParams& params) {

    if (params.buyer_pubkey.size() != 32 ||
        params.seller_pubkey.size() != 32) {
        throw std::runtime_error("BuildEscrow: buyer/seller pubkeys must be 32-byte x-only");
    }
    for (const auto& att : params.attestor_pubkeys) {
        if (att.size() != 32) {
            throw std::runtime_error("BuildEscrow: attestor pubkeys must be 32-byte x-only");
        }
    }
    if (params.attestor_threshold == 0 ||
        params.attestor_threshold > params.attestor_pubkeys.size() + 1) {
        // +1 because seller counts toward the threshold
        throw std::runtime_error("BuildEscrow: invalid attestor threshold");
    }

    TemplateTreeResult result;

    // --- Build leaf scripts ---
    // Release leaf: seller + attestors multisig
    auto release_script = BuildAttestorScript(
        params.seller_pubkey, params.attestor_pubkeys, params.attestor_threshold);

    // Timeout leaf: buyer can reclaim after timeout
    auto timeout_script = BuildCSVScript(params.buyer_pubkey, params.timeout_blocks);

    // --- Compute leaf hashes ---
    auto release_leaf_hash = ComputeLeafHash(release_script);
    auto timeout_leaf_hash = ComputeLeafHash(timeout_script);

    // --- Compute merkle root ---
    auto merkle_root = ComputeBranchHash(release_leaf_hash, timeout_leaf_hash);

    // --- Compute output key with parity ---
    // v1: key-path = buyer_pubkey (MuSig2(buyer, seller) in v2)
    OutputKeyWithParity okp;
    if (!ComputeOutputKeyInternal(params.buyer_pubkey, merkle_root, okp)) {
        throw std::runtime_error("BuildEscrow: output key computation failed");
    }

    result.output_key = okp.key;

    // --- scriptPubKey: OP_1 <32-byte output_key> ---
    result.scriptPubKey.reserve(34);
    result.scriptPubKey.push_back(0x51);  // OP_1
    result.scriptPubKey.push_back(0x20);  // push 32 bytes
    result.scriptPubKey.insert(result.scriptPubKey.end(),
        okp.key.begin(), okp.key.end());

    // --- Build control blocks ---
    std::array<uint8_t, 32> internal_key_arr;
    std::copy(params.buyer_pubkey.begin(), params.buyer_pubkey.end(), internal_key_arr.begin());

    // Release leaf control block: path = [timeout_leaf_hash]
    TaprootControlBlock release_cb;
    release_cb.leaf_version = 0xc0;
    release_cb.output_key_parity = okp.parity;
    release_cb.internal_key = internal_key_arr;
    release_cb.merkle_path.push_back(timeout_leaf_hash);

    // Timeout leaf control block: path = [release_leaf_hash]
    TaprootControlBlock timeout_cb;
    timeout_cb.leaf_version = 0xc0;
    timeout_cb.output_key_parity = okp.parity;
    timeout_cb.internal_key = internal_key_arr;
    timeout_cb.merkle_path.push_back(release_leaf_hash);

    // --- Populate leaves ---
    result.leaves.resize(2);

    result.leaves[0].label = "release";
    result.leaves[0].script = std::move(release_script);
    result.leaves[0].leaf_hash = release_leaf_hash;
    result.leaves[0].control_block = std::move(release_cb);

    result.leaves[1].label = "timeout";
    result.leaves[1].script = std::move(timeout_script);
    result.leaves[1].leaf_hash = timeout_leaf_hash;
    result.leaves[1].control_block = std::move(timeout_cb);

    // --- Build PolicyDescriptor ---
    result.policy.template_type = PolicyTemplate::ESCROW;
    result.policy.template_version = 1;
    result.policy.params = params.Serialize();
    result.policy_id = result.policy.ComputePolicyId();

    return result;
}

} // namespace dinero
