#pragma once
#include "orchard_backend.h"
#include <string>
#include <string_view>

namespace dinero::orchard {
enum class WalletScope : uint8_t { External=0, Internal=1 };
enum class WalletNetwork : uint8_t { Mainnet=0, Testnet=1, Regtest=2 };
using DiversifierIndex = std::array<uint8_t,11>; // ZIP32 little-endian 88-bit index.
using FullViewingKeyBytes = std::array<uint8_t,96>; // Private wallet activity data.

class WalletReceiver {
public:
    [[nodiscard]] static WalletReceiver FromViewingKey(const FullViewingKeyBytes&,
        WalletScope, const DiversifierIndex&);
    [[nodiscard]] static WalletReceiver DecodeAddress(std::string_view, WalletNetwork expected_network);
    [[nodiscard]] std::string EncodeAddress(WalletNetwork) const;
    const std::array<uint8_t,43>& Raw() const noexcept { return raw_; }
    bool operator==(const WalletReceiver&) const = default;
private:
    explicit WalletReceiver(std::array<uint8_t,43> raw):raw_(raw){}
    const std::array<uint8_t,43> raw_;
};

// Opaque move-only spending-key owner. No raw spending key leaves Rust.
// The existing wallet must supply its protected master seed; this component
// neither generates entropy nor reads/writes a wallet or mainnet data directory.
class WalletKeys {
public:
    [[nodiscard]] static WalletKeys FromSeed(std::span<const uint8_t>, uint32_t account);
    WalletKeys(WalletKeys&&) noexcept = default;
    WalletKeys& operator=(WalletKeys&&) noexcept = default;
    WalletKeys(const WalletKeys&) = delete;
    WalletKeys& operator=(const WalletKeys&) = delete;
    [[nodiscard]] FullViewingKeyBytes ExportFullViewingKey() const;
    [[nodiscard]] WalletReceiver Receiver(WalletScope, const DiversifierIndex&) const;
private:
    struct Deleter { void operator()(DineroOrchardWalletKeys*) const noexcept; };
    explicit WalletKeys(DineroOrchardWalletKeys* handle):handle_(handle){}
    std::unique_ptr<DineroOrchardWalletKeys,Deleter> handle_;
};
} // namespace dinero::orchard
