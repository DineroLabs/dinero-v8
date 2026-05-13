#pragma once

#include <cstdint>
#include <vector>
#include <memory>
#include <google/protobuf/message.h>

namespace dinero {

class HIDTransport;  // Forward declaration

/**
 * @brief Trezor Wire Protocol Transport
 *
 * Implements Trezor's wire protocol framing over HID transport:
 * - Message header: ## (0x23, 0x23)
 * - Message type: uint16_t (big endian)
 * - Payload length: uint32_t (big endian)
 * - Payload: protobuf-encoded message
 *
 * Simpler than Ledger APDU framing (no chunking, no channel/sequence tracking).
 */
class TrezorTransport {
public:
    TrezorTransport();
    ~TrezorTransport();

    /**
     * @brief Open connection to Trezor device
     * @param vendor_id USB vendor ID (0x534c or 0x1209 for Trezor)
     * @param product_id USB product ID (0 for any)
     * @return True if device opened successfully
     */
    bool open(uint16_t vendor_id, uint16_t product_id = 0);

    /**
     * @brief Open connection to a specific enumerated HID path
     * @param path Platform-specific HID path from HIDTransport::enumerate()
     * @return True if device opened successfully
     */
    bool openPath(const std::string& path);

    /**
     * @brief Close connection to device
     */
    void close();

    /**
     * @brief Check if device is open
     */
    bool isOpen() const;

    /**
     * @brief Send protobuf message to Trezor device
     * @param msg_type Trezor message type ID
     * @param message Protobuf message to send
     * @return True if sent successfully
     */
    bool sendMessage(uint16_t msg_type, const google::protobuf::Message& message);

    /**
     * @brief Receive protobuf message from Trezor device
     * @param msg_type Output: received message type
     * @param payload Output: received message payload (protobuf-encoded)
     * @return True if received successfully
     */
    bool recvMessage(uint16_t& msg_type, std::vector<uint8_t>& payload);

    /**
     * @brief Get last error message
     */
    std::string getLastError() const { return m_last_error; }

private:
    /**
     * @brief Build Trezor wire protocol frame
     * @param msg_type Message type ID
     * @param payload Message payload
     * @return Framed message ready for HID transport
     */
    std::vector<uint8_t> buildFrame(uint16_t msg_type, const std::vector<uint8_t>& payload);

    /**
     * @brief Parse Trezor wire protocol frame
     * @param frame Received frame data
     * @param msg_type Output: message type
     * @param payload Output: message payload
     * @return True if frame parsed successfully
     */
    bool parseFrame(const std::vector<uint8_t>& frame, uint16_t& msg_type, std::vector<uint8_t>& payload);

    /**
     * @brief Convert uint16_t to big-endian bytes
     */
    static void uint16ToBE(uint16_t value, std::vector<uint8_t>& buffer);

    /**
     * @brief Convert uint32_t to big-endian bytes
     */
    static void uint32ToBE(uint32_t value, std::vector<uint8_t>& buffer);

    /**
     * @brief Convert big-endian bytes to uint16_t
     */
    static uint16_t BEToUint16(const uint8_t* data);

    /**
     * @brief Convert big-endian bytes to uint32_t
     */
    static uint32_t BEToUint32(const uint8_t* data);

    std::unique_ptr<HIDTransport> m_hid_transport;
    std::string m_last_error;

    // Trezor wire protocol constants
    static constexpr uint8_t HEADER_MAGIC_1 = 0x23;  // '#'
    static constexpr uint8_t HEADER_MAGIC_2 = 0x23;  // '#'
    static constexpr size_t HEADER_SIZE = 1 + 1 + 2 + 4;  // ## + type + length
};

// Trezor USB vendor/product IDs
namespace TrezorIDs {
    constexpr uint16_t TREZOR_VENDOR_1 = 0x534c;  // UnaLabs
    constexpr uint16_t TREZOR_VENDOR_2 = 0x1209;  // pid.codes (Trezor One)
    constexpr uint16_t TREZOR_ONE = 0x53c1;
    constexpr uint16_t TREZOR_T = 0x53c0;
}

} // namespace dinero
