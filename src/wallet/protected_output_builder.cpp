#include "wallet/protected_output_builder.h"
#include "wallet/bip32_deriver.h"
#include "consensus/coin_type.h"
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <stdexcept>

namespace dinero {

/// Extract 32-byte x-only pubkey from a BIP32 derivation at the given path
static std::vector<uint8_t> DeriveXOnlyPubkey(
    const std::vector<uint8_t>& seed,
    uint32_t coin_type,
    uint32_t chain,
    bool chain_hardened,
    uint32_t index) {

    BIP32Deriver deriver(seed.data(), seed.size());
    deriver.deriveHardened(86);
    deriver.deriveHardened(coin_type);
    deriver.deriveHardened(0);

    if (chain_hardened) {
        deriver.deriveHardened(chain);
    } else {
        deriver.deriveNormal(chain);
    }
    deriver.deriveNormal(index);

    auto arr = deriver.getXOnlyPubkey();
    return std::vector<uint8_t>(arr.begin(), arr.end());
}

ProtectedOutputBuilder::ProtectedOutput ProtectedOutputBuilder::Build(
    const HDWallet& wallet,
    const SafetyProfile& profile,
    uint32_t key_index) {

    auto seed = wallet.GetSeed();
    if (seed.size() < 64) {
        throw std::runtime_error("ProtectedOutputBuilder: seed too short");
    }

    const uint32_t coin_type = dinero::consensus::DINERO_COIN_TYPE;

    // Derive keys
    // User key: standard receive chain (0, non-hardened)
    auto user_pubkey = DeriveXOnlyPubkey(seed, coin_type, 0, false, key_index);

    // Panic key: chain 100 (hardened)
    auto panic_pubkey = DeriveXOnlyPubkey(
        seed, coin_type, profile.panic_key_chain, true, key_index);

    // Recovery key: chain 101 (hardened)
    auto recovery_pubkey = DeriveXOnlyPubkey(
        seed, coin_type, profile.recovery_key_chain, true, key_index);

    // Build template params
    ProtectedTemplateParams params;
    params.user_pubkey = user_pubkey;
    params.panic_pubkey = panic_pubkey;
    params.panic_window_blocks = profile.panic_window_blocks;
    params.recovery_pubkey = recovery_pubkey;
    params.recovery_delay_blocks = profile.recovery_delay_blocks;

    // Build the Taproot tree
    auto tree = TaprootTemplateBuilder::BuildProtected(params);

    // Zeroize seed copy
    std::fill(seed.begin(), seed.end(), 0);

    ProtectedOutput result;
    result.tree = std::move(tree);
    result.key_index = key_index;
    result.user_pubkey = std::move(user_pubkey);
    result.panic_pubkey = std::move(panic_pubkey);
    result.recovery_pubkey = std::move(recovery_pubkey);
    return result;
}

} // namespace dinero
