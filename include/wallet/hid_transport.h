#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include <memory>
#include <optional>

// Forward declaration for hidapi
struct hid_device_;
typedef struct hid_device_ hid_device;

namespace dinero {

/**
 * @brief USB HID Transport for Hardware Wallets
 *
 * Cross-platform abstraction over hidapi for Ledger/Trezor communication.
 * Uses:
 * - macOS: IOKit (native)
 * - Linux: libusb
 * - Windows: WinUSB/HID
 *
 * Security: This is pure I/O - no crypto, no key handling.
 * Keys never leave the device.
 */
class HIDTransport {
public:
    /**
     * @brief Device information from enumeration
     */
    struct DeviceInfo {
        uint16_t vendor_id;
        uint16_t product_id;
        std::string manufacturer;
        std::string product;
        std::string serial;
        std::string path;  // Platform-specific device path
    };

    HIDTransport();
    ~HIDTransport();

    // Non-copyable (manages raw HID device handle)
    HIDTransport(const HIDTransport&) = delete;
    HIDTransport& operator=(const HIDTransport&) = delete;

    /**
     * @brief Enumerate all HID devices
     *
     * @param vendor_id Optional: Filter by vendor ID (0 = all)
     * @param product_id Optional: Filter by product ID (0 = all)
     * @return Vector of device information
     */
    static std::vector<DeviceInfo> enumerate(uint16_t vendor_id = 0, uint16_t product_id = 0);

    /**
     * @brief Open device by vendor/product ID
     *
     * @param vendor_id Vendor ID (e.g., 0x2c97 for Ledger)
     * @param product_id Product ID
     * @return true if opened successfully
     */
    bool open(uint16_t vendor_id, uint16_t product_id);

    /**
     * @brief Open device by path (from enumerate())
     *
     * @param path Platform-specific device path
     * @return true if opened successfully
     */
    bool openPath(const std::string& path);

    /**
     * @brief Close the device
     */
    void close();

    /**
     * @brief Check if device is open
     */
    bool isOpen() const { return device_ != nullptr; }

    /**
     * @brief Write data to device
     *
     * @param data Bytes to write (HID report)
     * @param timeout_ms Timeout in milliseconds
     * @return Number of bytes written, or -1 on error
     */
    int write(const std::vector<uint8_t>& data, int timeout_ms = 5000);

    /**
     * @brief Read data from device
     *
     * @param max_length Maximum bytes to read
     * @param timeout_ms Timeout in milliseconds
     * @return Data read (empty if error or timeout)
     */
    std::vector<uint8_t> read(size_t max_length = 64, int timeout_ms = 5000);

    /**
     * @brief Get last error message
     */
    std::string getError() const;

    /**
     * @brief Set non-blocking mode
     *
     * @param nonblocking true for non-blocking reads
     */
    void setNonBlocking(bool nonblocking);

private:
    hid_device* device_ = nullptr;
};

// Known hardware wallet vendor/product IDs
namespace HardwareWalletIDs {
    // Ledger
    constexpr uint16_t LEDGER_VENDOR = 0x2c97;
    constexpr uint16_t LEDGER_NANO_S = 0x0001;
    constexpr uint16_t LEDGER_NANO_X = 0x0004;
    constexpr uint16_t LEDGER_NANO_S_PLUS = 0x0005;

    // Trezor
    constexpr uint16_t TREZOR_VENDOR = 0x534c;
    constexpr uint16_t TREZOR_ONE = 0x0001;
    constexpr uint16_t TREZOR_MODEL_T = 0x0006;
}

} // namespace dinero
