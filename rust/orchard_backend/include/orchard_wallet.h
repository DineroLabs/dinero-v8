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
    friend class WalletShieldPlan;
    struct Deleter { void operator()(DineroOrchardWalletKeys*) const noexcept; };
    explicit WalletKeys(DineroOrchardWalletKeys* handle):handle_(handle){}
    std::unique_ptr<DineroOrchardWalletKeys,Deleter> handle_;
};

struct WalletPayment {
    std::uint64_t amount_una;
    WalletReceiver recipient;
    std::array<std::uint8_t,512> memo{};
};
// Owns the completed canonical inner bundle and the authorization verified
// against the supplied context. Transparent signing/admission remain separate.
class ProvedWalletBundle {
public:
    const std::vector<std::uint8_t>& Bytes() const noexcept { return bytes_; }
    const VerifiedAuthorization& Authorization() const noexcept { return authorization_; }
private:
    friend class WalletShieldPlan;
    ProvedWalletBundle(std::vector<std::uint8_t> bytes, VerifiedAuthorization authorization)
        :bytes_(std::move(bytes)),authorization_(std::move(authorization)){}
    const std::vector<std::uint8_t> bytes_;
    const VerifiedAuthorization authorization_;
};
class WalletShieldPlan {
public:
    [[nodiscard]] static WalletShieldPlan Prepare(const WalletKeys&, std::span<const WalletPayment>);
    WalletShieldPlan(WalletShieldPlan&&) noexcept = default;
    WalletShieldPlan& operator=(WalletShieldPlan&&) noexcept = default;
    WalletShieldPlan(const WalletShieldPlan&) = delete;
    WalletShieldPlan& operator=(const WalletShieldPlan&) = delete;
    const DineroOrchardFacts& UnprovedFacts() const noexcept { return facts_; }
    // Consumes this plan, including after a failed attempt. There is no public
    // caller-digest overload. Context must originate from authenticated coins.
    [[nodiscard]] ProvedWalletBundle Prove(const SigningContext&) &&;
private:
    struct Deleter { void operator()(DineroOrchardShieldPlan*) const noexcept; };
    explicit WalletShieldPlan(DineroOrchardShieldPlan* handle);
    std::unique_ptr<DineroOrchardShieldPlan,Deleter> handle_;
    DineroOrchardFacts facts_{};
};
} // namespace dinero::orchard
