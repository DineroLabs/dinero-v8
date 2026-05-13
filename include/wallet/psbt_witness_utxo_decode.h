#pragma once

#include <cstdint>
#include <vector>

namespace din {
namespace psbt {

struct WitnessUtxoDecodeResult {
    bool ok = false;
    bool used_legacy_fallback = false;
    uint64_t amount = 0;
    std::vector<uint8_t> script_pubkey;
};

inline bool DecodeCompactSize(const uint8_t*& p, const uint8_t* e, uint64_t& v) {
    if (p >= e) return false;
    const uint8_t ch = *p++;
    if (ch < 0xFD) {
        v = ch;
        return true;
    }
    if (ch == 0xFD) {
        if (e - p < 2) return false;
        v = static_cast<uint64_t>(p[0]) |
            (static_cast<uint64_t>(p[1]) << 8);
        p += 2;
        return true;
    }
    if (ch == 0xFE) {
        if (e - p < 4) return false;
        v = static_cast<uint64_t>(p[0]) |
            (static_cast<uint64_t>(p[1]) << 8) |
            (static_cast<uint64_t>(p[2]) << 16) |
            (static_cast<uint64_t>(p[3]) << 24);
        p += 4;
        return true;
    }
    if (e - p < 8) return false;
    v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(p[i]) << (8 * i);
    }
    p += 8;
    return true;
}

inline WitnessUtxoDecodeResult DecodeWitnessUtxoValue(const std::vector<uint8_t>& value) {
    WitnessUtxoDecodeResult out;
    if (value.size() < 9) return out;

    uint64_t amount = 0;
    for (int i = 0; i < 8; ++i) {
        amount |= static_cast<uint64_t>(value[i]) << (8 * i);
    }

    const uint8_t* p = value.data() + 8;
    const uint8_t* e = value.data() + value.size();
    uint64_t script_len = 0;

    if (DecodeCompactSize(p, e, script_len)) {
        // Strict canonical parse: exact full consumption only.
        const uint64_t remaining = static_cast<uint64_t>(e - p);
        if (script_len != remaining) {
            return out;
        }
        out.script_pubkey.assign(p, p + script_len);
        if (out.script_pubkey.empty()) {
            return WitnessUtxoDecodeResult{};
        }
        out.ok = true;
        out.amount = amount;
        out.used_legacy_fallback = false;
        return out;
    }

    // Legacy fallback is only for CompactSize decode failures.
    out.script_pubkey.assign(value.begin() + 8, value.end());
    if (out.script_pubkey.empty()) {
        return WitnessUtxoDecodeResult{};
    }
    out.ok = true;
    out.amount = amount;
    out.used_legacy_fallback = true;
    return out;
}

}  // namespace psbt
}  // namespace din

