// Minimal stubs for missing symbols (test-only)
// These provide just enough implementation for the restart test to link

#include "consensus/undo.h"
#include "consensus/block_index.h"
#include "wallet/transaction.h"
#include <vector>
#include <set>
#include <string>

namespace dinero {

// NOTE: UndoRecord::Serialize() and UndoRecord::Deserialize() removed - now in src/consensus/undo.cpp
// NOTE: Transaction::Serialize() and Transaction::GetTxid() removed - now in src/wallet/transaction.cpp
// NOTE: MarkBlockInvalid() and g_invalid_blocks removed - now defined in src/consensus/block_lifecycle.cpp

// Mempool stubs
class Mempool {
public:
    bool addTransaction(const Transaction& /*tx*/, bool /*skip_validation*/) { return true; }
    void removeTransaction(const std::string& /*txid*/) {}
    bool hasTransaction(const std::string& /*txid*/) const { return false; }
};

} // namespace dinero
