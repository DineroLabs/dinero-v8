/**
 * Canonical v5 shielded bundle serialization.
 * See include/consensus/shielded/shielded_serialization.h.
 */

#include "consensus/shielded/shielded_serialization.h"

#include <algorithm>
#include <cstring>

namespace dinero::consensus::shielded {

namespace {

// CompactSize (Bitcoin-standard minimal varint encoding).
void WriteCompactSize(std::vector<uint8_t>& out, uint64_t v) {
    if (v < 253) {
        out.push_back(static_cast<uint8_t>(v));
    } else if (v <= 0xFFFF) {
        out.push_back(0xFD);
        out.push_back(static_cast<uint8_t>(v & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    } else if (v <= 0xFFFFFFFF) {
        out.push_back(0xFE);
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((v >> (8*i)) & 0xFF));
    } else {
        out.push_back(0xFF);
        for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>((v >> (8*i)) & 0xFF));
    }
}

bool ReadCompactSize(const uint8_t*& p, const uint8_t* end, uint64_t& out) {
    if (p >= end) return false;
    uint8_t first = *p++;
    if (first < 253) {
        out = first;
    } else if (first == 0xFD) {
        if (p + 2 > end) return false;
        out = static_cast<uint64_t>(p[0]) | (static_cast<uint64_t>(p[1]) << 8);
        if (out < 253) return false;  // non-minimal
        p += 2;
    } else if (first == 0xFE) {
        if (p + 4 > end) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) out |= static_cast<uint64_t>(p[i]) << (8*i);
        if (out <= 0xFFFF) return false;  // non-minimal
        p += 4;
    } else {
        if (p + 8 > end) return false;
        out = 0;
        for (int i = 0; i < 8; ++i) out |= static_cast<uint64_t>(p[i]) << (8*i);
        if (out <= 0xFFFFFFFF) return false;  // non-minimal
        p += 8;
    }
    return true;
}

void WriteLE64(std::vector<uint8_t>& out, int64_t v) {
    uint64_t u;
    std::memcpy(&u, &v, 8);
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>((u >> (8*i)) & 0xFF));
}

bool ReadLE64(const uint8_t*& p, const uint8_t* end, int64_t& out) {
    if (p + 8 > end) return false;
    uint64_t u = 0;
    for (int i = 0; i < 8; ++i) u |= static_cast<uint64_t>(p[i]) << (8*i);
    std::memcpy(&out, &u, 8);
    p += 8;
    return true;
}

void WriteHash(std::vector<uint8_t>& out, const Hash& h) {
    out.insert(out.end(), h.begin(), h.end());
}

bool ReadHash(const uint8_t*& p, const uint8_t* end, Hash& out) {
    if (p + HASH_BYTES > end) return false;
    std::memcpy(out.data(), p, HASH_BYTES);
    p += HASH_BYTES;
    return true;
}

// 64-byte binding signature serializer (Phase 3 wave 2).
void WriteSig64(std::vector<uint8_t>& out, const BindingSignature& sig) {
    out.insert(out.end(), sig.begin(), sig.end());
}

bool ReadSig64(const uint8_t*& p, const uint8_t* end, BindingSignature& out) {
    if (p + out.size() > end) return false;
    std::memcpy(out.data(), p, out.size());
    p += out.size();
    return true;
}

// 33-byte Pedersen value-commitment serializer (Phase 3 wave 2).
void WriteCv(std::vector<uint8_t>& out, const ValueCommitment& cv) {
    out.insert(out.end(), cv.begin(), cv.end());
}

bool ReadCv(const uint8_t*& p, const uint8_t* end, ValueCommitment& out) {
    if (p + out.size() > end) return false;
    std::memcpy(out.data(), p, out.size());
    p += out.size();
    return true;
}

void WriteVec(std::vector<uint8_t>& out, const std::vector<uint8_t>& v) {
    WriteCompactSize(out, v.size());
    out.insert(out.end(), v.begin(), v.end());
}

bool ReadVec(const uint8_t*& p, const uint8_t* end, std::vector<uint8_t>& out) {
    uint64_t len = 0;
    if (!ReadCompactSize(p, end, len)) return false;
    if (p + len > end) return false;
    out.assign(p, p + len);
    p += len;
    return true;
}

// Canonical ordering comparators.
bool SpendLess(const ShieldedSpend& a, const ShieldedSpend& b) {
    return a.nullifier < b.nullifier;
}
bool OutputLess(const ShieldedOutput& a, const ShieldedOutput& b) {
    return a.commitment < b.commitment;
}

} // namespace

std::vector<uint8_t> SerializeShieldedBundle(const ShieldedBundle& bundle) {
    // Sort copies for canonical ordering.
    auto spends = bundle.spends;
    auto outputs = bundle.outputs;
    std::sort(spends.begin(), spends.end(), SpendLess);
    std::sort(outputs.begin(), outputs.end(), OutputLess);

    std::vector<uint8_t> out;
    out.reserve(256);

    WriteLE64(out, bundle.value_balance);

    WriteCompactSize(out, spends.size());
    for (const auto& s : spends) {
        WriteHash(out, s.nullifier);
        WriteHash(out, s.anchor);
        WriteCv(out, s.cv);              // Phase 3 wave 2 (33 bytes)
        WriteVec(out, s.zk_proof);
    }

    WriteCompactSize(out, outputs.size());
    for (const auto& o : outputs) {
        WriteHash(out, o.commitment);
        WriteCv(out, o.cv);              // Phase 3 wave 2 (33 bytes)
        WriteVec(out, o.encrypted_note);
        WriteVec(out, o.zk_proof);
    }

    WriteVec(out, bundle.aggregated_range_proof);  // Phase 3 wave 1
    WriteCv(out, bundle.bvk_commitment);            // Phase 3 wave 2
    WriteSig64(out, bundle.binding_sig);            // Phase 3 wave 2 (64 bytes)

    return out;
}

BundleDecodeError DeserializeShieldedBundle(const uint8_t* data,
                                            size_t len,
                                            ShieldedBundle* out) {
    const uint8_t* p = data;
    const uint8_t* end = data + len;

    if (!ReadLE64(p, end, out->value_balance)) return BundleDecodeError::Truncated;

    uint64_t num_spends = 0;
    if (!ReadCompactSize(p, end, num_spends)) return BundleDecodeError::Truncated;
    out->spends.resize(static_cast<size_t>(num_spends));
    for (size_t i = 0; i < num_spends; ++i) {
        auto& s = out->spends[i];
        if (!ReadHash(p, end, s.nullifier)) return BundleDecodeError::Truncated;
        if (!ReadHash(p, end, s.anchor))    return BundleDecodeError::Truncated;
        if (!ReadCv(p, end, s.cv))          return BundleDecodeError::Truncated;
        if (!ReadVec(p, end, s.zk_proof))   return BundleDecodeError::Truncated;
    }

    uint64_t num_outputs = 0;
    if (!ReadCompactSize(p, end, num_outputs)) return BundleDecodeError::Truncated;
    out->outputs.resize(static_cast<size_t>(num_outputs));
    for (size_t i = 0; i < num_outputs; ++i) {
        auto& o = out->outputs[i];
        if (!ReadHash(p, end, o.commitment))     return BundleDecodeError::Truncated;
        if (!ReadCv(p, end, o.cv))               return BundleDecodeError::Truncated;
        if (!ReadVec(p, end, o.encrypted_note))  return BundleDecodeError::Truncated;
        if (!ReadVec(p, end, o.zk_proof))        return BundleDecodeError::Truncated;
    }

    if (!ReadVec(p, end, out->aggregated_range_proof)) return BundleDecodeError::Truncated;
    if (!ReadCv(p, end, out->bvk_commitment)) return BundleDecodeError::Truncated;
    if (!ReadSig64(p, end, out->binding_sig)) return BundleDecodeError::Truncated;

    if (p != end) return BundleDecodeError::TrailingBytes;

    // Canonical ordering check.
    for (size_t i = 1; i < out->spends.size(); ++i) {
        if (!(out->spends[i-1].nullifier < out->spends[i].nullifier)) {
            return BundleDecodeError::OrderViolation;
        }
    }
    for (size_t i = 1; i < out->outputs.size(); ++i) {
        if (!(out->outputs[i-1].commitment < out->outputs[i].commitment)) {
            return BundleDecodeError::OrderViolation;
        }
    }

    // Round-trip check: re-serialize must produce identical bytes.
    auto reserialized = SerializeShieldedBundle(*out);
    if (reserialized.size() != len ||
        std::memcmp(reserialized.data(), data, len) != 0) {
        return BundleDecodeError::NotCanonical;
    }

    return BundleDecodeError::Ok;
}

} // namespace dinero::consensus::shielded
