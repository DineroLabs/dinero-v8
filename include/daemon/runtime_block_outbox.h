#pragma once
#include "consensus/orchard_state_transition.h"
#include "daemon/runtime_block_notifications.h"
#include <thread>
namespace rocksdb { class WriteBatch; }

namespace dinero {
class ChainDB;
// Local delivery log, not a consensus commitment or a validity certificate.
// Indexed Orchard writes append atomically with canonical state. Records are
// retained across rollback. The first entry establishes a coverage origin;
// it does not certify notification delivery before that origin. Historical
// transitions after that origin join the same log. Whole-reorg intents still
// describe preparation, not committed delivery.
struct RuntimeOutboxCursor {
    uint64_t sequence = 0;
    uint256 digest;
    bool operator==(const RuntimeOutboxCursor&) const = default;
};
struct RuntimeOutboxEvent {
    RuntimeOutboxCursor cursor;
    uint256 previous_digest;
    RuntimeBlockDirection direction;
    consensus::OrchardBlockContext context;
    // At/after activation: exact mixed bytes including Utreexo suffix.
    // Before activation: canonical historical Block serialization.
    std::vector<uint8_t> body;
    bool IsOrchardProfile() const { return context.height >= context.activation_height; }
};
struct RuntimeOutboxPage {
    RuntimeOutboxCursor head, next;
    std::vector<RuntimeOutboxEvent> events;
};
// Prepared before historical validation mutates memory. Abandonment writes
// nothing. The actual historical canonical batch must include this record;
// successful enqueue to an in-memory consumer is not delivery acknowledgment.
// Thread-affine, single-use. Staging failure terminates because its caller may
// already have changed memory. Hold the same selected writer lock throughout.
class PreparedHistoricalRuntimeOutbox {
public:
    PreparedHistoricalRuntimeOutbox(PreparedHistoricalRuntimeOutbox&&) = default;
    PreparedHistoricalRuntimeOutbox& operator=(PreparedHistoricalRuntimeOutbox&&) = default;
    PreparedHistoricalRuntimeOutbox(const PreparedHistoricalRuntimeOutbox&) = delete;
    PreparedHistoricalRuntimeOutbox& operator=(const PreparedHistoricalRuntimeOutbox&) = delete;
    static std::optional<PreparedHistoricalRuntimeOutbox> PrepareUnderLock(
        const ChainDB&, const consensus::OrchardBlockContext& selected_profile,
        const Block&, uint32_t height, RuntimeBlockDirection);
    void StageOrTerminateUnderLock(const ChainDB&, rocksdb::WriteBatch&) noexcept;
private:
    PreparedHistoricalRuntimeOutbox() = default;
    std::string before_, key_, record_, head_;
    uint256 tip_hash_;
    int32_t tip_height_ = 0;
    std::thread::id thread_;
    bool staged_ = false;
};
// Hold the selected writer lock for a consistent view. Only domain and
// activation_height are used from selected_profile. Cursor must be the last
// durably APPLIED consumer checkpoint (or zero). Checks sequence/digest links,
// framing/domain/body identity and adjacent transition hash/height continuity.
// The retained head must end at the persisted canonical tip, including EOF
// reads. Only visited adjacency is checked; this is not a full historical
// audit or proof/script validation. Head/predecessor reads are additional to
// the returned-page byte budget, which is not a resident-memory bound.
// Throws OrchardStateLookupError for absent/corrupt records or bad bounds.
// A budget too small for the next record fails, never impersonates end-of-log.
// No acknowledgement/deletion API: consumers must atomically checkpoint with
// their own effects and be idempotent. No production consumer is installed yet.
[[nodiscard]] RuntimeOutboxPage ReadRuntimeOutboxUnderLock(
    const ChainDB&, const consensus::OrchardBlockContext& selected_profile,
    RuntimeOutboxCursor after = {}, size_t maximum_events = 32,
    size_t maximum_bytes = 16 * 1024 * 1024);
} // namespace dinero
