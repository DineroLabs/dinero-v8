/**
 * Header Proof-of-Work Verification Test
 *
 * Regression for the header-PoW gap: HeaderChainSelector::ValidateHeader used to
 * accept headers without checking hash <= target. Because fork-choice credits
 * chainwork = GetBlockProof(header.difficulty) from the *claimed* bits, a peer
 * could submit headers claiming arbitrarily hard difficulty with NO real work,
 * win UpdateBestHeader(), and steer block download toward a forged chain — a
 * zero-cost sync-stall / eclipse vector. Full blocks are still rejected at
 * connect (block_acceptor), so impact is availability, not theft.
 *
 * This test runs on MAINNET (where PoW enforcement is active; regtest skips it,
 * matching block_acceptor's PATH A) and asserts:
 *   1. The real genesis (valid PoW, nonce 813915426) is accepted.
 *   2. A child claiming hard difficulty with no real work is REJECTED.
 *   3. The forged header does NOT become the best header (no forged-chainwork
 *      takeover of fork-choice).
 */

#include "consensus/header_chain.h"
#include "consensus/chainparams.h"
#include "consensus/genesis_canonical.h"
#include "primitives/block.h"
#include "primitives/uint256.h"
#include <iostream>
#include <cassert>

using namespace dinero;
using namespace dinero::consensus;

int main() {
    // Mainnet: header PoW enforcement is active (regtest would skip it).
    SelectParams(Chain::MAINNET);
    std::cout << "=== Header PoW Verification Test (mainnet) ===" << std::endl;

    HeaderChainSelector selector;

    // ------------------------------------------------------------------
    // 1. The real genesis carries valid PoW and MUST be accepted.
    //    (It flows through ValidateHeader at startup seed time, so a
    //     too-strict PoW check here would brick the node — verify it passes.)
    // ------------------------------------------------------------------
    BlockHeader genesis = BuildCanonicalGenesis(Params()).header;
    bool genesis_ok = selector.AddHeader(genesis);
    assert(genesis_ok && "real genesis (valid PoW) must be accepted");
    std::cout << "  [1] real genesis accepted: OK" << std::endl;

    const HeaderIndexEntry* best_after_genesis = selector.GetBestHeader();
    assert(best_after_genesis != nullptr &&
           best_after_genesis->hash == genesis.GetHash());

    // ------------------------------------------------------------------
    // 2. Forged child: claims a HARDER target than genesis (0x1d00ffff →
    //    large GetBlockProof chainwork) but is not mined (nonce=1, arbitrary
    //    content) so its hash will not meet that target. Must be rejected.
    // ------------------------------------------------------------------
    BlockHeader forged;
    forged.version = 1;
    forged.prev_block_hash = genesis.GetHash();
    forged.merkle_root = uint256();
    forged.utreexo_root = uint256();
    forged.timestamp = genesis.timestamp + 120;  // > parent median-time-past
    forged.difficulty = 0x1d00ffff;              // harder than genesis (0x1d31ffce)
    forged.nonce = 1;                            // not mined → hash won't meet target

    bool forged_added = selector.AddHeader(forged);
    assert(!forged_added &&
           "forged hard-difficulty header with no PoW must be rejected");
    std::cout << "  [2] forged no-PoW child rejected: OK" << std::endl;

    // ------------------------------------------------------------------
    // 3. Best header must remain genesis — the forged header (which claimed
    //    huge chainwork) did NOT win fork-choice.
    // ------------------------------------------------------------------
    const HeaderIndexEntry* best_after_forge = selector.GetBestHeader();
    assert(best_after_forge != nullptr &&
           best_after_forge->hash == genesis.GetHash() &&
           "forged header must not become best (no forged-chainwork takeover)");
    std::cout << "  [3] best header unchanged (no forged-chainwork takeover): OK"
              << std::endl;

    std::cout << "✅ Header PoW verification enforced" << std::endl;
    return 0;
}
