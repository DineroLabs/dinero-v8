#pragma once
// Compact shielded v1 uses the existing v6 transaction envelope. Validity
// is selected by the network's reviewed consensus activation parameters.
#include "consensus/shielded/shielded_tx.h"
#include "consensus/chainparams.h"

namespace dinero::consensus::shielded {
struct CompactShieldedRules {
    bool enabled = false;
    uint32_t activation_height = UINT32_MAX;
    constexpr bool Active(uint64_t height) const {
        return enabled && activation_height != UINT32_MAX && height >= activation_height;
    }
};

// Compact v1 expands only the recipient-authority spend and cv-bound output
// circuits. It must follow their existing resets; it does not create a reset
// or change the note/nullifier epoch. Fail closed even for locally constructed
// parameter copies that have not gone through SelectParams.
inline bool CompactActivationConfigurationValid(const ChainParams& params) {
    const auto height = params.shielded_compact_activation_height;
    if (height == UINT32_MAX) return true;
    return params.shielded_activation_height < height &&
           params.shielded_input_binding_activation_height < height &&
           params.shielded_cv_binding_activation_height < height &&
           params.shielded_spend_auth_activation_height < height &&
           params.shielded_epoch_reset_height == params.shielded_cv_binding_activation_height &&
           params.shielded_spend_auth_epoch_reset_height == params.shielded_spend_auth_activation_height &&
           params.shielded_input_binding_activation_height <= params.shielded_cv_binding_activation_height &&
           params.shielded_cv_binding_activation_height < params.shielded_spend_auth_activation_height;
}
inline CompactShieldedRules CompactRulesFor(const ChainParams& params) {
    return {CompactActivationConfigurationValid(params), params.shielded_compact_activation_height};
}

// DZE1 cannot be a valid historical Spartan proof: its first byte is not
// any historical proof-profile byte. Inspect actual proof fields, never scan
// arbitrary ciphertext/signature bytes for a magic substring. One tagged proof
// selects compact validation; expansion requires every proof to match its
// correct compact profile, so mixed/full/truncated bundles fail closed.
inline bool HasCompactProofs(const ShieldedBundle& bundle) {
    const auto tagged = [](const auto& proof) {
        return proof.size() >= 4 && proof[0] == 'D' && proof[1] == 'Z' &&
               proof[2] == 'E' && proof[3] == '1';
    };
    for (const auto& spend : bundle.spends) if (tagged(spend.zk_proof)) return true;
    for (const auto& output : bundle.outputs) if (tagged(output.zk_proof)) return true;
    return false;
}

// Transactional transforms: failure leaves the destination unchanged. Circuit
// selection is fixed by the ordinary Auth profile, never by wire dimensions.
bool PackCompactShieldedBundle(ShieldedBundle& bundle);
bool ExpandCompactShieldedBundle(const ShieldedBundle& bundle, ShieldedBundle& expanded);
} // namespace dinero::consensus::shielded
