#pragma once
// The Bitcoin header chain on disk (docs/design/
// din-btc-swaps-without-bitcoin-core.md, section 3.2).
//
// One file: a short label naming the network and the checkpoint the headers
// start from, then the best chain's headers, 80 bytes each. It is written
// whole, to a temporary file that is synced and renamed over the old one, so a
// crash leaves the old file or the new one.
//
// The file is a cache, not a source of truth. Loading replays every header
// through the chain's own checks, so a damaged or tampered file can shorten
// what is known (the rest is fetched again) but cannot add a header the chain
// would refuse. A file for another network or checkpoint is ignored.

#include "wallet/swap/btc_header_chain.h"

#include <string>

namespace dinero::swap {

// Throws std::runtime_error if the file cannot be written.
void SaveBtcHeaderChain(const std::string& path, const BtcHeaderChain& chain);

// Headers accepted from the file (0 if it is missing, foreign or unreadable).
size_t LoadBtcHeaderChain(const std::string& path, BtcHeaderChain& chain, uint32_t now_unix);

}  // namespace dinero::swap
