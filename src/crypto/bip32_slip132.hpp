#pragma once
#include <cstdint>
#include <string>
#include <span>

namespace dinero::bip32 {

// 74-byte serialized node payload (no version, no checksum):
// depth(1) | parent_fpr(4) | child_num(4) | chain_code(32) | keydata(33)
// Version (4 bytes) is prepended during Base58Check encoding to make 78 bytes total
struct NodeSer {
  uint8_t data[74];  // BIP32 payload WITHOUT version bytes
};

// Serialize XPUB (public) node; pubkey33 must be compressed
NodeSer serialize_xpub(uint8_t depth, uint32_t parent_fpr, uint32_t child_num,
                       const uint8_t chain_code[32], const uint8_t pubkey33[33]);

// Base58Check encoders for versions
// mainnet: xpub 0x0488B21E, zpub 0x04B24746
// testnet: tpub 0x043587CF, vpub 0x045F1CF6
std::string to_xpub_mainnet(const NodeSer& n);
std::string to_tpub_testnet(const NodeSer& n);
std::string to_zpub_mainnet(const NodeSer& n); // SLIP-0132 (P2WPKH)
std::string to_vpub_testnet(const NodeSer& n);

// Descriptor (importable by many wallets): wpkh([fpr/84'/coin'/acct']xpub/0/*)
std::string descriptor_wpkh(uint32_t master_fpr, int coin_type, int account, const std::string& xpub);

} // namespace dinero::bip32
