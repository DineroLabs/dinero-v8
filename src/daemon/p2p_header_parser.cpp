#include "daemon/p2p_header_parser.h"
#include "common/logger.h"

namespace dinero::daemon {
std::vector<BlockHeader> ParseHeadersPayload(const std::vector<uint8_t>& payload) {
    std::vector<BlockHeader> headers;

    // Bitcoin wire format: varint(count) + (header_bytes + varint(tx_count))*N
    // Dinero headers are 128 bytes (not Bitcoin's 80 bytes)
    // tx_count is always 0 for headers message

    if (payload.size() < 1) {
        g_logger.warning("[ParseHeaders] Empty headers payload");
        return headers;
    }

    size_t offset = 0;

    // Read varint for header count
    auto read_varint = [&]() -> uint64_t {
        if (offset >= payload.size()) return 0;
        uint8_t first = payload[offset++];
        if (first < 0xFD) {
            return first;
        } else if (first == 0xFD) {
            if (offset + 2 > payload.size()) return 0;
            uint64_t val = payload[offset] | (static_cast<uint64_t>(payload[offset + 1]) << 8);
            offset += 2;
            return val;
        } else if (first == 0xFE) {
            if (offset + 4 > payload.size()) return 0;
            uint64_t val = 0;
            for (int i = 0; i < 4; i++) val |= static_cast<uint64_t>(payload[offset + i]) << (i * 8);
            offset += 4;
            return val;
        } else {
            if (offset + 8 > payload.size()) return 0;
            uint64_t val = 0;
            for (int i = 0; i < 8; i++) val |= static_cast<uint64_t>(payload[offset + i]) << (i * 8);
            offset += 8;
            return val;
        }
    };

    uint64_t count = read_varint();
    if (count == 0) {
        // Zero headers is a valid "nothing new" response (peer at same tip).
        return headers;
    }
    if (count > 2000) {
        g_logger.warning("[ParseHeaders] Invalid header count: " + std::to_string(count));
        return headers;
    }

    // Parse each header (128 bytes) + tx_count varint (should be 0)
    for (uint64_t i = 0; i < count; i++) {
        // Check we have enough bytes for 128-byte header
        if (offset + 128 > payload.size()) {
            g_logger.warning("[ParseHeaders] Header too short: " + std::to_string(payload.size() - offset) +
                           " bytes remaining (expected 128)");
            break;
        }

        // Network framing places headers at arbitrary byte offsets. The
        // canonical byte-wise decoder is alignment-safe and explicitly LE.
        const auto header = BlockHeader::Deserialize(payload.data() + offset, 128);
        if (!header) break; // The complete-header bound was checked above.

        offset += 128;  // Move past header

        // Read tx_count varint (should be 0)
        read_varint();

        headers.push_back(*header);
    }

    g_logger.info("[ParseHeaders] Parsed " + std::to_string(headers.size()) + " headers from P2P message");
    return headers;
}


} // namespace dinero::daemon
