// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "p2p/relay_hint_v2.h"

#include "daemon/node_identity.h"
#include "dinero/core/crypto/dinero_crypto_minimal.h"

#include <cstring>

namespace dinero::p2p {

namespace {

constexpr char kHintDomain[] = "dinero-relay-hint-v2";
constexpr size_t kFixedHead = 20 + 33 + 1 + 1;  // node_id, pubkey, net, addr_len

std::vector<uint8_t> SigningBytes(const HintFields& h) {
    std::vector<uint8_t> m(kHintDomain, kHintDomain + sizeof(kHintDomain) - 1);
    m.insert(m.end(), h.node_id.begin(), h.node_id.end());
    m.push_back(static_cast<uint8_t>(h.relay_net));
    m.push_back(static_cast<uint8_t>(h.relay_addr.size()));
    m.insert(m.end(), h.relay_addr.begin(), h.relay_addr.end());
    m.push_back(static_cast<uint8_t>(h.relay_port >> 8));
    m.push_back(static_cast<uint8_t>(h.relay_port & 0xFF));
    for (int i = 0; i < 4; ++i) m.push_back(static_cast<uint8_t>(h.expiry >> (i * 8)));
    return m;
}

// Offset of the hop byte, or 0 if the payload is too short to have one.
size_t HopOffset(std::span<const uint8_t> p) {
    if (p.size() < kFixedHead) return 0;
    const size_t off = kFixedHead + p[20 + 33 + 1] + 2 + 4;
    return off < p.size() ? off : 0;
}

}  // namespace

std::vector<uint8_t> SignHint(const dinero::daemon::NodeIdentity& identity,
                              const HintFields& fields) {
    HintFields h = fields;
    h.node_id = identity.get_node_id_bytes();
    h.pubkey = identity.get_pubkey_bytes();
    size_t expected = 0;
    if (!NetworkTypeExpectedLength(h.relay_net, &expected) || h.relay_addr.size() != expected) {
        return {};
    }
    const auto msg = SigningBytes(h);
    const auto sig = identity.sign_bytes(msg.data(), msg.size());
    if (sig.empty() || sig.size() > 72) return {};
    std::vector<uint8_t> out;
    out.insert(out.end(), h.node_id.begin(), h.node_id.end());
    out.insert(out.end(), h.pubkey.begin(), h.pubkey.end());
    out.push_back(static_cast<uint8_t>(h.relay_net));
    out.push_back(static_cast<uint8_t>(h.relay_addr.size()));
    out.insert(out.end(), h.relay_addr.begin(), h.relay_addr.end());
    out.push_back(static_cast<uint8_t>(h.relay_port >> 8));
    out.push_back(static_cast<uint8_t>(h.relay_port & 0xFF));
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(h.expiry >> (i * 8)));
    out.push_back(h.hop);
    out.push_back(static_cast<uint8_t>(sig.size()));
    out.insert(out.end(), sig.begin(), sig.end());
    return out;
}

bool IsPublicRelayAddress(NetworkType net, const std::vector<uint8_t>& a) {
    if (net == NetworkType::IPV4 && a.size() == 4) {
        if (a[0] == 0 || a[0] == 10 || a[0] == 127 || a[0] >= 224) return false;
        if (a[0] == 169 && a[1] == 254) return false;
        if (a[0] == 172 && (a[1] & 0xF0) == 16) return false;
        if (a[0] == 192 && a[1] == 168) return false;
        if (a[0] == 100 && (a[1] & 0xC0) == 64) return false;  // CGNAT 100.64/10
        return true;
    }
    if (net == NetworkType::IPV6 && a.size() == 16) {
        bool all_zero = true;
        for (size_t i = 0; i < 15; ++i) all_zero = all_zero && a[i] == 0;
        if (all_zero && (a[15] == 0 || a[15] == 1)) return false;  // :: and ::1
        if ((a[0] & 0xFE) == 0xFC) return false;                   // fc00::/7
        if (a[0] == 0xFE && (a[1] & 0xC0) == 0x80) return false;   // fe80::/10
        if (a[0] == 0xFF) return false;                            // multicast
        bool mapped = true;                                        // ::ffff:0:0/96
        for (size_t i = 0; i < 10; ++i) mapped = mapped && a[i] == 0;
        if (mapped && a[10] == 0xFF && a[11] == 0xFF) return false;
        return true;
    }
    return false;
}

std::optional<HintFields> VerifyHint(std::span<const uint8_t> p, uint32_t now,
                                     bool allow_private_relay) {
    if (p.size() < kFixedHead) return std::nullopt;
    HintFields h;
    std::memcpy(h.node_id.data(), p.data(), 20);
    std::memcpy(h.pubkey.data(), p.data() + 20, 33);
    h.relay_net = static_cast<NetworkType>(p[53]);
    const size_t addr_len = p[54];
    size_t expected = 0;
    if (!NetworkTypeExpectedLength(h.relay_net, &expected) || addr_len != expected) {
        return std::nullopt;
    }
    size_t off = kFixedHead;
    if (p.size() < off + addr_len + 2 + 4 + 1 + 1) return std::nullopt;
    h.relay_addr.assign(p.begin() + off, p.begin() + off + addr_len);
    off += addr_len;
    h.relay_port = static_cast<uint16_t>((p[off] << 8) | p[off + 1]);
    off += 2;
    for (int i = 0; i < 4; ++i) h.expiry |= static_cast<uint32_t>(p[off + i]) << (i * 8);
    off += 4;
    h.hop = p[off++];
    const size_t sig_len = p[off++];
    if (sig_len == 0 || sig_len > 72 || p.size() != off + sig_len) return std::nullopt;
    if (h.relay_port == 0) return std::nullopt;
    if (!allow_private_relay && !IsPublicRelayAddress(h.relay_net, h.relay_addr)) return std::nullopt;
    if (h.expiry <= now || h.expiry > now + kHintMaxLifetimeSeconds) return std::nullopt;

    // The key must be the one the node id names, and it must have signed.
    std::array<uint8_t, 20> derived{};
    ::HASH160(h.pubkey.data(), 33, derived.data());
    if (derived != h.node_id) return std::nullopt;
    const auto msg = SigningBytes(h);
    if (!dinero::daemon::NodeIdentity::verify_bytes(msg.data(), msg.size(), p.data() + off,
                                                    sig_len, h.pubkey.data())) {
        return std::nullopt;
    }
    return h;
}

std::vector<uint8_t> WithHop(std::span<const uint8_t> payload, uint8_t hop) {
    std::vector<uint8_t> out(payload.begin(), payload.end());
    const size_t off = HopOffset(payload);
    if (off) out[off] = hop;
    return out;
}

HintForwarder::HintForwarder(double per_minute) : bucket_(per_minute / 60.0, per_minute) {}

bool HintForwarder::ShouldForward(const HintFields& hint, uint32_t now_unix,
                                  std::chrono::steady_clock::time_point now) {
    if (now - last_prune_ >= std::chrono::minutes(1)) {
        for (auto it = seen_.begin(); it != seen_.end();) {
            it = it->second <= now_unix ? seen_.erase(it) : std::next(it);
        }
        last_prune_ = now;
    }
    if (hint.hop >= kHintMaxHops) return false;
    const Key key{hint.node_id, static_cast<uint8_t>(hint.relay_net), hint.relay_addr,
                  hint.relay_port, hint.expiry};
    if (seen_.count(key)) return false;  // already forwarded
    // Record only what we actually forward: a hint refused for budget may
    // go out later, when the bucket has refilled.
    if (!bucket_.TryConsume(1, now)) return false;
    seen_.emplace(key, hint.expiry);
    return true;
}

}  // namespace dinero::p2p
