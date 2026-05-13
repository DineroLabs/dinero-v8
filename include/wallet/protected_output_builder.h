#pragma once

#include "wallet/taproot_template_builder.h"
#include "wallet/safety_profile.h"
#include "wallet/hd_wallet.h"
#include <array>
#include <cstdint>
#include <vector>

namespace dinero {

/**
 * Builds PROTECTED Taproot outputs by combining:
 * - HDWallet key derivation (user, panic, recovery keys)
 * - TaprootTemplateBuilder::BuildProtected()
 * - SafetyProfile parameters
 *
 * Each protected output gets a unique key index (auto-incremented).
 */
class ProtectedOutputBuilder {
public:
    struct ProtectedOutput {
        TemplateTreeResult tree;
        uint32_t key_index;        // BIP32 index used for all three keys
        std::vector<uint8_t> user_pubkey;     // 32-byte x-only
        std::vector<uint8_t> panic_pubkey;    // 32-byte x-only
        std::vector<uint8_t> recovery_pubkey; // 32-byte x-only
    };

    /**
     * Build a protected output using the wallet's next key index.
     *
     * Derives keys at:
     *   user:     m/86'/1448'/0'/0/key_index (standard receive)
     *   panic:    m/86'/1448'/0'/100'/key_index
     *   recovery: m/86'/1448'/0'/101'/key_index
     *
     * @param wallet HD wallet for key derivation
     * @param profile Active safety profile
     * @param key_index Key derivation index
     * @return Built protected output with all tree data
     */
    static ProtectedOutput Build(
        const HDWallet& wallet,
        const SafetyProfile& profile,
        uint32_t key_index
    );
};

} // namespace dinero
