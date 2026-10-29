#pragma once
// Zcash transactions for the ZEC side of a DIN <-> ZEC swap (milestone 3 of
// docs/design/din-zec-atomic-swaps.md).
//
// v5 serialization, the ZIP 244 txid and SIGHASH_ALL, and t-address encoding
// come from librustzcash through rust/zcash_swap; nothing here hashes or
// serializes a Zcash transaction by hand. This file builds the P2SH HTLC
// spends and their scriptSigs. Callers sign the returned sighash with ECDSA
// (DER) and append SIGHASH_ALL (0x01), as on the Bitcoin side.
//
// Until the crate folds into rust/orchard_backend (v8.2.1), only tests link
// this file.

#include "wallet/swap/htlc.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dinero::swap {

struct ZecTxIn {
    std::array<uint8_t, 32> prev_txid{};  // wire byte order
    uint32_t prev_vout{};
    uint32_t sequence{0xffffffff};
    std::vector<uint8_t> script_sig;
};

struct ZecTxOut {
    uint64_t value_zat{};
    std::vector<uint8_t> script_pubkey;
};

// A v5 transaction with a transparent bundle only.
struct ZecTx {
    uint32_t consensus_branch_id{};  // signed into every input (ZIP 244)
    uint32_t locktime{0};
    uint32_t expiry_height{0};       // 0 = never expires (ZIP 203)
    std::vector<ZecTxIn> vin;
    std::vector<ZecTxOut> vout;
};

// All throw std::invalid_argument on bad input.
std::vector<uint8_t> SerializeZecTx(const ZecTx& tx);
Bytes32 ZecTxid(const std::vector<uint8_t>& tx_bytes);  // wire byte order
Bytes32 ZecTxid(const ZecTx& tx);

// ZIP 244 SIGHASH_ALL for input `index`; `prevouts` are the outputs spent by
// every input, in input order.
Bytes32 Zip244SighashAll(const std::vector<uint8_t>& tx_bytes, size_t index,
                         const std::vector<ZecTxOut>& prevouts);

// --- HTLC ------------------------------------------------------------------

// The ZEC lock uses the Bitcoin HTLC script unchanged, wrapped in P2SH.
using ZecHtlcTerms = BtcHtlcTerms;
std::vector<uint8_t> BuildZecHtlcRedeemScript(const ZecHtlcTerms& terms);
std::vector<uint8_t> ZecP2shScriptPubKey(const std::vector<uint8_t>& redeem_script);

// The ZEC HTLC output being spent.
struct ZecFunding {
    std::array<uint8_t, 32> txid{};  // wire byte order
    uint32_t vout{};
    uint64_t value_zat{};
};

// Unsigned spends: one input, one output paying funding - fee.
//   Claim:  nLockTime 0, final sequence.
//   Refund: nLockTime = the refund lock, sequence 0xfffffffe so CLTV applies.
// expiry_height is tip + 40 for spends the wallet can re-sign and 0 for a
// pre-signed tower rung. Throw std::invalid_argument when the fee is not below
// the funding value or the payout script is empty.
ZecTx BuildZecClaimTx(const ZecFunding& funding, const std::vector<uint8_t>& payout_script,
                      uint64_t fee_zat, uint32_t consensus_branch_id, uint32_t expiry_height);
ZecTx BuildZecRefundTx(const ZecHtlcTerms& terms, const ZecFunding& funding,
                       const std::vector<uint8_t>& payout_script, uint64_t fee_zat,
                       uint32_t consensus_branch_id, uint32_t expiry_height);

// Message to sign for either path.
Bytes32 ZecHtlcSighash(const ZecTx& tx, const ZecHtlcTerms& terms, const ZecFunding& funding);

// scriptSigs. `signature` is DER + the SIGHASH_ALL byte.
//   Claim:  <sig> <preimage> OP_TRUE <redeem script>; the preimage must hash to h.
//   Refund: <sig> OP_FALSE <redeem script>.
void SetZecClaimScriptSig(ZecTx& tx, const ZecHtlcTerms& terms, const std::vector<uint8_t>& signature,
                          const std::vector<uint8_t>& preimage);
void SetZecRefundScriptSig(ZecTx& tx, const ZecHtlcTerms& terms,
                           const std::vector<uint8_t>& signature);

// --- t-addresses -----------------------------------------------------------

enum class ZecNetwork : uint8_t { Main = 0, Test = 1, Regtest = 2 };
enum class ZecAddressKind : uint8_t { P2pkh = 0, P2sh = 1 };

struct ZecTAddress {
    ZecNetwork network{ZecNetwork::Main};
    ZecAddressKind kind{ZecAddressKind::P2pkh};
    std::array<uint8_t, 20> hash{};
};

std::string EncodeZecTAddress(const ZecTAddress& address);
// Regtest addresses encode like testnet ones, so they decode as Test.
ZecTAddress DecodeZecTAddress(const std::string& text);
std::vector<uint8_t> ZecScriptPubKey(const ZecTAddress& address);

}  // namespace dinero::swap
