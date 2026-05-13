// tools/verify_premine_seed.cpp
// Verify that a BIP39 mnemonic produces the canonical premine address
// via BIP86 m/86'/1447'/0'/0/0 (Taproot)
//
// Build: cmake --build build --target verify_premine_seed
// Run:   ./build/verify_premine_seed

#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>
#include <cstring>

#include "crypto/hd_keychain.h"
#include "consensus/premine_constants.h"

static std::string bytesToHex(const uint8_t* data, size_t len) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; ++i)
        oss << std::setw(2) << static_cast<unsigned>(data[i]);
    return oss.str();
}

int main(int argc, char** argv) {
    // ── Mnemonic ──
    std::string mnemonic;
    if (argc > 1) {
        // Accept mnemonic from command line (words joined by spaces)
        for (int i = 1; i < argc; ++i) {
            if (i > 1) mnemonic += " ";
            mnemonic += argv[i];
        }
    } else {
        mnemonic = "pink oil spell favorite point asset solve vehicle host alter clerk glide";
    }

    std::cout << "═══════════════════════════════════════════════════════════\n";
    std::cout << "  Verify Premine Seed → Address (BIP86 Taproot)\n";
    std::cout << "═══════════════════════════════════════════════════════════\n\n";

    std::cout << "Mnemonic:\n  " << mnemonic << "\n\n";

    // ── Derive master key from mnemonic ──
    auto master = dinero::crypto::HDKeychain::fromMnemonic(mnemonic, "");

    // ── BIP86: m/86'/1447'/0' (account key) ──
    const uint32_t COIN_TYPE = 1447;
    auto account = dinero::crypto::HDKeychain::getBIP86Account(master, COIN_TYPE, 0);

    std::cout << "Derivation path: m/86'/1447'/0'/0/0\n\n";

    // ── Derive first receive address: chain=0, index=0 ──
    auto external_chain = account.derive(0);  // chain 0 = external
    auto addr_key = external_chain.derive(0); // index 0

    // ── Get compressed pubkey ──
    auto pubkey = addr_key.getPublicKey();
    std::cout << "Public Key (compressed): " << bytesToHex(pubkey.data(), pubkey.size()) << "\n";

    // ── Get x-only pubkey ──
    auto xonly = addr_key.getXOnlyPubkey();
    std::cout << "X-Only Pubkey:           " << bytesToHex(xonly.data(), xonly.size()) << "\n\n";

    // ── Derive Taproot address ──
    std::string taproot_addr = addr_key.getTaprootAddress("din");
    std::cout << "Derived Taproot address:\n  " << taproot_addr << "\n\n";

    // ── Compare with premine constant ──
    std::string expected = dinero::premine::PREMINE_ADDRESS;
    std::cout << "Expected premine address:\n  " << expected << "\n\n";

    bool match = (taproot_addr == expected);
    std::cout << "═══════════════════════════════════════════════════════════\n";
    if (match) {
        std::cout << "  MATCH — This mnemonic controls the premine.\n";
    } else {
        std::cout << "  MISMATCH — This mnemonic does NOT produce the premine address.\n";
    }
    std::cout << "═══════════════════════════════════════════════════════════\n\n";

    // ── Also show first 5 addresses for reference ──
    std::cout << "First 5 receive addresses (BIP86 m/86'/1447'/0'/0/*):\n";
    for (uint32_t i = 0; i < 5; ++i) {
        auto chain_key = account.derive(0);
        auto key_i = chain_key.derive(i);
        std::cout << "  [" << i << "] " << key_i.getTaprootAddress("din") << "\n";
    }
    std::cout << "\n";

    // ── Also show first change address ──
    auto change_chain = account.derive(1);  // chain 1 = internal
    auto change_key = change_chain.derive(0);
    std::cout << "First change address (m/86'/1447'/0'/1/0):\n";
    std::cout << "  " << change_key.getTaprootAddress("din") << "\n\n";

    return match ? 0 : 1;
}
