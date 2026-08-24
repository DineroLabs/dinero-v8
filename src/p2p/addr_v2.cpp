// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "p2p/addr_v2.h"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cctype>

namespace dinero::p2p {

namespace {

constexpr char kBase32Alphabet[] = "abcdefghijklmnopqrstuvwxyz234567";
constexpr uint8_t kTorV3Version = 3;

std::string Base32Encode(const std::vector<uint8_t>& input) {
    std::string out;
    uint32_t accumulator = 0;
    int bits = 0;
    for (uint8_t byte : input) {
        accumulator = (accumulator << 8) | byte;
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            out.push_back(kBase32Alphabet[(accumulator >> bits) & 31]);
        }
    }
    if (bits > 0) out.push_back(kBase32Alphabet[(accumulator << (5 - bits)) & 31]);
    return out;
}

bool Base32Decode(const std::string& input, std::vector<uint8_t>* out) {
    if (!out) return false;
    std::vector<uint8_t> decoded;
    uint32_t accumulator = 0;
    int bits = 0;
    for (unsigned char raw : input) {
        const char c = static_cast<char>(std::tolower(raw));
        const char* found = std::find(std::begin(kBase32Alphabet),
                                      std::end(kBase32Alphabet) - 1, c);
        if (found == std::end(kBase32Alphabet) - 1) return false;
        accumulator = (accumulator << 5) |
            static_cast<uint32_t>(found - std::begin(kBase32Alphabet));
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            decoded.push_back(static_cast<uint8_t>((accumulator >> bits) & 0xff));
        }
    }
    *out = std::move(decoded);
    return true;
}

bool OnionChecksum(const std::vector<uint8_t>& public_key,
                   std::array<uint8_t, 2>* checksum) {
    if (!checksum || public_key.size() != 32) return false;
    static constexpr char kPrefix[] = ".onion checksum";
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return false;
    std::array<uint8_t, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0;
    const bool ok = EVP_DigestInit_ex(ctx, EVP_sha3_256(), nullptr) == 1 &&
        EVP_DigestUpdate(ctx, kPrefix, sizeof(kPrefix) - 1) == 1 &&
        EVP_DigestUpdate(ctx, public_key.data(), public_key.size()) == 1 &&
        EVP_DigestUpdate(ctx, &kTorV3Version, 1) == 1 &&
        EVP_DigestFinal_ex(ctx, digest.data(), &digest_size) == 1 &&
        digest_size >= 2;
    EVP_MD_CTX_free(ctx);
    if (!ok) return false;
    (*checksum)[0] = digest[0];
    (*checksum)[1] = digest[1];
    return true;
}

// Bitcoin-style CompactSize varint.
void WriteCompactSize(std::vector<uint8_t>* out, uint64_t v) {
    if (v < 253) {
        out->push_back(static_cast<uint8_t>(v));
    } else if (v <= 0xFFFFu) {
        out->push_back(253);
        out->push_back(static_cast<uint8_t>(v & 0xFF));
        out->push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    } else if (v <= 0xFFFFFFFFu) {
        out->push_back(254);
        for (int i = 0; i < 4; i++) {
            out->push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
        }
    } else {
        out->push_back(255);
        for (int i = 0; i < 8; i++) {
            out->push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
        }
    }
}

bool ReadCompactSize(const std::vector<uint8_t>& buf, size_t* offset,
                     uint64_t* v) {
    if (*offset >= buf.size()) return false;
    uint8_t marker = buf[(*offset)++];
    if (marker < 253) {
        *v = marker;
        return true;
    }
    auto read_le = [&](size_t bytes) -> bool {
        if (*offset + bytes > buf.size()) return false;
        uint64_t r = 0;
        for (size_t i = 0; i < bytes; i++) {
            r |= static_cast<uint64_t>(buf[*offset + i]) << (i * 8);
        }
        *v = r;
        *offset += bytes;
        return true;
    };
    switch (marker) {
        case 253: return read_le(2);
        case 254: return read_le(4);
        case 255: return read_le(8);
    }
    return false;  // unreachable
}

void WriteLE32(std::vector<uint8_t>* out, uint32_t v) {
    for (int i = 0; i < 4; i++) {
        out->push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
    }
}

bool ReadLE32(const std::vector<uint8_t>& buf, size_t* offset, uint32_t* v) {
    if (*offset + 4 > buf.size()) return false;
    *v = static_cast<uint32_t>(buf[*offset]) |
         (static_cast<uint32_t>(buf[*offset + 1]) << 8) |
         (static_cast<uint32_t>(buf[*offset + 2]) << 16) |
         (static_cast<uint32_t>(buf[*offset + 3]) << 24);
    *offset += 4;
    return true;
}

// BIP155 ports are big-endian (network byte order), unlike Dinero's
// legacy LE port in the v1 `addr` message.
void WriteBE16(std::vector<uint8_t>* out, uint16_t v) {
    out->push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out->push_back(static_cast<uint8_t>(v & 0xFF));
}

bool ReadBE16(const std::vector<uint8_t>& buf, size_t* offset, uint16_t* v) {
    if (*offset + 2 > buf.size()) return false;
    *v = (static_cast<uint16_t>(buf[*offset]) << 8) |
         static_cast<uint16_t>(buf[*offset + 1]);
    *offset += 2;
    return true;
}

}  // namespace

bool NetworkTypeExpectedLength(NetworkType net, size_t* out) {
    switch (net) {
        case NetworkType::IPV4:  *out = 4;  return true;
        case NetworkType::IPV6:  *out = 16; return true;
        case NetworkType::TORV3: *out = 32; return true;
        case NetworkType::I2P:   *out = 32; return true;
        case NetworkType::Unknown:
        default:
            return false;
    }
}

std::vector<uint8_t> EncodeAddrV2(const std::vector<AddrV2Entry>& entries) {
    // First filter to validated entries so the on-wire count matches the
    // body. Drop anything with the wrong-sized addr blob — decoder enforces
    // the same invariant, so emitting an invalid entry would only confuse
    // the remote.
    std::vector<const AddrV2Entry*> valid;
    valid.reserve(entries.size());
    for (const auto& e : entries) {
        size_t expected = 0;
        if (!NetworkTypeExpectedLength(e.net, &expected)) continue;
        if (e.addr.size() != expected) continue;
        valid.push_back(&e);
    }

    std::vector<uint8_t> out;
    out.reserve(1 + valid.size() * 40);  // rough preallocation

    WriteCompactSize(&out, valid.size());
    for (const auto* e : valid) {
        WriteLE32(&out, e->time);
        WriteCompactSize(&out, e->services);
        out.push_back(static_cast<uint8_t>(e->net));
        WriteCompactSize(&out, e->addr.size());
        out.insert(out.end(), e->addr.begin(), e->addr.end());
        WriteBE16(&out, e->port);
    }
    return out;
}

bool DecodeAddrV2(const std::vector<uint8_t>& payload,
                  std::vector<AddrV2Entry>* out,
                  std::string* err) {
    if (!out) {
        if (err) *err = "null output vector";
        return false;
    }
    out->clear();

    size_t offset = 0;
    uint64_t count = 0;
    if (!ReadCompactSize(payload, &offset, &count)) {
        if (err) *err = "truncated count varint";
        return false;
    }
    // BIP155 caps addrv2 messages at 1000 entries.
    if (count > 1000) {
        if (err) *err = "addrv2 entry count exceeds 1000";
        return false;
    }

    for (uint64_t i = 0; i < count; i++) {
        AddrV2Entry e;
        if (!ReadLE32(payload, &offset, &e.time)) {
            if (err) *err = "truncated entry time";
            return false;
        }
        uint64_t services = 0;
        if (!ReadCompactSize(payload, &offset, &services)) {
            if (err) *err = "truncated entry services";
            return false;
        }
        e.services = services;

        if (offset >= payload.size()) {
            if (err) *err = "truncated network-id byte";
            return false;
        }
        uint8_t net_byte = payload[offset++];

        uint64_t addr_len = 0;
        if (!ReadCompactSize(payload, &offset, &addr_len)) {
            if (err) *err = "truncated addr_len varint";
            return false;
        }
        // BIP155 caps individual address blobs at 512 bytes.
        if (addr_len > 512) {
            if (err) *err = "entry addr_len exceeds 512";
            return false;
        }
        if (offset + addr_len + 2 > payload.size()) {
            if (err) *err = "truncated addr bytes or port";
            return false;
        }

        e.addr.assign(payload.begin() + offset,
                      payload.begin() + offset + addr_len);
        offset += static_cast<size_t>(addr_len);

        if (!ReadBE16(payload, &offset, &e.port)) {
            if (err) *err = "truncated entry port";
            return false;
        }

        // Categorize. Unknown network ids advance the cursor but DON'T
        // make it into `out` — caller would have no way to use them
        // anyway, and ignoring keeps the network forward-compatible
        // when future BIP155 types appear on the wire.
        NetworkType net = NetworkType::Unknown;
        switch (net_byte) {
            case static_cast<uint8_t>(NetworkType::IPV4):  net = NetworkType::IPV4;  break;
            case static_cast<uint8_t>(NetworkType::IPV6):  net = NetworkType::IPV6;  break;
            case static_cast<uint8_t>(NetworkType::TORV3): net = NetworkType::TORV3; break;
            case static_cast<uint8_t>(NetworkType::I2P):   net = NetworkType::I2P;   break;
            default: break;  // skipped
        }
        if (net == NetworkType::Unknown) continue;

        // Validate addr-length matches the per-network expectation.
        size_t expected = 0;
        if (!NetworkTypeExpectedLength(net, &expected) || expected != e.addr.size()) {
            // Length mismatch: skip the entry but DON'T fail the whole
            // payload — a single bad entry shouldn't drop the rest.
            continue;
        }

        e.net = net;
        out->push_back(std::move(e));
    }

    return true;
}

bool DecodeTorV3Address(const std::string& onion,
                        std::vector<uint8_t>* public_key,
                        std::string* err) {
    if (!public_key) {
        if (err) *err = "null Tor v3 public-key output";
        return false;
    }
    std::string host = onion;
    std::transform(host.begin(), host.end(), host.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static constexpr char kSuffix[] = ".onion";
    if (host.size() != 56 + sizeof(kSuffix) - 1 ||
        host.compare(host.size() - (sizeof(kSuffix) - 1), sizeof(kSuffix) - 1,
                     kSuffix) != 0) {
        if (err) *err = "Tor v3 hostname must be 56 base32 characters plus .onion";
        return false;
    }
    std::vector<uint8_t> decoded;
    if (!Base32Decode(host.substr(0, 56), &decoded) || decoded.size() != 35 ||
        decoded[34] != kTorV3Version) {
        if (err) *err = "invalid Tor v3 base32 body or version";
        return false;
    }
    std::vector<uint8_t> key(decoded.begin(), decoded.begin() + 32);
    std::array<uint8_t, 2> checksum{};
    if (!OnionChecksum(key, &checksum) || decoded[32] != checksum[0] ||
        decoded[33] != checksum[1]) {
        if (err) *err = "Tor v3 checksum mismatch";
        return false;
    }
    *public_key = std::move(key);
    return true;
}

bool EncodeTorV3Address(const std::vector<uint8_t>& public_key,
                        std::string* onion,
                        std::string* err) {
    if (!onion || public_key.size() != 32) {
        if (err) *err = "Tor v3 public key must contain 32 bytes";
        return false;
    }
    std::array<uint8_t, 2> checksum{};
    if (!OnionChecksum(public_key, &checksum)) {
        if (err) *err = "Tor v3 checksum generation failed";
        return false;
    }
    std::vector<uint8_t> body = public_key;
    body.push_back(checksum[0]);
    body.push_back(checksum[1]);
    body.push_back(kTorV3Version);
    *onion = Base32Encode(body) + ".onion";
    return true;
}

}  // namespace dinero::p2p
