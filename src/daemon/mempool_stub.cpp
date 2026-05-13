// Minimal Mempool stub for reindex-only builds
// Phase M.0 migration pending - full implementation in mempool.cpp
// This file provides minimal symbols to satisfy the linker only

#include "primitives/transaction.h"
#include "primitives/uint256.h"
#include "consensus/outpoint.h"
#include <vector>
#include <string>
#include <stdexcept>
#include <functional>
#include <chrono>

namespace dinero {

// Forward declarations
struct Transaction;
class ChainDB;
// OutPoint is defined in consensus/outpoint.h in dinero namespace (already included)

// Minimal MempoolEntry struct at namespace level to match real interface
struct MempoolEntry {
    Transaction tx;
    uint64_t fee{0};
    double fee_rate{0.0};
    std::chrono::time_point<std::chrono::steady_clock> time;
    uint32_t height{0};
    size_t tx_size{0};
    std::vector<uint256> depends;
    std::vector<OutPoint> spends;
    uint64_t ancestor_fee{0};
    size_t ancestor_size{0};
    double ancestor_feerate{0.0};
    bool is_confidential{false};
    size_t total_proof_bytes{0};
    double adjusted_fee_rate{0.0};
};

// Minimal Mempool class definition for stub
class Mempool {
public:
    // Nested Stats struct to match real Mempool interface
    struct Stats {
        size_t count = 0;
        size_t size_bytes = 0;
        uint64_t total_fees = 0;
        double min_fee_rate = 0.0;
        double max_fee_rate = 0.0;
        double median_fee_rate = 0.0;
    };

    Mempool(ChainDB*);
    Mempool(ChainDB*, int);
    ~Mempool();

    // Transaction management
    bool addTransaction(const Transaction&, bool);
    bool submitTransactionTestOnly(const Transaction&, std::string&);
    bool hasTransaction(const uint256&) const;
    Transaction getTransaction(const uint256&) const;
    std::vector<Transaction> getTransactions() const;
    std::vector<uint256> getTransactionIds() const;

    // Mempool entry management
    MempoolEntry getMempoolEntry(const uint256&) const;

    // Stats and queries
    Stats getStats() const;
    size_t size() const;
    uint64_t getTotalSize() const;
    uint64_t getTotalFees() const;
    uint64_t getTransactionFee(const uint256&) const;
    double getTransactionFeeRate(const uint256&) const;

    // Block assembly
    std::vector<Transaction> selectTransactionsForBlock(size_t, uint64_t, uint32_t) const;
    std::vector<Transaction> getTransactionsByFeeRate(size_t) const;

    // UTXO tracking (uses dinero::OutPoint from consensus/outpoint.h)
    bool isOutputSpentInMempool(const OutPoint&) const;

    // Persistence
    std::string getDefaultMempoolPath() const;
    void loadFromDisk(const std::string&);
    void saveToDisk(const std::string&);

    // Clear
    void clear();

    // Iteration (for transaction_scorer.cpp / FeeHistogram)
    void forEachEntry(const std::function<void(const MempoolEntry&)>& fn) const;
};

// Method implementations
Mempool::Mempool(ChainDB*) {}
Mempool::Mempool(ChainDB*, int) {}
Mempool::~Mempool() {}

bool Mempool::addTransaction(const Transaction&, bool) { throw std::runtime_error("Mempool not available"); }
bool Mempool::submitTransactionTestOnly(const Transaction&, std::string&) { throw std::runtime_error("Mempool not available"); }
bool Mempool::hasTransaction(const uint256&) const { return false; }
Transaction Mempool::getTransaction(const uint256&) const { throw std::runtime_error("Mempool not available"); }
std::vector<Transaction> Mempool::getTransactions() const { return {}; }
std::vector<uint256> Mempool::getTransactionIds() const { return {}; }

MempoolEntry Mempool::getMempoolEntry(const uint256&) const { throw std::runtime_error("Mempool not available"); }

Mempool::Stats Mempool::getStats() const { return {}; }
size_t Mempool::size() const { return 0; }
uint64_t Mempool::getTotalSize() const { return 0; }
uint64_t Mempool::getTotalFees() const { return 0; }
uint64_t Mempool::getTransactionFee(const uint256&) const { return 0; }
double Mempool::getTransactionFeeRate(const uint256&) const { return 0.0; }

std::vector<Transaction> Mempool::selectTransactionsForBlock(size_t, uint64_t, uint32_t) const { return {}; }
std::vector<Transaction> Mempool::getTransactionsByFeeRate(size_t) const { return {}; }

bool Mempool::isOutputSpentInMempool(const OutPoint&) const { return false; }

std::string Mempool::getDefaultMempoolPath() const { return "/tmp/mempool.dat"; }
void Mempool::loadFromDisk(const std::string&) {}
void Mempool::saveToDisk(const std::string&) {}

void Mempool::clear() {}

void Mempool::forEachEntry(const std::function<void(const MempoolEntry&)>& fn) const {
    // Stub: No entries to iterate
}

} // namespace dinero
