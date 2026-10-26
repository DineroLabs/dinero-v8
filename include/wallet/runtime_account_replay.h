#pragma once
#include "wallet/orchard_account_delivery.h"
#include "daemon/runtime_block_outbox.h"

namespace dinero {
class ChainstateService;
namespace consensus {class ValidatedOrchardBlock;}
// Immutable branch views assembled by the selected service before wallet
// ownership. Revalidates Orchard authorizations/state from retained canonical
// replay material; does not independently certify historical consensus/baseline.
// Ordinary retained-only capture refuses missing pre-origin history/frames.
// A separate private factory can prepend genuinely revalidated activation
// bodies without creating or renumbering any canonical delivery event.
class RuntimeAccountReplay {
public:
    // Each non-null handle owns its immutable value independently of this
    // view and of other records. Retain the handle while borrowing references
    // into its value. This allows bounded storage to release unrelated
    // records without invalidating a caller's active record.
    using EventHandle = std::shared_ptr<const RuntimeOutboxEvent>;
    using BlockHandle = std::shared_ptr<const OrchardBlockCandidate>;
    using StateHandle = std::shared_ptr<const consensus::PreparedOrchardState>;
    using AuthorizationHandle = std::shared_ptr<const std::vector<consensus::VerifiedOrchardAuthorizations>>;
    RuntimeOutboxCursor Head() const;
    EventHandle Event(uint64_t sequence) const;
    wallet::OrchardAccountDelivery::RestorePoint Point(RuntimeOutboxCursor) const;
    // Immutable branch identity at a covered height; missing ancestry refuses.
    std::function<StatusOr<uint256>(uint32_t)> SelectedHashes(RuntimeOutboxCursor) const;
    uint32_t ForkHeight(RuntimeOutboxCursor,const uint256& block,uint32_t height) const;
    bool IsAncestorOf(RuntimeOutboxCursor,const uint256& block,uint32_t height) const;
    BlockHandle Block(uint64_t sequence) const;
    StateHandle State(uint64_t sequence) const;
    AuthorizationHandle Authorizations(uint64_t sequence) const;
    // A separately revalidated pre-outbox prefix never manufactures local
    // delivery events. These observations are for uncertified recovery scans;
    // only Event/AdvanceDelivery may advance a canonical receipt.
    struct ActivationPrefixStep {
        consensus::OrchardBlockContext context;
        BlockHandle block;
        StateHandle state;
        AuthorizationHandle authorizations;
    };
    uint32_t ActivationPrefixHeight() const;
    wallet::OrchardAccountDelivery::RestorePoint ActivationOrigin() const;
    wallet::OrchardAccountDelivery::RestorePoint UnappliedPrefixPoint(uint32_t,const uint256&) const;
    ActivationPrefixStep PrefixStep(uint32_t) const;
private:
    friend class ChainstateService;
    friend struct RuntimeAccountReplayTestAccess;
    using Source=std::function<RuntimeOutboxPage(RuntimeOutboxCursor,size_t)>;
    static std::shared_ptr<const RuntimeAccountReplay> Capture(const Source&);
    struct Data;
    static std::shared_ptr<Data> ReadSource(const Source&);
    static std::shared_ptr<const RuntimeAccountReplay> Build(std::shared_ptr<Data>);
    static std::optional<std::pair<uint32_t,uint256>> ActivationPrefixBase(const std::shared_ptr<Data>&);
    struct VerifiedPrefixStep {
        BlockHandle block;
        std::unique_ptr<const consensus::ValidatedOrchardBlock> checked;
    };
    static std::shared_ptr<const RuntimeAccountReplay> BuildWithActivationPrefix(
        std::shared_ptr<Data>,const std::function<VerifiedPrefixStep(uint32_t)>&);
    static std::shared_ptr<const RuntimeAccountReplay> BuildTail(std::shared_ptr<Data>);
    wallet::OrchardAccountDelivery::RestorePoint PointAt(const uint256&) const;
    explicit RuntimeAccountReplay(std::shared_ptr<const Data> data):data_(std::move(data)){}
    std::shared_ptr<const Data> data_;
};
} // namespace dinero
