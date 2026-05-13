#pragma once

#include "contracts/contract_state_db.h"
#include "contracts/commitment_builder.h"
#include "contracts/state_verifier.h"
#include <string>
#include <memory>
#include <optional>
#include <cstdint>
#include <chrono>

namespace dinero {
namespace contracts {

// Lending contract types
enum class LendingType {
    SIMPLE_INTEREST = 1,      // Fixed interest rate
    COMPOUND_INTEREST = 2,    // Compound interest
    COLLATERALIZED = 3        // Collateral-backed loan
};

// Lending contract status
enum class LendingStatus {
    PENDING = 1,              // Loan request pending
    ACTIVE = 2,               // Loan active, repayments ongoing
    OVERDUE = 3,              // Payment overdue
    REPAID = 4,               // Fully repaid
    DEFAULTED = 5,            // Defaulted (collateral seized)
    CANCELLED = 6             // Cancelled before activation
};

// Repayment schedule entry
struct RepaymentEntry {
    uint32_t payment_number;
    uint64_t due_date;        // Unix timestamp
    double principal_amount;
    double interest_amount;
    double total_amount;
    bool paid;
    std::string payment_txid;
    uint64_t paid_date;
    
    RepaymentEntry() : payment_number(0), due_date(0), 
                      principal_amount(0.0), interest_amount(0.0), 
                      total_amount(0.0), paid(false), paid_date(0) {}
};

// Lending contract manager with on-chain commitments
class LendingContractManager {
public:
    LendingContractManager(ContractStateDB& db);
    
    // Create lending contract
    std::optional<std::string> createLendingContract(
        const std::string& lender_address,
        const std::string& borrower_address,
        double principal_amount,
        double interest_rate,           // Annual percentage rate (e.g., 5.0 = 5%)
        uint32_t term_months,           // Loan term in months
        LendingType lending_type,
        const std::string& collateral_address = "",  // For collateralized loans
        double collateral_amount = 0.0
    );
    
    // Activate loan (funds transferred)
    bool activateLoan(
        const std::string& contract_id,
        const std::string& funding_txid
    );
    
    // Record payment
    bool recordPayment(
        const std::string& contract_id,
        uint32_t payment_number,
        const std::string& payment_txid,
        double amount_paid
    );
    
    // Calculate next payment due
    RepaymentEntry calculateNextPayment(const std::string& contract_id) const;
    
    // Get repayment schedule
    std::vector<RepaymentEntry> getRepaymentSchedule(const std::string& contract_id) const;
    
    // Check if loan is overdue
    bool isOverdue(const std::string& contract_id) const;
    
    // Update contract state
    bool updateLendingState(
        const std::string& contract_id,
        const std::string& new_status,
        const std::string& transitioned_by,
        const std::string& transition_data = ""
    );
    
    // Create commitment for current state
    std::optional<std::vector<uint8_t>> createCommitment(const std::string& contract_id);
    
    // Record commitment transaction
    bool recordCommitmentTransaction(
        const std::string& contract_id,
        const std::string& commitment_txid,
        uint32_t block_height = 0,
        const std::string& block_hash = ""
    );
    
    // Get lending contract state
    bool getLendingContract(const std::string& contract_id, ContractState& out) const;
    
    // Get contract history
    std::vector<StateHistoryEntry> getLendingHistory(const std::string& contract_id) const;
    
    // Verify contract state
    bool verifyLendingState(const std::string& contract_id) const;
    
    // Get verification report
    StateVerifier::VerificationReport getVerificationReport(const std::string& contract_id) const;
    
private:
    ContractStateDB& db_;
    std::unique_ptr<StateVerifier> verifier_;
    
    // Helper: Convert lending status to contract status
    ContractStatus lendingStatusToContractStatus(LendingStatus status) const;
    
    // Helper: Build contract data JSON
    std::string buildLendingContractData(
        const std::string& lender_address,
        const std::string& borrower_address,
        double principal_amount,
        double interest_rate,
        uint32_t term_months,
        LendingType lending_type,
        const std::string& collateral_address,
        double collateral_amount
    ) const;
    
    // Helper: Calculate state hash
    std::string calculateLendingStateHash(
        const std::string& contract_id,
        const std::string& status,
        const std::string& contract_data
    ) const;
    
    // Helper: Generate repayment schedule
    std::vector<RepaymentEntry> generateRepaymentSchedule(
        double principal,
        double annual_rate,
        uint32_t term_months,
        uint64_t start_date
    ) const;
    
    // Helper: Calculate simple interest payment
    double calculateSimpleInterestPayment(
        double principal,
        double annual_rate,
        uint32_t term_months
    ) const;
    
    // Helper: Calculate compound interest payment
    double calculateCompoundInterestPayment(
        double principal,
        double annual_rate,
        uint32_t term_months
    ) const;
};

}} // namespace dinero::contracts

