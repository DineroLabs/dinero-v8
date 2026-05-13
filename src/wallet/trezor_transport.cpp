#include "wallet/trezor_transport.h"
#include "wallet/hid_transport.h"
#include "common/logger.h"
#include <google/protobuf/message.h>

namespace dinero {

extern Logger g_logger;  // Defined in main.cpp

TrezorTransport::TrezorTransport()
    : m_hid_transport(std::make_unique<HIDTransport>())
{
}

TrezorTransport::~TrezorTransport() {
    close();
}

bool TrezorTransport::open(uint16_t vendor_id, uint16_t product_id) {
    if (m_hid_transport->isOpen()) {
        m_last_error = "Device already open";
        return false;
    }

    if (!m_hid_transport->open(vendor_id, product_id)) {
        m_last_error = "Failed to open Trezor device";
        g_logger.error("[TrezorTransport] " + m_last_error);
        return false;
    }

    g_logger.info("[TrezorTransport] Trezor device opened successfully");
    return true;
}

bool TrezorTransport::openPath(const std::string& path) {
    if (m_hid_transport->isOpen()) {
        m_last_error = "Device already open";
        return false;
    }

    if (!m_hid_transport->openPath(path)) {
        m_last_error = "Failed to open Trezor device at HID path";
        g_logger.error("[TrezorTransport] " + m_last_error + ": " + path);
        return false;
    }

    g_logger.info("[TrezorTransport] Trezor device opened successfully at path: " + path);
    return true;
}

void TrezorTransport::close() {
    if (m_hid_transport->isOpen()) {
        m_hid_transport->close();
        g_logger.info("[TrezorTransport] Trezor device closed");
    }
}

bool TrezorTransport::isOpen() const {
    return m_hid_transport->isOpen();
}

bool TrezorTransport::sendMessage(uint16_t msg_type, const google::protobuf::Message& message) {
    if (!m_hid_transport->isOpen()) {
        m_last_error = "Device not open";
        return false;
    }

    // Serialize protobuf message
    std::vector<uint8_t> payload(message.ByteSizeLong());
    if (!message.SerializeToArray(payload.data(), payload.size())) {
        m_last_error = "Failed to serialize protobuf message";
        g_logger.error("[TrezorTransport] " + m_last_error);
        return false;
    }

    // Build wire protocol frame
    std::vector<uint8_t> frame = buildFrame(msg_type, payload);

    // Send frame via HID transport
    int bytes_written = m_hid_transport->write(frame);
    if (bytes_written <= 0) {
        m_last_error = "Failed to write to HID device";
        g_logger.error("[TrezorTransport] " + m_last_error);
        return false;
    }

    g_logger.debug("[TrezorTransport] Sent message type 0x" + std::to_string(msg_type) +
                   " (" + std::to_string(payload.size()) + " bytes)");
    return true;
}

bool TrezorTransport::recvMessage(uint16_t& msg_type, std::vector<uint8_t>& payload) {
    if (!m_hid_transport->isOpen()) {
        m_last_error = "Device not open";
        return false;
    }

    // Read response from HID device
    std::vector<uint8_t> response = m_hid_transport->read();
    if (response.empty()) {
        m_last_error = "Failed to read from HID device";
        g_logger.error("[TrezorTransport] " + m_last_error);
        return false;
    }

    // Parse wire protocol frame
    if (!parseFrame(response, msg_type, payload)) {
        m_last_error = "Failed to parse Trezor wire frame";
        g_logger.error("[TrezorTransport] " + m_last_error);
        return false;
    }

    g_logger.debug("[TrezorTransport] Received message type 0x" + std::to_string(msg_type) +
                   " (" + std::to_string(payload.size()) + " bytes)");
    return true;
}

std::vector<uint8_t> TrezorTransport::buildFrame(uint16_t msg_type, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> frame;
    frame.reserve(HEADER_SIZE + payload.size());

    // Header: ## (0x23, 0x23)
    frame.push_back(HEADER_MAGIC_1);
    frame.push_back(HEADER_MAGIC_2);

    // Message type (uint16_t, big endian)
    uint16ToBE(msg_type, frame);

    // Payload length (uint32_t, big endian)
    uint32ToBE(static_cast<uint32_t>(payload.size()), frame);

    // Payload
    frame.insert(frame.end(), payload.begin(), payload.end());

    return frame;
}

bool TrezorTransport::parseFrame(const std::vector<uint8_t>& frame, uint16_t& msg_type, std::vector<uint8_t>& payload) {
    // Verify minimum frame size
    if (frame.size() < HEADER_SIZE) {
        m_last_error = "Frame too small (minimum " + std::to_string(HEADER_SIZE) + " bytes)";
        return false;
    }

    // Verify header magic
    if (frame[0] != HEADER_MAGIC_1 || frame[1] != HEADER_MAGIC_2) {
        m_last_error = "Invalid frame header (expected ##)";
        return false;
    }

    // Parse message type (big endian)
    msg_type = BEToUint16(&frame[2]);

    // Parse payload length (big endian)
    uint32_t payload_len = BEToUint32(&frame[4]);

    // Verify payload length matches frame size
    if (frame.size() != HEADER_SIZE + payload_len) {
        m_last_error = "Frame size mismatch (expected " + std::to_string(HEADER_SIZE + payload_len) +
                       " bytes, got " + std::to_string(frame.size()) + ")";
        return false;
    }

    // Extract payload
    payload.assign(frame.begin() + HEADER_SIZE, frame.end());

    return true;
}

void TrezorTransport::uint16ToBE(uint16_t value, std::vector<uint8_t>& buffer) {
    buffer.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    buffer.push_back(static_cast<uint8_t>(value & 0xFF));
}

void TrezorTransport::uint32ToBE(uint32_t value, std::vector<uint8_t>& buffer) {
    buffer.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    buffer.push_back(static_cast<uint8_t>(value & 0xFF));
}

uint16_t TrezorTransport::BEToUint16(const uint8_t* data) {
    return (static_cast<uint16_t>(data[0]) << 8) |
            static_cast<uint16_t>(data[1]);
}

uint32_t TrezorTransport::BEToUint32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
            static_cast<uint32_t>(data[3]);
}

} // namespace dinero
