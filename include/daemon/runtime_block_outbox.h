#pragma once
#include "consensus/orchard_state_transition.h"
#include "daemon/runtime_block_notifications.h"

namespace dinero {
// Local delivery log, not a consensus commitment or a validity certificate.
// Indexed Orchard writes append atomically with canonical state. Records are
// retained across rollback. The first entry establishes a coverage origin;
// it does not certify notification delivery before that origin. Historical
// transitions and whole-reorg intents still need their own durable handoff.
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
    std::vector<uint8_t> body; // Exact mixed body, including Utreexo suffix.
};
struct RuntimeOutboxPage {
    RuntimeOutboxCursor head, next;
    std::vector<RuntimeOutboxEvent> events;
};
// Hold the selected writer lock for a consistent view. Only domain and
// activation_height are used from selected_profile. Cursor must be the last
// durably APPLIED consumer checkpoint (or zero). Checks sequence/digest links,
// framing/domain/body identity; does not replay proof/script validation.
// Throws OrchardStateLookupError for absent/corrupt records or bad bounds.
// A budget too small for the next record fails, never impersonates end-of-log.
// No acknowledgement/deletion API: consumers must atomically checkpoint with
// their own effects and be idempotent. No production consumer is installed yet.
[[nodiscard]] RuntimeOutboxPage ReadRuntimeOutboxUnderLock(
    const ChainDB&, const consensus::OrchardBlockContext& selected_profile,
    RuntimeOutboxCursor after = {}, size_t maximum_events = 32,
    size_t maximum_bytes = 16 * 1024 * 1024);
} // namespace dinero
