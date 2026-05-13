#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <array>
#include <optional>
// Phase 8.5: NO <chrono> include - wall time is FORBIDDEN

namespace dinero {
namespace lightning {

// BOLT 7: Gossip message types
enum class GossipMessageType : uint16_t {
    CHANNEL_ANNOUNCEMENT = 256,
    NODE_ANNOUNCEMENT = 257,
    CHANNEL_UPDATE = 258,
    ANNOUNCEMENT_SIGNATURES = 259,
    QUERY_SHORT_CHANNEL_IDS = 261,
    REPLY_SHORT_CHANNEL_IDS_END = 262,
    QUERY_CHANNEL_RANGE = 263,
    REPLY_CHANNEL_RANGE = 264,
    GOSSIP_TIMESTAMP_FILTER = 265
};

// Short channel ID: block_height (3 bytes) | tx_index (3 bytes) | output_index (2 bytes)
struct ShortChannelID {
    uint32_t block_height;
    uint32_t tx_index;
    uint16_t output_index;

    // Encode to 8-byte format
    uint64_t encode() const {
        return (static_cast<uint64_t>(block_height) << 40) |
               (static_cast<uint64_t>(tx_index) << 16) |
               output_index;
    }

    // Decode from 8-byte format
    static ShortChannelID decode(uint64_t encoded) {
        return ShortChannelID{
            static_cast<uint32_t>((encoded >> 40) & 0xFFFFFF),
            static_cast<uint32_t>((encoded >> 16) & 0xFFFFFF),
            static_cast<uint16_t>(encoded & 0xFFFF)
        };
    }

    std::string toString() const;
};

// Node public key (33-byte compressed secp256k1 pubkey)
using NodeID = std::array<uint8_t, 33>;

// Signature (64 bytes for Schnorr, 64-72 for ECDSA)
using Signature = std::vector<uint8_t>;

// Channel announcement (BOLT 7)
struct ChannelAnnouncement {
    // Signatures
    Signature node_signature_1;      // Node 1's signature
    Signature node_signature_2;      // Node 2's signature
    Signature dinero_signature_1;   // Funding key 1 signature (BOLT #7)
    Signature dinero_signature_2;   // Funding key 2 signature (BOLT #7)

    // Channel identification
    ShortChannelID short_channel_id;
    NodeID node_id_1;                // Lexicographically first
    NodeID node_id_2;                // Lexicographically second
    NodeID dinero_key_1;            // Funding pubkey 1 (BOLT #7)
    NodeID dinero_key_2;            // Funding pubkey 2 (BOLT #7)

    // Features (variable length)
    std::vector<uint8_t> features;

    // Chain hash (32 bytes - identifies blockchain)
    std::array<uint8_t, 32> chain_hash;

    // Validation
    bool verify_signatures() const;
    std::vector<uint8_t> serialize() const;
    static std::optional<ChannelAnnouncement> deserialize(const std::vector<uint8_t>& data);
};

// Node announcement (BOLT 7)
struct NodeAnnouncement {
    // Signature over the announcement
    Signature signature;

    // Node identification
    NodeID node_id;

    // Node information
    std::array<uint8_t, 3> rgb_color;     // RGB color for visualization
    std::string alias;                     // UTF-8, max 32 bytes
    std::vector<uint8_t> features;

    // Timestamp (for freshness)
    uint32_t timestamp;

    // Network addresses
    struct Address {
        enum Type : uint8_t {
            IPv4 = 1,
            IPv6 = 2,
            TORv2 = 3,
            TORv3 = 4
        };

        Type type;
        std::vector<uint8_t> addr;
        uint16_t port;
    };
    std::vector<Address> addresses;

    bool verify_signature() const;
    std::vector<uint8_t> serialize() const;
    static std::optional<NodeAnnouncement> deserialize(const std::vector<uint8_t>& data);
};

// Channel update (BOLT 7) - routing policy
struct ChannelUpdate {
    // Signature
    Signature signature;

    // Channel identification
    std::array<uint8_t, 32> chain_hash;
    ShortChannelID short_channel_id;

    // Timestamp
    uint32_t timestamp;

    // Flags
    struct Flags {
        bool direction;           // 0 = node_1 to node_2, 1 = node_2 to node_1
        bool disabled;            // Channel disabled
    };
    uint8_t message_flags;
    uint8_t channel_flags;

    // Routing policy
    uint16_t cltv_expiry_delta;   // CLTV blocks to add
    uint64_t htlc_minimum_muna;   // Minimum HTLC amount
    uint64_t htlc_maximum_muna;   // Maximum HTLC amount
    uint32_t fee_base_muna;       // Base fee
    uint32_t fee_proportional_millionths;  // Proportional fee (ppm)

    bool verify_signature() const;
    std::vector<uint8_t> serialize() const;
    static std::optional<ChannelUpdate> deserialize(const std::vector<uint8_t>& data);
};

// Query short channel IDs (BOLT 7)
struct QueryShortChannelIds {
    std::array<uint8_t, 32> chain_hash;
    std::vector<ShortChannelID> short_channel_ids;

    std::vector<uint8_t> serialize() const;
    static std::optional<QueryShortChannelIds> deserialize(const std::vector<uint8_t>& data);
};

// Query channel range (BOLT 7)
struct QueryChannelRange {
    std::array<uint8_t, 32> chain_hash;
    uint32_t first_blocknum;
    uint32_t number_of_blocks;

    std::vector<uint8_t> serialize() const;
    static std::optional<QueryChannelRange> deserialize(const std::vector<uint8_t>& data);
};

// Reply channel range (BOLT 7)
struct ReplyChannelRange {
    std::array<uint8_t, 32> chain_hash;
    uint32_t first_blocknum;
    uint32_t number_of_blocks;
    bool complete;
    std::vector<ShortChannelID> short_channel_ids;

    std::vector<uint8_t> serialize() const;
    static std::optional<ReplyChannelRange> deserialize(const std::vector<uint8_t>& data);
};

// Gossip timestamp filter (BOLT 7)
struct GossipTimestampFilter {
    std::array<uint8_t, 32> chain_hash;
    uint32_t first_timestamp;
    uint32_t timestamp_range;

    std::vector<uint8_t> serialize() const;
    static std::optional<GossipTimestampFilter> deserialize(const std::vector<uint8_t>& data);
};

// Network graph edge (directional channel)
struct ChannelEdge {
    ShortChannelID short_channel_id;
    NodeID source;
    NodeID target;

    // Routing policy (from ChannelUpdate)
    uint16_t cltv_expiry_delta;
    uint64_t htlc_minimum_muna;
    uint64_t htlc_maximum_muna;
    uint32_t fee_base_muna;
    uint32_t fee_proportional_millionths;

    // Channel capacity
    uint64_t capacity_sat;

    // Status
    bool disabled;
    uint32_t last_update;

    // Calculate routing fee for amount
    uint64_t calculate_fee(uint64_t amount_muna) const {
        return fee_base_muna +
               (amount_muna * fee_proportional_millionths) / 1000000;
    }
};

// Network graph node
struct GraphNode {
    NodeID node_id;
    std::string alias;
    std::array<uint8_t, 3> rgb_color;
    std::vector<NodeAnnouncement::Address> addresses;
    uint32_t last_update;

    // Connected channels
    std::vector<ShortChannelID> channels;
};

// Peer information
struct PeerInfo {
    NodeID node_id;
    std::string address;
    uint16_t port;
    bool connected;
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t last_ping;          // Phase 8.5: Unix timestamp (deterministic from block height)
    uint64_t connected_since;    // Phase 8.5: Unix timestamp (deterministic from block height)
};

} // namespace lightning
} // namespace dinero
