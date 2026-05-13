#include "bip32_slip132.hpp"
#include <cstring>
#include <string>
#include <cstdio>
#include "base58.hpp"

namespace dinero::bip32 {

NodeSer serialize_xpub(uint8_t depth, uint32_t parent_fpr, uint32_t child_num,
                       const uint8_t cc[32], const uint8_t pub33[33]) {
  NodeSer n{};
  n.data[0] = depth;
  // big-endian fields
  n.data[1] = (parent_fpr>>24)&0xff; n.data[2]=(parent_fpr>>16)&0xff; n.data[3]=(parent_fpr>>8)&0xff; n.data[4]=parent_fpr&0xff;
  n.data[5] = (child_num>>24)&0xff;  n.data[6]=(child_num>>16)&0xff;  n.data[7]=(child_num>>8)&0xff;  n.data[8]=child_num&0xff;
  std::memcpy(&n.data[9], cc, 32);
  std::memcpy(&n.data[41], pub33, 33);
  return n;
}

static std::string enc_with_version(uint32_t ver, const NodeSer& n) {
  uint8_t v[4] = { (uint8_t)(ver>>24), (uint8_t)(ver>>16), (uint8_t)(ver>>8), (uint8_t)ver };
  return b58::encode_check(v, n.data, 74);  // 74-byte payload + 4-byte version = 78 bytes (BIP32 standard)
}

std::string to_xpub_mainnet(const NodeSer& n) { return enc_with_version(0x0488B21E, n); }
std::string to_tpub_testnet(const NodeSer& n) { return enc_with_version(0x043587CF, n); }
std::string to_zpub_mainnet(const NodeSer& n) { return enc_with_version(0x04B24746, n); }
std::string to_vpub_testnet(const NodeSer& n) { return enc_with_version(0x045F1CF6, n); }

std::string descriptor_wpkh(uint32_t master_fpr, int coin_type, int account, const std::string& xpub) {
  char buf[128];
  // origin: [fpr/84'/coin'/acct']
  std::snprintf(buf, sizeof(buf), "%08x/84'/%d'/%d'", master_fpr, coin_type, account);
  return std::string("wpkh([") + buf + "]" + xpub + "/0/*)";
}

} // namespace dinero::bip32
