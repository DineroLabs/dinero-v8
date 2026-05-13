#include "wallet/hardware_wallet.h"
#include "common/logger.h"
#include <algorithm>

namespace dinero {

std::shared_ptr<HardwareWallet> HardwareWalletFactory::createWallet(HardwareWalletType type) {
    g_logger.info("Creating hardware wallet of type: " + getTypeName(type));
    
    switch (type) {
        case HardwareWalletType::LEDGER:
            return std::make_shared<LedgerWallet>();
            
        case HardwareWalletType::TREZOR:
            return std::make_shared<TrezorWallet>();
            
        case HardwareWalletType::UNKNOWN:
        default:
            g_logger.error("Unknown hardware wallet type: " + std::to_string(static_cast<int>(type)));
            return nullptr;
    }
}

std::shared_ptr<HardwareWallet> HardwareWalletFactory::createWallet(const std::string& device_path) {
    g_logger.info("Creating hardware wallet for device: " + device_path);
    
    HardwareWalletType type = detectType(device_path);
    if (type == HardwareWalletType::UNKNOWN) {
        g_logger.error("Could not detect hardware wallet type for device: " + device_path);
        return nullptr;
    }
    
    return createWallet(type);
}

HardwareWalletType HardwareWalletFactory::detectType(const std::string& device_path) {
    g_logger.debug("Detecting hardware wallet type for device: " + device_path);
    
    // Convert to lowercase for comparison
    std::string path_lower = device_path;
    std::transform(path_lower.begin(), path_lower.end(), path_lower.begin(), ::tolower);
    
    // Check for Ledger devices
    if (path_lower.find("ledger") != std::string::npos || 
        path_lower.find("nano") != std::string::npos ||
        path_lower.find("s") != std::string::npos ||
        path_lower.find("x") != std::string::npos) {
        g_logger.debug("Detected Ledger device: " + device_path);
        return HardwareWalletType::LEDGER;
    }
    
    // Check for Trezor devices
    if (path_lower.find("trezor") != std::string::npos ||
        path_lower.find("model") != std::string::npos ||
        path_lower.find("one") != std::string::npos ||
        path_lower.find("t") != std::string::npos) {
        g_logger.debug("Detected Trezor device: " + device_path);
        return HardwareWalletType::TREZOR;
    }
    
    // Check for USB vendor/product IDs
    if (path_lower.find("0x2c97") != std::string::npos || // Ledger vendor ID
        path_lower.find("0x0001") != std::string::npos || // Ledger Nano S
        path_lower.find("0x0004") != std::string::npos || // Ledger Nano X
        path_lower.find("0x0005") != std::string::npos) { // Ledger Nano S Plus
        g_logger.debug("Detected Ledger device by vendor ID: " + device_path);
        return HardwareWalletType::LEDGER;
    }
    
    if (path_lower.find("0x534c") != std::string::npos || // Trezor vendor ID
        path_lower.find("0x0001") != std::string::npos || // Trezor One
        path_lower.find("0x0002") != std::string::npos || // Trezor T
        path_lower.find("0x0003") != std::string::npos) { // Trezor T
        g_logger.debug("Detected Trezor device by vendor ID: " + device_path);
        return HardwareWalletType::TREZOR;
    }
    
    g_logger.warning("Could not detect hardware wallet type for device: " + device_path);
    return HardwareWalletType::UNKNOWN;
}

std::vector<HardwareWalletType> HardwareWalletFactory::getSupportedTypes() {
    return {
        HardwareWalletType::LEDGER,
        HardwareWalletType::TREZOR
    };
}

std::string HardwareWalletFactory::getTypeName(HardwareWalletType type) {
    switch (type) {
        case HardwareWalletType::LEDGER:
            return "Ledger";
            
        case HardwareWalletType::TREZOR:
            return "Trezor";
            
        case HardwareWalletType::UNKNOWN:
        default:
            return "Unknown";
    }
}

} // namespace dinero 