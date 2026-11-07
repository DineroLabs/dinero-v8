// Snapshot wallet source DATA types (moved verbatim from
// include/daemon/services/snapshot_wallet_source.h, Task 5.4): the wallet
// consumes these values, so they live in a wallet header; the daemon keeps
// the readers that produce them.
#pragma once
#include "primitives/block.h"
#include <vector>

namespace dinero {
// Authenticated transparent source DATA for one caller-supplied base header.
// It is not selected-chain authority, wallet ownership, complete historical
// discovery, a delivery receipt or permission to write a wallet/index.
struct SnapshotWalletCoin {
    uint256 txid;
    uint32_t output=0;
    uint64_t amount=0;
    std::vector<uint8_t> script;
    uint32_t height=0;
    bool coinbase=false;
    // Legacy leaves do not commit creation height/coinbase. Those fields still
    // require a canonical creation-transaction proof before wallet use.
    bool maturity_bound=false;
};
struct SnapshotWalletSourceData {
    uint256 base;
    uint32_t height=0;
    std::vector<SnapshotWalletCoin> coins;
};
}
