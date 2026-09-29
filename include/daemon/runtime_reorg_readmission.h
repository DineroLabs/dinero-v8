#pragma once
#include "daemon/runtime_outbox_cursor.h"
#include <cstddef>
#include <optional>
#include <vector>
#include "daemon/mempool_transaction.h"
#include "daemon/interfaces/ingress_types.h"

namespace dinero {
// A report about one retained intent at one checked canonical head. Neither
// this cursor nor any successful admission is a durable consumer checkpoint.
struct RuntimeReorgReadmission {
    std::optional<RuntimeOutboxCursor> intent;
    RuntimeOutboxCursor observed_head;
    size_t planned_disconnects = 0;
    size_t matched_disconnects = 0;
    struct Entry {
        MempoolTransaction body;
        TxAcceptResult result;
        bool present_after_attempt = false;
    };
    // Parent blocks first, original transaction order within each block.
    std::vector<Entry> entries;
};
} // namespace dinero
