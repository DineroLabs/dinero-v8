#pragma once
#include "primitives/block.h"
#include <istream>
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
// Reads only the transparent record and forest sections. Rebuilds and checks
// the forest, then requires one-to-one coverage of every live leaf. Other
// snapshot sections are not imported or qualified. Throws before any effects.
SnapshotWalletSourceData ReadSnapshotWalletSource(std::istream&,
    const BlockHeader& expected_base,uint32_t expected_height,const Block& genesis);
}
namespace dinero {
// POSIX regular-file, no-follow reader for phone maintenance. Windows callers
// refuse; the stream verifier above remains portable for fixtures/tools.
SnapshotWalletSourceData ReadSnapshotWalletSourceFile(const std::string&,
    const BlockHeader&,uint32_t,const Block&);
}
