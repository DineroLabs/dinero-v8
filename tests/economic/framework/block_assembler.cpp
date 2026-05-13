#include "block_assembler.h"
#include <algorithm>
#include <numeric>
#include <sstream>
#include <iomanip>

namespace dinero {
namespace economic {
namespace test {

BlockAssembler::BlockAssembler(const EconomicPolicy& policy)
    : policy_(policy)
    , event_sequence_(0)
{
}

BlockTemplateState BlockAssembler::assembleTemplate(
    const MempoolSimulator& mempool,
    uint32_t chain_height,
    uint64_t timestamp
) {
    return assembleTemplateWithRequirements(mempool, {}, chain_height, timestamp);
}

BlockTemplateState BlockAssembler::assembleTemplateWithRequirements(
    const MempoolSimulator& mempool,
    const std::vector<TxID>& required_txs,
    uint32_t chain_height,
    uint64_t timestamp
) {
    clearEvents();

    // Get all mempool transactions
    auto candidates = mempool.getTxsSortedByFeeRate();  // Already sorted by fee rate (descending)

    // If there are required txs, ensure they're included first
    std::vector<MempoolEntry> selected;
    uint32_t current_size = 0;

    // Add required transactions first
    for (const auto& required_tx_id : required_txs) {
        auto tx_entry = mempool.getTx(required_tx_id);
        if (tx_entry) {
            if (fits(current_size, tx_entry->tx_size_bytes, policy_.max_block_size_bytes)) {
                selected.push_back(*tx_entry);
                current_size += tx_entry->tx_size_bytes;

                // Record selection event
                EconomicEvent event;
                event.type = EconomicEventType::TX_SELECTED_FOR_BLOCK;
                event.timestamp = timestamp;
                event.sequence_number = event_sequence_++;
                event.node_id = mempool.getNodeId();
                event.tx_id = required_tx_id;
                event.fee_una = tx_entry->fee_una;
                event.fee_rate = tx_entry->fee_rate;
                event.success = true;
                event.details = "Required transaction";
                recordEvent(event);
            }
        }
    }

    // Greedy selection from remaining candidates
    for (const auto& entry : candidates) {
        // Skip if already included (was required)
        bool already_included = false;
        for (const auto& selected_entry : selected) {
            if (selected_entry.tx_id == entry.tx_id) {
                already_included = true;
                break;
            }
        }
        if (already_included) {
            continue;
        }

        // Check if it fits
        if (fits(current_size, entry.tx_size_bytes, policy_.max_block_size_bytes)) {
            selected.push_back(entry);
            current_size += entry.tx_size_bytes;

            // Record selection event
            EconomicEvent event;
            event.type = EconomicEventType::TX_SELECTED_FOR_BLOCK;
            event.timestamp = timestamp;
            event.sequence_number = event_sequence_++;
            event.node_id = mempool.getNodeId();
            event.tx_id = entry.tx_id;
            event.fee_una = entry.fee_una;
            event.fee_rate = entry.fee_rate;
            event.success = true;
            event.details = "Greedy selection";
            recordEvent(event);
        } else {
            // Record exclusion event
            EconomicEvent event;
            event.type = EconomicEventType::TX_EXCLUDED_FROM_BLOCK;
            event.timestamp = timestamp;
            event.sequence_number = event_sequence_++;
            event.node_id = mempool.getNodeId();
            event.tx_id = entry.tx_id;
            event.fee_una = entry.fee_una;
            event.fee_rate = entry.fee_rate;
            event.success = false;
            event.error_message = "Block full";
            recordEvent(event);
        }
    }

    // Calculate total fees
    uint64_t total_fees = 0;
    std::vector<TxID> tx_ids;
    for (const auto& entry : selected) {
        total_fees += entry.fee_una;
        tx_ids.push_back(entry.tx_id);
    }

    // Create template state
    BlockTemplateState template_state;
    template_state.template_hash = createTemplateHash(tx_ids, chain_height, timestamp);
    template_state.height = chain_height;
    template_state.included_txs = tx_ids;
    template_state.total_fees = total_fees;
    template_state.total_size_bytes = current_size;
    template_state.creation_time = timestamp;

    // Record template assembled event
    EconomicEvent assembled_event;
    assembled_event.type = EconomicEventType::BLOCK_TEMPLATE_ASSEMBLED;
    assembled_event.timestamp = timestamp;
    assembled_event.sequence_number = event_sequence_++;
    assembled_event.node_id = mempool.getNodeId();
    assembled_event.block_hash = template_state.template_hash;
    assembled_event.block_height = chain_height;
    assembled_event.template_txs = tx_ids;
    assembled_event.template_total_fees = total_fees;
    assembled_event.success = true;
    assembled_event.details = "Template assembled with " + std::to_string(tx_ids.size()) + " transactions";
    recordEvent(assembled_event);

    return template_state;
}

std::vector<MempoolEntry> BlockAssembler::greedySelect(
    const std::vector<MempoolEntry>& candidates,
    uint32_t max_size
) {
    std::vector<MempoolEntry> selected;
    uint32_t current_size = 0;

    // Candidates should already be sorted by fee rate (descending)
    for (const auto& entry : candidates) {
        if (fits(current_size, entry.tx_size_bytes, max_size)) {
            selected.push_back(entry);
            current_size += entry.tx_size_bytes;
        }
    }

    return selected;
}

bool BlockAssembler::fits(uint32_t current_size, uint32_t tx_size, uint32_t max_size) const {
    return (current_size + tx_size) <= max_size;
}

void BlockAssembler::recordEvent(EconomicEvent event) {
    assembly_events_.push_back(event);
}

BlockHash BlockAssembler::createTemplateHash(
    const std::vector<TxID>& tx_ids,
    uint32_t height,
    uint64_t timestamp
) const {
    // Simple deterministic hash: combine height, timestamp, and tx_ids
    std::stringstream ss;
    ss << "template_" << height << "_" << timestamp << "_";

    for (size_t i = 0; i < tx_ids.size() && i < 5; ++i) {  // Include first 5 tx_ids
        ss << tx_ids[i];
        if (i < tx_ids.size() - 1) {
            ss << "_";
        }
    }

    return ss.str();
}

} // namespace test
} // namespace economic
} // namespace dinero
