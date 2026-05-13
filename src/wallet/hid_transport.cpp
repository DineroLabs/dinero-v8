/**
 * HID Transport Implementation
 *
 * Cross-platform USB HID communication for hardware wallets.
 * Uses hidapi which abstracts OS differences:
 * - macOS: IOKit
 * - Linux: libusb
 * - Windows: WinUSB/HID
 */

#include "wallet/hid_transport.h"
#include <hidapi.h>  // vendored: third_party/hidapi
#include <cstring>
#include <iostream>

namespace dinero {

HIDTransport::HIDTransport() : device_(nullptr) {
    // Initialize hidapi (safe to call multiple times)
    hid_init();
}

HIDTransport::~HIDTransport() {
    close();
    // Note: We don't call hid_exit() here because hidapi uses a global state
    // and other HIDTransport instances might still be active
}

std::vector<HIDTransport::DeviceInfo> HIDTransport::enumerate(uint16_t vendor_id, uint16_t product_id) {
    std::vector<DeviceInfo> devices;

    struct hid_device_info* devs = hid_enumerate(vendor_id, product_id);
    struct hid_device_info* cur_dev = devs;

    while (cur_dev) {
        DeviceInfo info;
        info.vendor_id = cur_dev->vendor_id;
        info.product_id = cur_dev->product_id;
        info.path = cur_dev->path ? cur_dev->path : "";

        // Convert wide strings to UTF-8
        if (cur_dev->manufacturer_string) {
            wchar_t* wstr = cur_dev->manufacturer_string;
            char buf[256];
            wcstombs(buf, wstr, sizeof(buf));
            info.manufacturer = buf;
        }

        if (cur_dev->product_string) {
            wchar_t* wstr = cur_dev->product_string;
            char buf[256];
            wcstombs(buf, wstr, sizeof(buf));
            info.product = buf;
        }

        if (cur_dev->serial_number) {
            wchar_t* wstr = cur_dev->serial_number;
            char buf[256];
            wcstombs(buf, wstr, sizeof(buf));
            info.serial = buf;
        }

        devices.push_back(info);
        cur_dev = cur_dev->next;
    }

    hid_free_enumeration(devs);
    return devices;
}

bool HIDTransport::open(uint16_t vendor_id, uint16_t product_id) {
    if (device_) {
        std::cerr << "[HIDTransport] Device already open, closing first" << std::endl;
        close();
    }

    device_ = hid_open(vendor_id, product_id, nullptr);
    if (!device_) {
        std::cerr << "[HIDTransport] Failed to open device "
                  << std::hex << "0x" << vendor_id << ":0x" << product_id << std::dec
                  << " - " << getError() << std::endl;
        return false;
    }

    std::cout << "[HIDTransport] Opened device "
              << std::hex << "0x" << vendor_id << ":0x" << product_id << std::dec << std::endl;
    return true;
}

bool HIDTransport::openPath(const std::string& path) {
    if (device_) {
        std::cerr << "[HIDTransport] Device already open, closing first" << std::endl;
        close();
    }

    device_ = hid_open_path(path.c_str());
    if (!device_) {
        std::cerr << "[HIDTransport] Failed to open device at path: " << path
                  << " - " << getError() << std::endl;
        return false;
    }

    std::cout << "[HIDTransport] Opened device at path: " << path << std::endl;
    return true;
}

void HIDTransport::close() {
    if (device_) {
        hid_close(device_);
        device_ = nullptr;
        std::cout << "[HIDTransport] Device closed" << std::endl;
    }
}

int HIDTransport::write(const std::vector<uint8_t>& data, int timeout_ms) {
    if (!device_) {
        std::cerr << "[HIDTransport] Cannot write: device not open" << std::endl;
        return -1;
    }

    if (data.empty()) {
        std::cerr << "[HIDTransport] Cannot write: empty data" << std::endl;
        return -1;
    }

    // hidapi write sends HID report
    int result = hid_write(device_, data.data(), data.size());

    if (result < 0) {
        std::cerr << "[HIDTransport] Write failed: " << getError() << std::endl;
        return -1;
    }

    return result;
}

std::vector<uint8_t> HIDTransport::read(size_t max_length, int timeout_ms) {
    if (!device_) {
        std::cerr << "[HIDTransport] Cannot read: device not open" << std::endl;
        return {};
    }

    std::vector<uint8_t> buffer(max_length);

    int result = hid_read_timeout(device_, buffer.data(), buffer.size(), timeout_ms);

    if (result < 0) {
        std::cerr << "[HIDTransport] Read failed: " << getError() << std::endl;
        return {};
    }

    if (result == 0) {
        // Timeout - not an error, just no data
        return {};
    }

    // Resize to actual bytes read
    buffer.resize(result);
    return buffer;
}

std::string HIDTransport::getError() const {
    if (!device_) {
        return "Device not open";
    }

    const wchar_t* err = hid_error(device_);
    if (!err) {
        return "Unknown error";
    }

    // Convert wide string to UTF-8
    char buf[512];
    wcstombs(buf, err, sizeof(buf));
    return std::string(buf);
}

void HIDTransport::setNonBlocking(bool nonblocking) {
    if (device_) {
        hid_set_nonblocking(device_, nonblocking ? 1 : 0);
    }
}

} // namespace dinero
