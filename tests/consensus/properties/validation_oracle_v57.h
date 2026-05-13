#pragma once

#include "validation_property_oracle.h"
#include <unordered_map>
#include <vector>
#include <string>

// Ring 2 V5.7: Deterministic Enforcement
// Property: Same invalid input → same rejection path every time

namespace dinero::consensus::test {

class ValidationOracleV57 : public ValidationPropertyOracle {
public:
    ValidationOracleV57() = default;

    std::string name() const override {
        return "V5.7: Deterministic enforcement";
    }

    void reset() override {
        ValidationPropertyOracle::reset();
        rejection_paths_.clear();
    }

    void observe(
        const ValidationState& state,
        const ValidationEvent& event,
        uint64_t event_index
    ) override {
        // Track rejection paths for each input
        if (!event.success) {
            std::string input_id;

            if (event.block.has_value()) {
                input_id = "block:" + event.block.value().GetHash().GetHex();
            } else if (event.transaction.has_value()) {
                input_id = "tx:" + event.transaction.value().GetTxid().AsUint256().GetHex();
            }

            if (!input_id.empty()) {
                RejectionPath path;
                path.input_id = input_id;
                path.error_message = event.error_message;
                path.event_type = event.type;
                path.event_index = event_index;

                // Check if we've seen this input before
                auto it = rejection_paths_.find(input_id);
                if (it != rejection_paths_.end()) {
                    // V5.7: Same input must produce same rejection
                    if (it->second.error_message != path.error_message ||
                        it->second.event_type != path.event_type) {
                        reportViolation(
                            "V5.7",
                            "Non-deterministic rejection for input: " + input_id +
                            " (first: " + it->second.error_message +
                            ", now: " + path.error_message + ")",
                            event_index
                        );
                    }
                } else {
                    // First time seeing this input
                    rejection_paths_[input_id] = path;
                }
            }
        }
    }

private:
    struct RejectionPath {
        std::string input_id;
        std::string error_message;
        ValidationEventType event_type;
        uint64_t event_index;
    };

    std::unordered_map<std::string, RejectionPath> rejection_paths_;
};

} // namespace dinero::consensus::test
