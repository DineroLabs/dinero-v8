#include <gtest/gtest.h>

#include "daemon/block_relay_manager.h"
#include "daemon/header_serialization.h"
#include "daemon/p2p_message.h"
#include "primitives/uint256.h"
#include "util/ser.h"

namespace dinero {

TEST(HeadersWireV1, BlockRelayManagerSerializeHeaders_Uses128ByteHeader) {
    BlockHeader h{};
    h.version = 1;
    h.prev_block_hash = uint256::FromHexUnsafe("11" + std::string(62, '0'));
    h.merkle_root = uint256::FromHexUnsafe("22" + std::string(62, '0'));
    h.utreexo_root = uint256::FromHexUnsafe("33" + std::string(62, '0'));
    h.timestamp = 0x1122334455667788ULL;  // ensure 64-bit timestamp survives wire serialization
    h.difficulty = 0x207fffff;            // regtest-style pow limit bits
    h.nonce = 0x99aabbcc;
    h.ZeroReserved();

    BlockRelayManager mgr(/*logger=*/nullptr, /*scheduler=*/nullptr);
    const std::vector<uint8_t> payload = mgr.SerializeHeaders({h});

    // 1 (count varint) + 128 (header) + 1 (txcount=0 varint)
    ASSERT_EQ(payload.size(), 130u);

    HeadersMessage msg;
    ASSERT_TRUE(msg.deserialize(payload));
    ASSERT_EQ(msg.headers.size(), 1u);
    ASSERT_EQ(msg.headers[0].size(), 128u);

    const std::vector<uint8_t> expected_header = serializeHeaderForWire(h);
    ASSERT_EQ(msg.headers[0], expected_header);

    // Ensure the full payload matches the expected wire format exactly.
    std::vector<uint8_t> expected_payload;
    ser::writeCompactSize(1, expected_payload);
    expected_payload.insert(expected_payload.end(), expected_header.begin(), expected_header.end());
    expected_payload.push_back(0);
    ASSERT_EQ(payload, expected_payload);
}

} // namespace dinero

