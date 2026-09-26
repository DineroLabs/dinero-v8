#pragma once
#include "orchard_backend.h"
#include <string>
#include <string_view>
#include <optional>

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
    friend class WalletBundlePlan;
    struct Deleter { void operator()(DineroOrchardWalletKeys*) const noexcept; };
    explicit WalletKeys(DineroOrchardWalletKeys* handle):handle_(handle){}
    std::unique_ptr<DineroOrchardWalletKeys,Deleter> handle_;
};

// Created only by decrypting an already authorized bundle. This is NOT a
// confirmed/unspent note until the wallet binds it to selected chain history.
class WalletNote {
public:
    [[nodiscard]] static std::optional<WalletNote> Receive(const VerifiedAuthorization&,
        const FullViewingKeyBytes&, WalletScope, std::uint32_t action_index);
    WalletNote(WalletNote&&) noexcept = default;
    WalletNote& operator=(WalletNote&&) noexcept = default;
    WalletNote(const WalletNote&) = delete;
    WalletNote& operator=(const WalletNote&) = delete;
    ~WalletNote();
    const DineroOrchardNoteFacts& Facts() const noexcept { return facts_; }
private:
    friend class WalletBundlePlan;
    struct Deleter { void operator()(DineroOrchardNote*) const noexcept; };
    explicit WalletNote(DineroOrchardNote*);
    std::unique_ptr<DineroOrchardNote,Deleter> handle_;
    DineroOrchardNoteFacts facts_{};
};
class WalletWitness {
public:
    [[nodiscard]] static WalletWitness ForAppendedLeaf(const OrchardFrontier& parent,
        std::span<const Hash> ordered_commitments, std::size_t index_in_batch);
    [[nodiscard]] WalletWitness Append(std::span<const Hash> ordered_commitments,
        const Hash& expected_parent, const Hash& expected_next) const;
    [[nodiscard]] std::vector<std::uint8_t> Encode() const;
    [[nodiscard]] static WalletWitness Decode(std::span<const std::uint8_t>,
        const Hash& expected_commitment, const Hash& expected_root, std::uint64_t expected_leaf_count);
    WalletWitness(WalletWitness&&) noexcept = default;
    WalletWitness& operator=(WalletWitness&&) noexcept = default;
    WalletWitness(const WalletWitness&) = delete;
    WalletWitness& operator=(const WalletWitness&) = delete;
    const DineroOrchardWitnessFacts& Facts() const noexcept { return facts_; }
private:
    struct Deleter { void operator()(DineroOrchardWitness*) const noexcept; };
    explicit WalletWitness(DineroOrchardWitness*);
    std::unique_ptr<DineroOrchardWitness,Deleter> handle_;
    DineroOrchardWitnessFacts facts_{};
};
struct WalletSpendInput { const WalletNote& note; const WalletWitness& witness; };

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
    friend class WalletBundlePlan;
    ProvedWalletBundle(std::vector<std::uint8_t> bytes, VerifiedAuthorization authorization)
        :bytes_(std::move(bytes)),authorization_(std::move(authorization)){}
    const std::vector<std::uint8_t> bytes_;
    const VerifiedAuthorization authorization_;
};
// Immutable intent constructed from the real randomized plan and owned signing
// context before proving. It is not an authorization or a broadcastable tx.
class WalletProvingIntent {
public:
    const SigningDomain& Domain()const noexcept{return domain_;}
    const Hash& Message()const noexcept{return message_;}
    const std::vector<ResolvedInput>& Inputs()const noexcept{return inputs_;}
    const std::vector<Hash>& Nullifiers()const noexcept{return nullifiers_;}
private:
    friend class WalletBundlePlan;
    WalletProvingIntent(SigningDomain domain,Hash message,std::vector<ResolvedInput> inputs,std::vector<Hash> nullifiers)
        :domain_(domain),message_(message),inputs_(std::move(inputs)),nullifiers_(std::move(nullifiers)){}
    SigningDomain domain_;
    Hash message_;
    std::vector<ResolvedInput> inputs_;
    std::vector<Hash> nullifiers_;
};
class WalletBundlePlan {
public:
    [[nodiscard]] static WalletBundlePlan PrepareShield(const WalletKeys&, std::span<const WalletPayment>);
    [[nodiscard]] static WalletBundlePlan PrepareSpend(const WalletKeys&,
        std::span<const WalletSpendInput>, const Hash& selected_anchor,
        std::span<const WalletPayment> payments);
    WalletBundlePlan(WalletBundlePlan&&) noexcept = default;
    WalletBundlePlan& operator=(WalletBundlePlan&&) noexcept = default;
    WalletBundlePlan(const WalletBundlePlan&) = delete;
    WalletBundlePlan& operator=(const WalletBundlePlan&) = delete;
    const DineroOrchardFacts& UnprovedFacts() const noexcept { return facts_; }
    [[nodiscard]] WalletProvingIntent Intent(const SigningContext&)const;
    // Consumes this plan, including after a failed attempt. There is no public
    // caller-digest overload. Context must originate from authenticated coins.
    [[nodiscard]] ProvedWalletBundle Prove(const SigningContext&) &&;
private:
    struct Deleter { void operator()(DineroOrchardWalletPlan*) const noexcept; };
    explicit WalletBundlePlan(DineroOrchardWalletPlan* handle);
    std::unique_ptr<DineroOrchardWalletPlan,Deleter> handle_;
    DineroOrchardFacts facts_{};
};
} // namespace dinero::orchard
