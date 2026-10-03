#pragma once
#include "wallet/orchard_account_delivery.h"
#include "daemon/runtime_block_outbox.h"

namespace dinero {
class ChainstateService;
// Immutable branch views assembled by the selected service before wallet
// ownership. Revalidates Orchard authorizations/state from retained canonical
// replay material; does not independently certify historical consensus/baseline.
// Missing pre-origin history or old records without replay frames refuse.
class RuntimeAccountReplay {
public:
    RuntimeOutboxCursor Head() const;
    const RuntimeOutboxEvent& Event(uint64_t sequence) const;
    wallet::OrchardAccountDelivery::RestorePoint Point(RuntimeOutboxCursor) const;
    // Immutable branch identity at a covered height; missing ancestry refuses.
    std::function<StatusOr<uint256>(uint32_t)> SelectedHashes(RuntimeOutboxCursor) const;
    uint32_t ForkHeight(RuntimeOutboxCursor,const uint256& block,uint32_t height) const;
    bool IsAncestorOf(RuntimeOutboxCursor,const uint256& block,uint32_t height) const;
    const OrchardBlockCandidate& Block(uint64_t sequence) const;
    const consensus::PreparedOrchardState& State(uint64_t sequence) const;
    const std::vector<consensus::VerifiedOrchardAuthorizations>& Authorizations(uint64_t sequence) const;
private:
    friend class ChainstateService;
    friend struct RuntimeAccountReplayTestAccess;
    using Source=std::function<RuntimeOutboxPage(RuntimeOutboxCursor,size_t)>;
    static std::shared_ptr<const RuntimeAccountReplay> Capture(const Source&);
    struct Data;
    static std::shared_ptr<Data> ReadSource(const Source&);
    static std::shared_ptr<const RuntimeAccountReplay> Build(std::shared_ptr<Data>);
    explicit RuntimeAccountReplay(std::shared_ptr<const Data> data):data_(std::move(data)){}
    std::shared_ptr<const Data> data_;
};
} // namespace dinero
