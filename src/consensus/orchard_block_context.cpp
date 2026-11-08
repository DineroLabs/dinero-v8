// SelectedOrchardBlockContext — moved VERBATIM from
// src/consensus/orchard_header.cpp (Task 6.3 follow-up, owner decision
// 2026-11-07: extract ONLY this pure function into a small consensus
// library so the wallet delivery libraries stop reaching the node-coupled
// header-validation library). The declaration stays in
// include/consensus/orchard_header.h; the ancestry and header-validation
// code stays in orchard_header.cpp.

#include "consensus/orchard_header.h"
#include "consensus/chainparams.h"
#include "consensus/orchard_profile.h"

#include <algorithm>
#include <limits>

namespace dinero::consensus {

std::optional<OrchardBlockContext> SelectedOrchardBlockContext(
    const BlockHeader& header, uint32_t height) {
    const auto& params = Params();
    if (!OrchardProfileConfigurationValid(params))
        throw OrchardHeaderLookupError("invalid selected Orchard activation configuration");
    if (!OrchardActiveForHeight(params, height)) return std::nullopt;
    if (height > uint32_t(INT32_MAX))
        throw OrchardHeaderLookupError("unsupported selected Orchard height");
    const uint8_t network = params.name == "mainnet" ? 0 :
        params.name == "testnet" ? 1 : params.name == "regtest" ? 2 : 0xff;
    uint256 genesis;
    if (network == 0xff || !uint256::FromHex(params.genesis_hash, genesis) || genesis.IsNull())
        throw OrchardHeaderLookupError("invalid selected Orchard network identity");
    OrchardBlockContext context;
    context.height = height;
    context.block_hash = header.GetHash();
    context.parent_hash = header.prev_block_hash;
    context.activation_height = params.orchard_activation_height;
    context.domain.network_code = network;
    context.domain.branch_id = params.orchard_branch_id;
    std::copy(genesis.begin(), genesis.end(), context.domain.genesis_wire.begin());
    return context;
}

} // namespace dinero::consensus
