#include "consensus/script_interpreter.h"
#include "primitives/transaction.h"
#include "crypto/tagged_hash.h"
#include "common/sha256d.h"
#include <cstring>
#include <algorithm>

namespace dinero {
namespace consensus {

// ============================================================================
// Helper Functions for Sighash Computation
// ============================================================================

/**
 * Single SHA256 hash (returns raw bytes, not hex)
 */
static std::vector<uint8_t> SHA256_Single(const std::vector<uint8_t>& data) {
    Dinero::Common::sha256 hasher;
    hasher.update(data.data(), data.size());
    return hasher.finalize();
}

static std::vector<uint8_t> SHA256_Single(const uint8_t* data, size_t len) {
    Dinero::Common::sha256 hasher;
    hasher.update(data, len);
    return hasher.finalize();
}

// Forward declaration of TaggedHash (implemented at end of file)
std::vector<uint8_t> TaggedHash(const std::string& tag, const std::vector<uint8_t>& data);

static void WriteUint32LE(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(value & 0xFF);
    out.push_back((value >> 8) & 0xFF);
    out.push_back((value >> 16) & 0xFF);
    out.push_back((value >> 24) & 0xFF);
}

static void WriteInt32LE(std::vector<uint8_t>& out, int32_t value) {
    WriteUint32LE(out, static_cast<uint32_t>(value));
}

static void WriteUint64LE(std::vector<uint8_t>& out, uint64_t value) {
    for (int i = 0; i < 8; i++) {
        out.push_back((value >> (i * 8)) & 0xFF);
    }
}

static void WriteVarint(std::vector<uint8_t>& out, uint64_t value) {
    if (value < 0xFD) {
        out.push_back(static_cast<uint8_t>(value));
    } else if (value <= 0xFFFF) {
        out.push_back(0xFD);
        out.push_back(value & 0xFF);
        out.push_back((value >> 8) & 0xFF);
    } else if (value <= 0xFFFFFFFF) {
        out.push_back(0xFE);
        WriteUint32LE(out, static_cast<uint32_t>(value));
    } else {
        out.push_back(0xFF);
        WriteUint64LE(out, value);
    }
}

static void WriteScript(std::vector<uint8_t>& out, const std::vector<uint8_t>& script) {
    WriteVarint(out, script.size());
    out.insert(out.end(), script.begin(), script.end());
}

static std::vector<uint8_t> HexToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < hex.length(); i += 2) {
        uint8_t byte = static_cast<uint8_t>(std::strtol(hex.substr(i, 2).c_str(), nullptr, 16));
        bytes.push_back(byte);
    }
    return bytes;
}

static bool HasConfidentialPrevoutContext(const ScriptExecutionContext& ctx) {
    if (!ctx.tx) {
        return false;
    }

    const size_t input_count = ctx.tx->vin.size();
    if (ctx.all_amounts.size() != input_count ||
        ctx.all_scriptpubkeys.size() != input_count ||
        ctx.all_confidential_flags.size() != input_count ||
        ctx.all_input_commitments.size() != input_count) {
        return false;
    }

    return std::any_of(
        ctx.all_confidential_flags.begin(),
        ctx.all_confidential_flags.end(),
        [](uint8_t flag) { return flag != 0; });
}

static bool HasFullTaprootPrevoutContext(const ScriptExecutionContext& ctx) {
    if (!ctx.tx) {
        return false;
    }

    const size_t input_count = ctx.tx->vin.size();
    return ctx.all_amounts.size() == input_count &&
           ctx.all_scriptpubkeys.size() == input_count &&
           ctx.all_confidential_flags.size() == input_count &&
           ctx.all_input_commitments.size() == input_count;
}

static uint64_t GetTaprootInputAmount(const ScriptExecutionContext& ctx, size_t index) {
    const bool is_confidential =
        index < ctx.all_confidential_flags.size() && ctx.all_confidential_flags[index] != 0;
    if (is_confidential) {
        return 0;
    }

    if (ctx.tx && !ctx.all_amounts.empty() && ctx.all_amounts.size() == ctx.tx->vin.size()) {
        return ctx.all_amounts[index];
    }

    if (index == ctx.input_index) {
        return ctx.amount;
    }

    return 0;
}

static void WriteConfidentialPrevoutDescriptor(
    std::vector<uint8_t>& out,
    const ScriptExecutionContext& ctx,
    size_t index
) {
    const bool is_confidential =
        index < ctx.all_confidential_flags.size() && ctx.all_confidential_flags[index] != 0;

    out.push_back(is_confidential ? 1 : 0);
    if (!is_confidential || index >= ctx.all_input_commitments.size()) {
        WriteVarint(out, 0);
        return;
    }

    const auto& commitment = ctx.all_input_commitments[index];
    WriteVarint(out, commitment.size());
    out.insert(out.end(), commitment.begin(), commitment.end());
}

static std::vector<uint8_t> ComputeConfidentialPrevoutExtension(
    const ScriptExecutionContext& ctx,
    bool anyonecanpay
) {
    if (!HasConfidentialPrevoutContext(ctx) || !ctx.tx) {
        return {};
    }

    std::vector<uint8_t> data;
    data.push_back(0x01);  // CT prevout extension version
    data.push_back(anyonecanpay ? 0x01 : 0x00);

    if (anyonecanpay) {
        WriteVarint(data, 1);
        WriteConfidentialPrevoutDescriptor(data, ctx, ctx.input_index);
    } else {
        WriteVarint(data, ctx.tx->vin.size());
        for (size_t i = 0; i < ctx.tx->vin.size(); ++i) {
            WriteConfidentialPrevoutDescriptor(data, ctx, i);
        }
    }

    return TaggedHash("dinero/ct-prevouts/v1", data);
}

// ============================================================================
// Phase 24.3: Legacy Signature Hash (Pre-SegWit)
// ============================================================================

/**
 * Legacy signature hash (pre-SegWit)
 *
 * This implements the original Bitcoin signature hash algorithm used for
 * P2PKH, P2SH, and other pre-SegWit transaction types.
 */
std::vector<uint8_t> SignatureHashLegacy(
    const Script& script_code,
    const ScriptExecutionContext& ctx,
    uint8_t hash_type
) {
    if (!ctx.tx) {
        return std::vector<uint8_t>(32, 0);
    }

    const Transaction& tx = *ctx.tx;
    const uint32_t input_index = ctx.input_index;

    // Validate input index
    if (input_index >= tx.vin.size()) {
        return std::vector<uint8_t>(32, 1);  // Return "1" hash for invalid index
    }

    // Extract base hash type and ANYONECANPAY flag
    uint8_t base_type = hash_type & 0x1F;
    bool anyonecanpay = (hash_type & SIGHASH_ANYONECANPAY) != 0;

    // Begin serialization
    std::vector<uint8_t> data;

    // 1. Transaction version (4 bytes)
    WriteInt32LE(data, tx.version);

    // 2. Inputs
    if (anyonecanpay) {
        // SIGHASH_ANYONECANPAY: Only serialize the current input
        WriteVarint(data, 1);

        // Previous output - Serialize prevout txid and vout
        // CRITICAL: Txids MUST be serialized in little-endian (internal) byte order.
        // DO NOT reverse! Bitcoin wire format uses little-endian, NOT display format (big-endian).
        // See Phase 12b bugfix (commit 76e72f45): reversing caused all signatures to fail.
        const auto& txid_u256 = tx.vin[input_index].prevout.txid.AsUint256();
        data.insert(data.end(), txid_u256.data, txid_u256.data + 32);
        WriteUint32LE(data, tx.vin[input_index].prevout.vout);

        // ScriptSig = script_code for the input being signed
        WriteScript(data, script_code.data());

        // Sequence
        WriteUint32LE(data, tx.vin[input_index].sequence);
    } else {
        // Serialize all inputs
        WriteVarint(data, tx.vin.size());

        for (size_t i = 0; i < tx.vin.size(); i++) {
            // Previous output - Serialize prevout txid and vout
            // CRITICAL: Txids MUST be serialized in little-endian (internal) byte order.
            // DO NOT reverse! Bitcoin wire format uses little-endian, NOT display format (big-endian).
            // See Phase 12b bugfix (commit 76e72f45): reversing caused all signatures to fail.
            const auto& txid_u256 = tx.vin[i].prevout.txid.AsUint256();
            data.insert(data.end(), txid_u256.data, txid_u256.data + 32);
            WriteUint32LE(data, tx.vin[i].prevout.vout);

            // ScriptSig
            if (i == input_index) {
                // For the input being signed, use script_code
                WriteScript(data, script_code.data());
            } else {
                // For other inputs, use empty script
                WriteVarint(data, 0);
            }

            // Sequence
            if (i != input_index && (base_type == SIGHASH_SINGLE || base_type == SIGHASH_NONE)) {
                // SIGHASH_SINGLE/NONE: Set sequence to 0 for other inputs
                WriteUint32LE(data, 0);
            } else {
                WriteUint32LE(data, tx.vin[i].sequence);
            }
        }
    }

    // 3. Outputs
    if (base_type == SIGHASH_NONE) {
        // SIGHASH_NONE: No outputs
        WriteVarint(data, 0);
    } else if (base_type == SIGHASH_SINGLE) {
        // SIGHASH_SINGLE: Only the output at the same index as the input
        if (input_index >= tx.vout.size()) {
            // Out of bounds - return "1" hash (Bitcoin Core behavior)
            return std::vector<uint8_t>(32, 1);
        }

        WriteVarint(data, input_index + 1);

        // Serialize null outputs before the one we care about
        for (size_t i = 0; i < input_index; i++) {
            WriteUint64LE(data, 0xFFFFFFFFFFFFFFFFULL);  // -1 value
            WriteVarint(data, 0);  // Empty script
        }

        // Serialize the actual output
        // Phase M.6.1: Extract raw value for serialization
        WriteUint64LE(data, tx.vout[input_index].value.GetUna());
        WriteScript(data, tx.vout[input_index].scriptPubKey);
    } else {
        // SIGHASH_ALL (default): All outputs
        WriteVarint(data, tx.vout.size());

        for (const auto& output : tx.vout) {
            // Phase M.6.1: Extract raw value for serialization
            WriteUint64LE(data, output.value.GetUna());
            WriteScript(data, output.scriptPubKey);
        }
    }

    // 4. Lock time (4 bytes)
    WriteUint32LE(data, tx.lockTime);

    // 5. Hash type (4 bytes)
    WriteUint32LE(data, hash_type);

    // 6. Compute double SHA256 (use raw bytes for signature verification)
    return Dinero::Common::double_sha256_raw(data);
}

/**
 * BIP 143: Witness v0 signature hash
 *
 * This implements the SegWit v0 signature hash algorithm which prevents
 * quadratic hashing and makes hardware wallets more efficient.
 */
std::vector<uint8_t> SignatureHashWitness(
    const Script& script_code,
    const ScriptExecutionContext& ctx,
    uint8_t hash_type
) {
    if (!ctx.tx) {
        return std::vector<uint8_t>(32, 0);
    }

    const Transaction& tx = *ctx.tx;
    const uint32_t input_index = ctx.input_index;

    if (input_index >= tx.vin.size()) {
        return std::vector<uint8_t>(32, 0);
    }

    // Extract base hash type and ANYONECANPAY flag
    uint8_t base_type = hash_type & 0x1F;
    bool anyonecanpay = (hash_type & SIGHASH_ANYONECANPAY) != 0;

    std::vector<uint8_t> data;

    // 1. nVersion (4 bytes)
    WriteInt32LE(data, tx.version);

    // 2. hashPrevouts (32 bytes)
    if (!anyonecanpay) {
        std::vector<uint8_t> prevouts;
        for (const auto& input : tx.vin) {
            // CRITICAL: Txids in little-endian (internal) byte order - DO NOT reverse!
            // See Phase 12b bugfix (commit 76e72f45): reversing caused signature failures.
            const auto& txid_u256 = input.prevout.txid.AsUint256();
            prevouts.insert(prevouts.end(), txid_u256.data, txid_u256.data + 32);
            WriteUint32LE(prevouts, input.prevout.vout);
        }
        std::vector<uint8_t> hash_prevouts = Dinero::Common::double_sha256_raw(prevouts);
        data.insert(data.end(), hash_prevouts.begin(), hash_prevouts.end());
    } else {
        // Zero hash for ANYONECANPAY
        data.insert(data.end(), 32, 0);
    }

    // 3. hashSequence (32 bytes)
    if (!anyonecanpay && base_type != SIGHASH_SINGLE && base_type != SIGHASH_NONE) {
        std::vector<uint8_t> sequences;
        for (const auto& input : tx.vin) {
            WriteUint32LE(sequences, input.sequence);
        }
        std::vector<uint8_t> hash_sequence = Dinero::Common::double_sha256_raw(sequences);
        data.insert(data.end(), hash_sequence.begin(), hash_sequence.end());
    } else {
        // Zero hash for ANYONECANPAY, SINGLE, or NONE
        data.insert(data.end(), 32, 0);
    }

    // 4. outpoint (36 bytes) - Serialize prevout txid and vout
    // CRITICAL: Txids MUST be serialized in little-endian (internal) byte order.
    // DO NOT reverse! Bitcoin wire format uses little-endian, NOT display format (big-endian).
    // See Phase 12b bugfix (commit 76e72f45): reversing caused all signatures to fail.
    const auto& txid_u256 = tx.vin[input_index].prevout.txid.AsUint256();
    data.insert(data.end(), txid_u256.data, txid_u256.data + 32);
    WriteUint32LE(data, tx.vin[input_index].prevout.vout);

    // 5. scriptCode
    WriteScript(data, script_code.data());

    // 6. value (8 bytes) - need to get from prev output (use ctx.amount if available)
    WriteUint64LE(data, ctx.amount);

    // 7. nSequence (4 bytes)
    WriteUint32LE(data, tx.vin[input_index].sequence);

    // 8. hashOutputs (32 bytes)
    if (base_type != SIGHASH_SINGLE && base_type != SIGHASH_NONE) {
        // SIGHASH_ALL: hash all outputs
        std::vector<uint8_t> outputs;
        for (const auto& output : tx.vout) {
            // Phase M.6.1: Extract raw value for serialization
            WriteUint64LE(outputs, output.value.GetUna());
            WriteScript(outputs, output.scriptPubKey);
        }
        std::vector<uint8_t> hash_outputs = Dinero::Common::double_sha256_raw(outputs);
        data.insert(data.end(), hash_outputs.begin(), hash_outputs.end());
    } else if (base_type == SIGHASH_SINGLE && input_index < tx.vout.size()) {
        // SIGHASH_SINGLE: hash only the output at the same index
        std::vector<uint8_t> output;
        // Phase M.6.1: Extract raw value for serialization
        WriteUint64LE(output, tx.vout[input_index].value.GetUna());
        WriteScript(output, tx.vout[input_index].scriptPubKey);
        std::vector<uint8_t> hash_output = Dinero::Common::double_sha256_raw(output);
        data.insert(data.end(), hash_output.begin(), hash_output.end());
    } else {
        // Zero hash for NONE or out-of-bounds SINGLE
        data.insert(data.end(), 32, 0);
    }

    // 9. nLockTime (4 bytes)
    WriteUint32LE(data, tx.lockTime);

    // 10. sighash type (4 bytes)
    WriteUint32LE(data, hash_type);

    // 11. Compute double SHA256 (use raw bytes for signature verification)
    return Dinero::Common::double_sha256_raw(data);
}

/**
 * BIP 341: Taproot signature hash
 *
 * This implements the Taproot (SegWit v1) signature hash algorithm.
 * It uses tagged hashes per BIP 340: SHA256(SHA256(tag) || SHA256(tag) || msg)
 *
 * The final hash uses the "TapSighash" tag.
 * Internal hashes (sha_prevouts, sha_amounts, etc.) use single SHA256.
 */
std::vector<uint8_t> SignatureHashTaproot(
    const ScriptExecutionContext& ctx,
    uint8_t hash_type,
    const std::vector<uint8_t>& leaf_hash,
    const std::vector<uint8_t>& annex
) {
    if (!ctx.tx) {
        return std::vector<uint8_t>(32, 0);
    }

    const Transaction& tx = *ctx.tx;
    const uint32_t input_index = ctx.input_index;

    if (input_index >= tx.vin.size()) {
        return std::vector<uint8_t>(32, 0);
    }

    if (!HasFullTaprootPrevoutContext(ctx)) {
        return {};
    }

    // BIP341 only defines DEFAULT, ALL/NONE/SINGLE, and the three
    // ANYONECANPAY variants. Undefined values must fail rather than being
    // silently masked into a defined mode.
    switch (hash_type) {
        case 0x00:
        case SIGHASH_ALL:
        case SIGHASH_NONE:
        case SIGHASH_SINGLE:
        case SIGHASH_ALL | SIGHASH_ANYONECANPAY:
        case SIGHASH_NONE | SIGHASH_ANYONECANPAY:
        case SIGHASH_SINGLE | SIGHASH_ANYONECANPAY:
            break;
        default:
            return {};
    }

    // Default sighash type for Taproot (SIGHASH_DEFAULT = 0x00 is treated as SIGHASH_ALL)
    uint8_t effective_hash_type = (hash_type == 0) ? SIGHASH_ALL : hash_type;

    // Extract base hash type and ANYONECANPAY flag
    uint8_t base_type = effective_hash_type & 0x1F;
    bool anyonecanpay = (effective_hash_type & SIGHASH_ANYONECANPAY) != 0;
    if (base_type == SIGHASH_SINGLE && input_index >= tx.vout.size()) {
        return {};
    }

    std::vector<uint8_t> data;

    // 1. Epoch (1 byte) - always 0 for BIP 341
    data.push_back(0);

    // 2. Hash type (1 byte) - use original hash_type (0 for SIGHASH_DEFAULT)
    data.push_back(hash_type);

    // 3. nVersion (4 bytes)
    WriteInt32LE(data, tx.version);

    // 4. nLockTime (4 bytes)
    WriteUint32LE(data, tx.lockTime);

    // 5. sha_prevouts (32 bytes) - if not ANYONECANPAY
    // BIP 341: single SHA256 of all outpoints
    if (!anyonecanpay) {
        std::vector<uint8_t> prevouts;
        for (const auto& input : tx.vin) {
            // CRITICAL: Txids in little-endian (internal) byte order - DO NOT reverse!
            // See Phase 12b bugfix (commit 76e72f45): reversing caused signature failures.
            const auto& txid_u256 = input.prevout.txid.AsUint256();
            prevouts.insert(prevouts.end(), txid_u256.data, txid_u256.data + 32);
            WriteUint32LE(prevouts, input.prevout.vout);
        }
        std::vector<uint8_t> sha_prevouts = SHA256_Single(prevouts);
        data.insert(data.end(), sha_prevouts.begin(), sha_prevouts.end());
    }

    // 6. sha_amounts (32 bytes) - if not ANYONECANPAY
    // BIP 341: single SHA256 of all input amounts
    if (!anyonecanpay) {
        std::vector<uint8_t> amounts;
        for (size_t i = 0; i < tx.vin.size(); ++i) {
            WriteUint64LE(amounts, GetTaprootInputAmount(ctx, i));
        }
        std::vector<uint8_t> sha_amounts = SHA256_Single(amounts);
        data.insert(data.end(), sha_amounts.begin(), sha_amounts.end());
    }

    // 7. sha_scriptpubkeys (32 bytes) - if not ANYONECANPAY
    // BIP 341: single SHA256 of all input scriptPubKeys (with compact size prefix)
    if (!anyonecanpay) {
        std::vector<uint8_t> scriptpubkeys;
        for (const auto& spk : ctx.all_scriptpubkeys) {
            WriteScript(scriptpubkeys, spk);
        }
        std::vector<uint8_t> sha_scriptpubkeys = SHA256_Single(scriptpubkeys);
        data.insert(data.end(), sha_scriptpubkeys.begin(), sha_scriptpubkeys.end());
    }

    // 8. sha_sequences (32 bytes) - if not ANYONECANPAY
    // BIP 341: single SHA256 of all sequences
    if (!anyonecanpay) {
        std::vector<uint8_t> sequences;
        for (const auto& input : tx.vin) {
            WriteUint32LE(sequences, input.sequence);
        }
        std::vector<uint8_t> sha_sequences = SHA256_Single(sequences);
        data.insert(data.end(), sha_sequences.begin(), sha_sequences.end());
    }

    // 9. sha_outputs (32 bytes) - only ALL/DEFAULT. BIP341 places the
    // SIGHASH_SINGLE output commitment later, after the annex commitment.
    if (base_type != SIGHASH_NONE && base_type != SIGHASH_SINGLE) {
        std::vector<uint8_t> outputs;
        for (const auto& output : tx.vout) {
            WriteUint64LE(outputs, output.value.GetUna());
            WriteScript(outputs, output.scriptPubKey);
        }
        std::vector<uint8_t> sha_outputs = SHA256_Single(outputs);
        data.insert(data.end(), sha_outputs.begin(), sha_outputs.end());
    }

    // 10. spend_type (1 byte)
    // spend_type = (ext_flag * 2) + annex_present.
    uint8_t ext_flag = leaf_hash.empty() ? 0 : 1;
    uint8_t annex_present = annex.empty() ? 0 : 1;
    uint8_t spend_type = (ext_flag << 1) | annex_present;
    data.push_back(spend_type);

    // 11. Input data (depends on ANYONECANPAY)
    if (anyonecanpay) {
        // Serialize the current input directly
        // CRITICAL: Txids in little-endian (internal) byte order - DO NOT reverse!
        // See Phase 12b bugfix (commit 76e72f45): reversing caused signature failures.
        const auto& txid_u256 = tx.vin[input_index].prevout.txid.AsUint256();
        data.insert(data.end(), txid_u256.data, txid_u256.data + 32);
        WriteUint32LE(data, tx.vin[input_index].prevout.vout);

        // Amount
        WriteUint64LE(data, GetTaprootInputAmount(ctx, input_index));

        // scriptPubKey of the spent output
        WriteScript(data, ctx.all_scriptpubkeys[input_index]);

        // Sequence
        WriteUint32LE(data, tx.vin[input_index].sequence);
    } else {
        // Input index (4 bytes)
        WriteUint32LE(data, input_index);
    }

    // 12. BIP341 annex hash (if annex_present)
    // sha_annex = SHA256(compact_size(len(annex)) || annex)
    if (!annex.empty()) {
        std::vector<uint8_t> annex_serialized;
        WriteVarint(annex_serialized, annex.size());
        annex_serialized.insert(annex_serialized.end(), annex.begin(), annex.end());
        std::vector<uint8_t> sha_annex = SHA256_Single(annex_serialized);
        data.insert(data.end(), sha_annex.begin(), sha_annex.end());
    }

    // 13. SIGHASH_SINGLE commits to its corresponding output here,
    // regardless of ANYONECANPAY.
    if (base_type == SIGHASH_SINGLE) {
        std::vector<uint8_t> output;
        WriteUint64LE(output, tx.vout[input_index].value.GetUna());
        WriteScript(output, tx.vout[input_index].scriptPubKey);
        std::vector<uint8_t> sha_output = SHA256_Single(output);
        data.insert(data.end(), sha_output.begin(), sha_output.end());
    }

    // 14. Script path data (if spending via script path)
    if (!leaf_hash.empty()) {
        // tapleaf_hash (32 bytes)
        data.insert(data.end(), leaf_hash.begin(), leaf_hash.end());
        // key_version (1 byte) - always 0 for BIP 342
        data.push_back(0);
        // codesep_pos (4 bytes) - position of last executed OP_CODESEPARATOR (-1 if none)
        WriteUint32LE(data, 0xFFFFFFFF);  // -1 = no OP_CODESEPARATOR
    }

    // 15. Dinero CT extension: bind confidential prevout commitments when present.
    const std::vector<uint8_t> ct_extension = ComputeConfidentialPrevoutExtension(ctx, anyonecanpay);
    if (!ct_extension.empty()) {
        data.insert(data.end(), ct_extension.begin(), ct_extension.end());
        return TaggedHash("dinero/sighash/v1", data);
    }

    // 16. Final hash using the standard TapSighash tag for transparent prevouts.
    return TaggedHash("TapSighash", data);
}

// ============================================================================
// BIP 340/341 Tagged Hash Functions (Exported)
// ============================================================================

std::vector<uint8_t> TaggedHash(const std::string& tag, const std::vector<uint8_t>& data) {
    return dinero::crypto::TaggedHash(tag, data);
}

std::vector<uint8_t> TapLeafHash(uint8_t leaf_version, const std::vector<uint8_t>& script) {
    // TapLeaf = TaggedHash("TapLeaf", leaf_version || compact_size(script) || script)
    std::vector<uint8_t> data;
    data.reserve(1 + 9 + script.size());  // max compact size is 9 bytes

    // Leaf version (1 byte)
    data.push_back(leaf_version);

    // Script with compact size prefix
    WriteScript(data, script);

    return TaggedHash("TapLeaf", data);
}

std::vector<uint8_t> TapBranchHash(const std::vector<uint8_t>& left, const std::vector<uint8_t>& right) {
    // TapBranch = TaggedHash("TapBranch", sorted(left, right))
    // The two hashes are sorted lexicographically before hashing
    std::vector<uint8_t> data;
    data.reserve(64);

    if (left < right) {
        data.insert(data.end(), left.begin(), left.end());
        data.insert(data.end(), right.begin(), right.end());
    } else {
        data.insert(data.end(), right.begin(), right.end());
        data.insert(data.end(), left.begin(), left.end());
    }

    return TaggedHash("TapBranch", data);
}

std::vector<uint8_t> TapTweakHash(const std::vector<uint8_t>& pubkey, const std::vector<uint8_t>& merkle_root) {
    // TapTweak = TaggedHash("TapTweak", pubkey || merkle_root)
    // If merkle_root is empty, it's a key-path-only spend
    std::vector<uint8_t> data;
    data.reserve(pubkey.size() + merkle_root.size());

    data.insert(data.end(), pubkey.begin(), pubkey.end());
    if (!merkle_root.empty()) {
        data.insert(data.end(), merkle_root.begin(), merkle_root.end());
    }

    return TaggedHash("TapTweak", data);
}

} // namespace consensus
} // namespace dinero
