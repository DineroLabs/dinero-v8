#include "vault/state_snapshot.h"

#include <algorithm>
#include <climits>
#include <limits>
#include <map>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>

namespace dinero::vault {
namespace {
void Require(bool value) {
    if (!value) throw std::runtime_error("vault saved state and ledger binding mismatch");
}
bool Nonzero(const auto& value) {
    return std::any_of(value.begin(),value.end(),[](uint8_t byte){return byte!=0;});
}
struct DepositBinding {
    const VaultSavedDeposit* saved{};
    // 0 detected, 1 observed, 2 credited, 3 settled, 4 ledger-reverted.
    unsigned progress{0};
    bool compensated{false};
    LedgerSeq reversalSeq{0};
    LedgerSeq compensationSeq{0};
};
struct WithdrawalBinding {
    const VaultSavedWithdrawal* saved{};
    // 0 no entry, 1 initiated, 2 settled, 3 reverted.
    unsigned progress{0};
};
}

Ledger ReplayVaultStateLedger(const VaultStateSnapshot& state) {
    // Apply the same representation checks to in-memory candidates and reads.
    // This also bounds counts/bytes before replay and refuses duplicate ids.
    (void)EncodeVaultState(state);
    Require(state.revision<=INT64_MAX);
    if (state.revision==0)
        Require(state.entries.empty() && state.deposits.empty() && state.withdrawals.empty());

    // Replay is the single owner of source accounting and transition checks.
    // Snapshot bindings below reconcile its complete result with the saved
    // requests/deposits; no second approximation of financial replay is used.
    auto replayed=Ledger::replay(state.entries,state.config.ledger_caps);
    const auto& allocations=replayed.creditAllocations();
    std::map<AllocationRequestId,const VaultSavedWithdrawal*> requests;
    std::unordered_map<OutpointId,DepositBinding> deposits;
    for (const auto& row:state.deposits) {
        Require(Nonzero(row.observed_block));
        Require(deposits.emplace(row.deposit.outpoint,DepositBinding{&row}).second);
    }
    std::unordered_map<OutpointId,WithdrawalBinding> withdrawals;
    std::unordered_map<AccountId,UnaAmount> outstanding;
    size_t depth=0;
    for (const auto& row:state.withdrawals) {
        const auto& request=row.request;
        Require(requests.emplace(request.request_id,&row).second);
        Require(Nonzero(request.request_id) && request.amount>0 &&
                !request.destination_script_pub_key.empty());
        Require(request.amount<=state.config.withdrawal_caps.per_request);
        bool active=std::holds_alternative<WithdrawalPending>(row.state) ||
                    std::holds_alternative<WithdrawalSigning>(row.state) ||
                    std::holds_alternative<WithdrawalBroadcast>(row.state) ||
                    std::holds_alternative<WithdrawalPaymentRetained>(row.state);
        const auto allocated=allocations.reservations().find(request.request_id);
        const bool restored_reservation=allocated!=allocations.reservations().end() &&
            allocated->second.previously_included && !allocated->second.inclusion;
        // A canonical disconnect must restore its exact reservation even when
        // later accepted requests have consumed the admission capacity. This
        // exception requires replayed Included -> Disconnected ownership.
        // Live enqueue still counts ALL restored reservations and blocks new
        // requests above the unchanged caps. Old-format cap checks are exact.
        if (active && !restored_reservation) {
            auto& total=outstanding[request.account];
            const auto cap=state.config.withdrawal_caps.per_account_outstanding;
            Require(total<=cap && request.amount<=cap-total);
            total+=request.amount;
            Require(depth<static_cast<size_t>(state.config.withdrawal_caps.global_queue_depth));
            ++depth;
        }
        if(allocations.reservations().contains(request.request_id))continue;
        Require(!std::holds_alternative<WithdrawalPaymentConfirmed>(row.state));
        std::visit([&](const auto& value) {
            using T=std::decay_t<decltype(value)>;
            if constexpr(std::is_same_v<T,WithdrawalBroadcast> ||
                         std::is_same_v<T,WithdrawalSettledOnChain> ||
                         std::is_same_v<T,WithdrawalRevertedOnChain> ||
                         std::is_same_v<T,WithdrawalPaymentRetained>) {
                // Existing pre-broadcast reversion has a zero txid and no
                // ledger footprint. Preserve it as such; invent no payment.
                if constexpr(std::is_same_v<T,WithdrawalRevertedOnChain>) {
                    if (!Nonzero(value.txid)) return;
                }
                Require(Nonzero(value.txid));
                OutpointId outpoint;outpoint.txid_raw=value.txid;outpoint.vout=0;
                if constexpr(std::is_same_v<T,WithdrawalPaymentRetained>) {
                    Require(request.payment_terms.has_value() && Nonzero(value.body_sha256));
                    Require(value.fee_una<=request.payment_terms->maximum_fee_una);
                    outpoint.vout=value.vout;
                }
                Require(withdrawals.emplace(outpoint,WithdrawalBinding{&row}).second);
            }
        },row.state);
    }

    for (const auto& entry:state.entries) {
        std::visit([&](const auto& value) {
            using T=std::decay_t<decltype(value)>;
            if constexpr(std::is_same_v<T,PolicyAdjustment>) {
                // Keep the recorded account, signed amounts and note. The
                // actual replay engine owns their established interpretation.
                return;
            } else if constexpr(std::is_same_v<T,WithdrawalAllocationReserved> ||
                                std::is_same_v<T,WithdrawalAllocationDispatchStarted> ||
                                std::is_same_v<T,WithdrawalAllocationPaymentBound> ||
                                std::is_same_v<T,WithdrawalAllocationIncluded> ||
                                std::is_same_v<T,WithdrawalAllocationDisconnected> ||
                                std::is_same_v<T,WithdrawalAllocationReleased>) {
                const auto found=requests.find(value.request);
                Require(found!=requests.end() && found->second->request.account==value.account);
                // Ledger replay checks the entire ordered lifecycle and each
                // exact origin. Final saved state is reconciled below.
            } else if constexpr(std::is_same_v<T,WithdrawalInitiated> ||
                                std::is_same_v<T,WithdrawalSettled> ||
                                std::is_same_v<T,WithdrawalReverted>) {
                auto found=withdrawals.find(value.request);
                Require(found!=withdrawals.end());
                auto& binding=found->second;
                Require(binding.saved->request.account==value.account);
                if constexpr(std::is_same_v<T,WithdrawalInitiated>) {
                    Require(binding.progress==0 && value.amount==binding.saved->request.amount);
                    binding.progress=1;
                } else if constexpr(std::is_same_v<T,WithdrawalSettled>) {
                    Require(binding.progress==1);binding.progress=2;
                } else {
                    Require(binding.progress==1);binding.progress=3;
                }
            } else {
                auto found=deposits.find(value.deposit);
                Require(found!=deposits.end());
                auto& binding=found->second;
                Require(binding.saved->deposit.account==value.account);
                if constexpr(std::is_same_v<T,DepositObserved>) {
                    Require(binding.progress==0 && value.amount==binding.saved->deposit.amount);
                    binding.progress=1;
                } else if constexpr(std::is_same_v<T,CreditOpened>) {
                    Require(binding.progress==1 && value.amount==binding.saved->deposit.amount);
                    binding.progress=2;
                } else if constexpr(std::is_same_v<T,CreditPositionMatured> ||
                                    std::is_same_v<T,CreditPositionReverted> ||
                                    std::is_same_v<T,CreditPositionRestored>) {
                    const auto origin=allocations.positions().find(value.credit_seq);
                    Require(origin!=allocations.positions().end() &&
                            origin->second.account==value.account && origin->second.deposit==value.deposit);
                    if constexpr(std::is_same_v<T,CreditPositionMatured>) {
                        Require(binding.progress==2);binding.progress=3;
                    } else if constexpr(std::is_same_v<T,CreditPositionReverted>) {
                        Require(binding.progress==2 || binding.progress==3);
                        binding.progress=4;binding.compensated=true;
                    } else {
                        Require(binding.progress==4 && binding.compensated);
                        binding.progress=3;binding.compensated=false;
                    }
                } else if constexpr(std::is_same_v<T,CreditSettled>) {
                    Require(binding.progress==2);binding.progress=3;
                } else if constexpr(std::is_same_v<T,CreditReverted>) {
                    Require(binding.progress==2 || binding.progress==3);binding.progress=4;
                    binding.reversalSeq=value.seq;binding.compensated=false;
                } else if constexpr(std::is_same_v<T,CompensatingDebit>) {
                    Require(binding.progress==4 && !binding.compensated &&
                            value.amount==binding.saved->deposit.amount);
                    binding.compensated=true;binding.compensationSeq=value.seq;
                } else if constexpr(std::is_same_v<T,CreditReinstated>) {
                    Require(binding.progress==4 && binding.compensated &&
                            value.reversalSeq==binding.reversalSeq &&
                            value.compensationSeq==binding.compensationSeq);
                    binding.progress=3;binding.compensated=false;
                }
            }
        },entry);
    }
    for (const auto& [outpoint,binding]:deposits) {
        switch (binding.saved->deposit.stage) {
            case DepositStage::DETECTED:Require(binding.progress==0);break;
            case DepositStage::OBSERVED:Require(binding.progress==1);break;
            case DepositStage::CREDITED:Require(binding.progress==2);break;
            case DepositStage::SETTLED:Require(binding.progress==3);break;
            case DepositStage::REVERTED:
                Require(binding.progress<=1 || (binding.progress==4 && binding.compensated));break;
            default:Require(false);
        }
    }
    for (const auto& [outpoint,binding]:withdrawals) {
        if (std::holds_alternative<WithdrawalBroadcast>(binding.saved->state) ||
            std::holds_alternative<WithdrawalPaymentRetained>(binding.saved->state))
            Require(binding.progress==1);
        else if (std::holds_alternative<WithdrawalSettledOnChain>(binding.saved->state))
            Require(binding.progress==2);
        else Require(binding.progress==3);
    }
    for(const auto& [id,reservation]:allocations.reservations()) {
        const auto found=requests.find(id);Require(found!=requests.end());
        const auto& row=*found->second;const auto& request=row.request;
        Require(request.account==reservation.account && request.amount==reservation.amount &&
                request.payment_terms.has_value());
        ValidateWithdrawalPaymentTerms(*request.payment_terms);
        if(reservation.released) {
            Require(!reservation.dispatch_started && !reservation.payment && !reservation.inclusion &&
                    std::holds_alternative<WithdrawalFailed>(row.state));
        } else if(!reservation.dispatch_started) {
            Require(!reservation.payment && !reservation.inclusion && std::holds_alternative<WithdrawalPending>(row.state));
        } else if(!reservation.payment) {
            Require(!reservation.inclusion && std::holds_alternative<WithdrawalSigning>(row.state));
        } else {
            const WithdrawalPaymentRetained* payment=nullptr;
            if(reservation.inclusion) {
                const auto* confirmed=std::get_if<WithdrawalPaymentConfirmed>(&row.state);
                Require(confirmed && confirmed->inclusion==*reservation.inclusion);
                payment=&confirmed->payment;
            } else payment=std::get_if<WithdrawalPaymentRetained>(&row.state);
            Require(payment && Nonzero(payment->txid) && Nonzero(payment->body_sha256) &&
                    payment->fee_una<=request.payment_terms->maximum_fee_una &&
                    reservation.payment->output==OutpointId{payment->txid,payment->vout} &&
                    reservation.payment->body_hash==payment->body_sha256);
        }
    }
    for(const auto& [seq,position]:allocations.positions()) {
        const auto found=deposits.find(position.deposit);Require(found!=deposits.end());
        const auto& saved=found->second.saved->deposit;
        Require(saved.account==position.account && saved.amount==position.nominal);
        Require(saved.stage==(!position.active?DepositStage::REVERTED:
            position.stage==CreditAllocationState::Stage::Pending?DepositStage::CREDITED:DepositStage::SETTLED));
    }
    return replayed;
}
} // namespace dinero::vault
