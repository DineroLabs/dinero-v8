#include "consensus/orchard_header.h"
#include "consensus/header_chain.h"
#include "consensus/pow.h"
#include "consensus/orchard_profile.h"
#include "consensus/pow_context.h"
#include <algorithm>
#include <limits>

namespace dinero::consensus {
namespace {
using Error = OrchardHeaderErrorCode;
[[noreturn]] void Reject(Error code) { throw OrchardHeaderError(code); }
}
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
void CheckOrchardHeaderUnderChainstateLock(
    const BlockHeader& header, const BlockHeader& parent,
    const OrchardBlockContext& context, const HeaderChainSelector& headers,
    uint64_t now_seconds) {
    const auto& params = Params();
    const uint8_t network = params.name == "mainnet" ? 0 :
        params.name == "testnet" ? 1 : params.name == "regtest" ? 2 : 0xff;
    uint256 genesis;
    if (network == 0xff || !uint256::FromHex(params.genesis_hash, genesis) || genesis.IsNull() ||
        now_seconds == 0 || now_seconds > static_cast<uint64_t>(INT64_MAX) - 7200)
        throw OrchardHeaderLookupError("invalid selected Orchard header configuration");
    const auto selected = SelectedOrchardBlockContext(header, context.height);
    if (!selected || context.activation_height != selected->activation_height ||
        context.domain.branch_id != selected->domain.branch_id) Reject(Error::Context);
    if (context.domain.network_code != network || context.domain.branch_id == 0 ||
        !std::equal(genesis.begin(), genesis.end(), context.domain.genesis_wire.begin()) ||
        context.height == 0 || context.height > static_cast<uint32_t>(INT32_MAX) ||
        context.activation_height == 0 || context.activation_height == UINT32_MAX ||
        context.height < context.activation_height || context.block_hash != header.GetHash() ||
        context.parent_hash != parent.GetHash() || header.prev_block_hash != context.parent_hash)
        Reject(Error::Context);
    if (header.version == 0 || !header.IsReservedValid() || header.difficulty == 0)
        Reject(Error::Shape);
    if (header.timestamp == 0) Reject(Error::TimeTooOld);
    // Subtract only after the comparison: no overflow even for UINT64_MAX.
    if (header.timestamp > now_seconds && header.timestamp - now_seconds > 7200)
        Reject(Error::TimeTooNew);

    const auto consensus = GetConsensusForCurrentNetwork();
    const auto anchor_height = TimingUpgradeAnchorHeight(context.height, consensus);
    const auto indexed_parent = headers.GetHeaderValue(context.parent_hash);
    HeaderAsertContext ancestry;
    if (!indexed_parent || indexed_parent->height != context.height - 1 ||
        indexed_parent->header.SerializeForHash() != parent.SerializeForHash() ||
        !headers.GetAsertContextByHash(context.parent_hash, ancestry, anchor_height) ||
        ancestry.parent_height != context.height - 1 || ancestry.parent_mtp <= 0)
        throw OrchardHeaderLookupError("Orchard selected header ancestry unavailable");
    if (header.timestamp <= static_cast<uint64_t>(ancestry.parent_mtp))
        Reject(Error::TimeTooOld);
    // Check the candidate's own ancestry. A checkpoint on some best-header
    // branch is not evidence for this selected parent's branch. Future
    // checkpoints do not prevent initial sync toward them.
    for (const auto& [height, encoded] : params.vCheckpoints) {
        if (height > context.height) break;
        uint256 expected;
        if (!uint256::FromHex(encoded, expected) || expected.IsNull())
            throw OrchardHeaderLookupError("invalid Orchard checkpoint configuration");
        uint256 actual = context.block_hash;
        if (height < context.height) {
            uint32_t parent_height = 0;
            if (!headers.GetAncestorHashByHash(context.parent_hash, height, actual, parent_height) ||
                parent_height != context.height - 1)
                throw OrchardHeaderLookupError("Orchard checkpoint ancestry unavailable");
        }
        if (actual != expected) Reject(Error::Checkpoint);
    }
    if (params.SkipProofOfWork()) return;
    if (context.height > 1 && !ancestry.timing_anchor && ancestry.block1_time <= 0)
        throw OrchardHeaderLookupError("Orchard ASERT reference unavailable");
    const auto input = BuildAsertInputForCandidateTimes(
        ancestry.parent_mtp, ancestry.block1_time, static_cast<NoChainDb*>(nullptr),
        static_cast<int32_t>(context.height), static_cast<int64_t>(header.timestamp),
        consensus, ancestry.timing_anchor);
    if (!input) throw OrchardHeaderLookupError("Orchard ASERT context incomplete");
    const auto expected = ComputeAsertBits(*input);
    if (expected == 0) throw OrchardHeaderLookupError("Orchard ASERT result unavailable");
    if (header.difficulty != expected) Reject(Error::Difficulty);
    // Exact ASERT bits, not the historical minimum-difficulty policy floor.
    if (!CheckProofOfWork(header, false)) Reject(Error::ProofOfWork);
}
} // namespace dinero::consensus
