// Derived financial ownership for versioned vault ledger entries.
// This is replay state inside Ledger, never a second persistence journal.
#pragma once
#include "vault/vault_types.h"
#include <array>
#include <map>
#include <optional>
#include <vector>

namespace dinero::vault {
class Ledger;
using AllocationRequestId = std::array<uint8_t,16>;
struct CreditAllocationRef {
    LedgerSeq credit_seq{0};
    UnaAmount amount{0};
    bool operator==(const CreditAllocationRef&) const = default;
};
struct AllocationPayment {
    OutpointId output;
    std::array<uint8_t,32> body_hash{};
    bool operator==(const AllocationPayment&) const = default;
};
struct AllocationInclusion {
    uint64_t height{0};
    std::array<uint8_t,32> block_hash{};
    bool operator==(const AllocationInclusion&) const = default;
};
struct AllocatedAccountAmounts {
    UnaAmount pending{0},confirmed{0},locked{0},available{0},operator_loss{0};
    bool operator==(const AllocatedAccountAmounts&) const = default;
};

class CreditAllocationState {
public:
    enum class Stage : uint8_t { Pending,Confirmed };
    struct Position {
        AccountId account;
        OutpointId deposit;
        UnaAmount nominal{0},remaining{0},reserved{0};
        Stage stage{Stage::Pending};
        bool active{true};
        bool operator==(const Position&) const = default;
    };
    struct Reservation {
        AccountId account;
        UnaAmount amount{0};
        std::vector<CreditAllocationRef> sources;
        bool dispatch_started{false};
        bool released{false};
        // Derived only from an actual Included entry, retained across undo.
        bool previously_included{false};
        std::optional<AllocationPayment> payment;
        BackendId backend;
        std::optional<AllocationInclusion> inclusion;
        bool operator==(const Reservation&) const = default;
    };
    [[nodiscard]] AllocatedAccountAmounts amounts(const AccountId&) const;
    [[nodiscard]] const auto& positions() const noexcept { return positions_; }
    [[nodiscard]] const auto& reservations() const noexcept { return reservations_; }
    bool operator==(const CreditAllocationState&) const = default;
    void swap(CreditAllocationState&) noexcept;
private:
    friend class Ledger;
    // Only the ledger replay owner may change this state. Its entries bind the
    // domain/account/source references, and its canonical owner proves inclusion.
    // Nothing in this class authenticates an arbitrary caller's chain claim.
    void open(LedgerSeq,const AccountId&,const OutpointId&,UnaAmount);
    void reserve(const AllocationRequestId&,const AccountId&,UnaAmount,
                 const std::vector<CreditAllocationRef>&);
    void beginDispatch(const AllocationRequestId&);
    void bind(const AllocationRequestId&,const AllocationPayment&,const BackendId&);
    void include(const AllocationRequestId&,const AllocationInclusion&);
    void disconnect(const AllocationRequestId&,const AllocationInclusion&);
    void release(const AllocationRequestId&);
    void mature(LedgerSeq,const AccountId&,const OutpointId&);
    void revert(LedgerSeq,const AccountId&,const OutpointId&);
    void restore(LedgerSeq,const AccountId&,const OutpointId&);
    Position& position(LedgerSeq,const AccountId&,const OutpointId&);
    Reservation& reservation(const AllocationRequestId&);
    void validate() const;
    template<class F> void update(F&& f) {
        CreditAllocationState next=*this;
        f(next);next.validate();swap(next);
    }
    std::map<LedgerSeq,Position> positions_;
    std::map<AllocationRequestId,Reservation> reservations_;
};
}
