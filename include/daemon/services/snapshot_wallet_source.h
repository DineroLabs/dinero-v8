#pragma once
#include "primitives/block.h"
#include "wallet/snapshot_wallet_types.h"  // SnapshotWalletCoin / SnapshotWalletSourceData (moved, Task 5.4)
#include <istream>
#include <vector>

namespace dinero {
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
