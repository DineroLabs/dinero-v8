// The built-in Bitcoin wallet's keys are the standard BIP84 ones: the test
// vectors published with BIP84, starting from its recovery words, through the
// wallet's own BIP39 and BIP32 code.
#include "wallet/swap/btc_wallet_keys.h"

#include "wallet/bip32_deriver.h"
#include "wallet/bip39.h"
#include "wallet/swap/btc_deposit.h"
#include "wallet/swap/swap_crypto.h"

#include <gtest/gtest.h>

using namespace dinero::swap;
using dinero::BIP32Deriver;
namespace bip39 = dinero::bip39;

namespace {

const char* const kWords =
    "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";

struct Secret {
    Bytes32 key{}, chain_code{};
};

// m/84'/coin'/0' of the BIP84 test words.
Secret AccountSecret(uint32_t coin) {
    std::vector<uint8_t> seed;
    if (!bip39::MnemonicToSeed(kWords, "", seed) || seed.size() != 64) throw std::runtime_error("seed");
    BIP32Deriver d(seed.data(), seed.size());
    d.deriveHardened(kBtcWalletPurpose);
    d.deriveHardened(coin);
    d.deriveHardened(0);
    return {d.getPrivateKey(), d.getChainCode()};
}

std::string Hex(const std::array<uint8_t, 33>& k) { return detail::ToHex(std::vector<uint8_t>(k.begin(), k.end())); }

}  // namespace

TEST(BtcWalletKeys, TheVectorsPublishedWithBip84) {
    const Secret s = AccountSecret(0);
    const BtcWalletAccount account = BtcWalletAccountFromSecret(s.key, s.chain_code);

    // Receive 0 and 1, change 0: public keys and addresses from the BIP.
    EXPECT_EQ(Hex(BtcWalletPubkey(account, 0, 0)), "0330d54fd0dd420a6e5f8d3624f5f3482cae350f79d5f0753bf5beef9c2d91af3c");
    EXPECT_EQ(BtcWalletAddress(account, 0, 0, "bc"), "bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu");
    EXPECT_EQ(Hex(BtcWalletPubkey(account, 0, 1)), "03e775fd51f0dfb8cd865d9ff1cca2a158cf651fe997fdc9fee9c1d3b5e995ea77");
    EXPECT_EQ(BtcWalletAddress(account, 0, 1, "bc"), "bc1qnjg0jd8228aq7egyzacy8cys3knf9xvrerkf9g");
    EXPECT_EQ(Hex(BtcWalletPubkey(account, 1, 0)), "03025324888e429ab8e3dbaf1f7802648b9cd01e9b418485c5fa4c1b9b5700e1a6");
    EXPECT_EQ(BtcWalletAddress(account, 1, 0, "bc"), "bc1q8c6fshw2dlwun7ekn9qwf37cu2rn755upcp6el");
}

TEST(BtcWalletKeys, ThePrivateKeyOfAnAddressSignsForIt) {
    // Public derivation (what a locked wallet does) and private derivation
    // (what signing does) must land on the same key, on both branches.
    for (uint32_t coin : {0u, 1u}) {
        const Secret s = AccountSecret(coin);
        const BtcWalletAccount account = BtcWalletAccountFromSecret(s.key, s.chain_code);
        for (uint32_t branch : {0u, 1u}) {
            for (uint32_t index : {0u, 1u, 19u, 20u, 1000u, 0x7fffffffu}) {
                const Bytes32 key = BtcWalletSecretKey(s.key, s.chain_code, branch, index);
                EXPECT_EQ(detail::CompressedOf(key), BtcWalletPubkey(account, branch, index)) << branch << "/" << index;
                EXPECT_EQ(BtcWalletScript(account, branch, index), BtcP2wpkhScript(detail::CompressedOf(key)));
            }
        }
    }
    // The wallet's own deriver agrees (non-hardened steps from the account).
    const Secret s = AccountSecret(0);
    BIP32Deriver d(s.key.data(), s.chain_code.data());
    d.deriveNormal(1);
    d.deriveNormal(7);
    EXPECT_EQ(d.getPrivateKey(), BtcWalletSecretKey(s.key, s.chain_code, 1, 7));
}

TEST(BtcWalletKeys, TestNetworksUseCoinTypeOneAndTheirOwnPrefix) {
    const Secret main = AccountSecret(0), test = AccountSecret(1);
    const auto a_main = BtcWalletAccountFromSecret(main.key, main.chain_code);
    const auto a_test = BtcWalletAccountFromSecret(test.key, test.chain_code);
    EXPECT_NE(a_main.pubkey, a_test.pubkey);
    EXPECT_EQ(BtcWalletAddress(a_test, 0, 0, "tb").rfind("tb1q", 0), 0u);
    EXPECT_EQ(BtcWalletAddress(a_test, 0, 0, "bcrt").rfind("bcrt1q", 0), 0u);
    // Addresses never repeat across branches or indexes.
    EXPECT_NE(BtcWalletAddress(a_main, 0, 0, "bc"), BtcWalletAddress(a_main, 1, 0, "bc"));
    EXPECT_NE(BtcWalletAddress(a_main, 0, 0, "bc"), BtcWalletAddress(a_main, 0, 1, "bc"));
}

TEST(BtcWalletKeys, RefusesWhatIsNotAWalletAddress) {
    const Secret s = AccountSecret(0);
    const auto account = BtcWalletAccountFromSecret(s.key, s.chain_code);
    EXPECT_THROW(BtcWalletPubkey(account, 2, 0), std::invalid_argument);            // no such branch
    EXPECT_THROW(BtcWalletPubkey(account, 0, 0x80000000u), std::invalid_argument);  // hardened: needs the private key
    EXPECT_THROW(BtcWalletSecretKey(s.key, s.chain_code, 2, 0), std::invalid_argument);
    EXPECT_THROW(BtcWalletSecretKey(s.key, s.chain_code, 0, 0x80000000u), std::invalid_argument);
    EXPECT_THROW(BtcWalletAccountFromSecret(Bytes32{}, s.chain_code), std::invalid_argument);  // zero is not a key
}

TEST(BtcWalletKeys, TheAccountsPublicHalfSurvivesItsFile) {
    const Secret s = AccountSecret(0);
    const auto account = BtcWalletAccountFromSecret(s.key, s.chain_code);
    const auto bytes = EncodeBtcWalletAccount(account);
    ASSERT_EQ(bytes.size(), 65u);
    EXPECT_EQ(DecodeBtcWalletAccount(bytes), account);
    // Nothing of the private key is in it.
    const std::string hex = detail::ToHex(bytes);
    EXPECT_EQ(hex.find(detail::ToHex(std::vector<uint8_t>(s.key.begin(), s.key.end()))), std::string::npos);
    // Damage is refused, not turned into other addresses silently.
    auto bad = bytes;
    bad.pop_back();
    EXPECT_THROW(DecodeBtcWalletAccount(bad), std::invalid_argument);
    bad = bytes;
    bad[0] = 0x05;  // not a compressed-key prefix
    EXPECT_THROW(DecodeBtcWalletAccount(bad), std::invalid_argument);
}
