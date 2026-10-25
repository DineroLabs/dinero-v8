#pragma once
#include "consensus/chainparams.h"
#include <sstream>

namespace dinero::consensus {
// Exact historical detached-validation fingerprint, shared with the selected
// consistency owner without importing transaction/prover headers into storage.
inline std::string OrchardValidationProfile(){
    const auto& p=Params();std::ostringstream out;
    out << ConsensusChecksum(p) << '\n' << p.name << '\n' << p.network_id << '\n'
        << p.genesis_hash << '\n' << p.orchard_activation_height << '\n'
        << p.orchard_branch_id << '\n' << p.release_v8113_activation_height << '\n'
        << p.orchard_catalog_commitment_activation_height << '\n'
        << p.max_block_size << '\n' << p.coinbase_maturity << '\n'
        << p.allow_min_difficulty << '\n' << p.require_standard_txs << '\n'
        << p.mine_blocks_on_demand << '\n' << p.regtest_enforce_pow << '\n'
        << p.enable_witness_magic_translation << '\n' << p.witness_magic_translation_height << '\n'
        << p.disable_confidential_transactions << '\n' << p.confidential_activation_height << '\n'
        << p.nMinimumChainWork << '\n' << p.defaultAssumeValid << '\n' << p.assumeValidHeight << '\n';
    for(const auto& [height,hash]:p.vCheckpoints)out << height << ':' << hash << '\n';
    return out.str();
}
} // namespace dinero::consensus
