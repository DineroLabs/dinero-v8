#pragma once
#include "common/status.h"
#include <stdexcept>
#include <string>

namespace dinero::consensus {
enum class OrchardStateErrorCode {
    Inactive, Context, ParentState, Anchor, DuplicateNullifier,
    SpentNullifier, DuplicateTransaction, DuplicateInput, PoolBalance, ResourceLimit,
    BlockBody, AuthorizationCoverage, RetiredLegacyPool, StateCommitment
};
class OrchardStateError : public std::runtime_error {
public:
    explicit OrchardStateError(OrchardStateErrorCode code)
        : std::runtime_error("Orchard state transition rejected"), code_(code) {}
    OrchardStateErrorCode Code() const noexcept { return code_; }
private:
    OrchardStateErrorCode code_;
};
class OrchardStateLookupError : public std::runtime_error {
public:
    // Same classification and diagnostic as before header extraction.
    explicit OrchardStateLookupError(Status status, const char* operation = nullptr)
        : std::runtime_error(operation ? std::string("Orchard state lookup failed at ") +
              operation + " (" + StatusToString(status) + ")" : "Orchard state lookup failed"),
          status_(status), operation_(operation) {}
    Status SourceStatus() const noexcept { return status_; }
    const char* Operation() const noexcept { return operation_; }
private:
    Status status_;
    const char* operation_;
};
} // namespace dinero::consensus
