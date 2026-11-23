#pragma once
// The built-in Bitcoin wallet's keys (docs/design/multi-coin-wallet.md,
// section 5): the standard BIP84 account of the wallet's own seed, so the same
// recovery words find the same coins in any other Bitcoin wallet.
//
//   account   m/84'/coin'/0'      coin 0 on mainnet, 1 on every test network
//   receive   account/0/i         change   account/1/i
//   address   native SegWit (P2WPKH): bc1q…, tb1q…, bcrt1q…
//
// The account's public half (public key and chain code) is enough to
// calculate every address, so receiving and reading balances need no unlock.
// It cannot spend, but whoever reads it sees every address of the wallet.

#include "wallet/swap/htlc.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dinero::swap {

inline constexpr uint32_t kBtcWalletPurpose = 84;
inline constexpr uint32_t kBtcWalletGapLimit = 20;  // unused addresses looked at beyond the last used one

struct BtcWalletAccount {
    std::array<uint8_t, 33> pubkey{};
    Bytes32 chain_code{};
    bool operator==(const BtcWalletAccount& o) const { return pubkey == o.pubkey && chain_code == o.chain_code; }
};

// The account's public half from its private key and chain code (the node at
// m/84'/coin'/0'). Throws std::invalid_argument for a key that is not one.
BtcWalletAccount BtcWalletAccountFromSecret(const Bytes32& account_secret_key, const Bytes32& chain_code);

// The public key at account/branch/index (branch 0: receive, 1: change), by
// BIP32 public derivation. Throws std::invalid_argument for a hardened index,
// a branch other than 0 or 1, or the (2^-127) index BIP32 says to skip.
std::array<uint8_t, 33> BtcWalletPubkey(const BtcWalletAccount& account, uint32_t branch, uint32_t index);

// The same key's private half, from the account's private half: for signing.
Bytes32 BtcWalletSecretKey(const Bytes32& account_secret_key, const Bytes32& chain_code, uint32_t branch, uint32_t index);

std::vector<uint8_t> BtcWalletScript(const BtcWalletAccount& account, uint32_t branch, uint32_t index);
std::string BtcWalletAddress(const BtcWalletAccount& account, uint32_t branch, uint32_t index, const std::string& hrp);

// 65 bytes (public key, chain code) for the wallet's own file. Decode throws
// std::invalid_argument unless the bytes are a public key on the curve.
std::vector<uint8_t> EncodeBtcWalletAccount(const BtcWalletAccount& account);
BtcWalletAccount DecodeBtcWalletAccount(const std::vector<uint8_t>& bytes);

}  // namespace dinero::swap
