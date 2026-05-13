#pragma once

// =============================================================================
// AUTO-GENERATED from genesis_bundle.json -- DO NOT EDIT
//
// Regenerate with:
//   python3 tools/gen_constants_from_bundle.py docs/chain/genesis_bundle.json \
//       > include/consensus/chain_bundle_generated.h
// =============================================================================

#include <cstdint>
#include <string>

namespace dinero {
namespace chain_bundle {

// ---------------------------------------------------------------------------
// Genesis block (height 0)
// ---------------------------------------------------------------------------

static constexpr int          GENESIS_HEIGHT       = 0;
static constexpr int          GENESIS_VERSION      = 1;
static constexpr const char*  GENESIS_BLOCK_HASH   = "0000001c36abf27e2c233ff40ed0c08888926c24450f3bff82a047ae1528b76f";
static constexpr const char*  GENESIS_MERKLE_ROOT  = "b040dae24ff59ae0c875252eec15722ec30ee8d600907eda63249800fa6be364";
static constexpr const char*  GENESIS_UTREEXO_ROOT = "566203e44f300cfdb9e47dddc722b66ae2c68fce00f80b800a5caac16d69fed5";
static constexpr uint64_t     GENESIS_TIMESTAMP    = 1776384000;
static constexpr uint32_t     GENESIS_DIFFICULTY    = 0x1d31ffce;
static constexpr uint32_t     GENESIS_NONCE        = 813915426;

static constexpr const char*  GENESIS_COINBASE_HEX =
        "0100000001000000000000000000000000000000000000000000000000000000000000"
        "0000ffffffff480044696e65726f3a205265616c204d6f6e657920466f722046726565"
        "2050656f706c65202d20506f73742d5175616e74756d204e61746976652e2041707269"
        "6c2031372032303236ffffffff0100e40b5402000000496a4744696e65726f3a205265"
        "616c204d6f6e657920466f7220467265652050656f706c65202d20506f73742d517561"
        "6e74756d204e61746976652e20417072696c203137203230323600000000";

static constexpr const char*  GENESIS_MOTTO =
        "Dinero: Real Money For Free People - Post-Quantum Native. April 17 2026";

// ---------------------------------------------------------------------------
// Premine block (height 1)
// ---------------------------------------------------------------------------

static constexpr int          PREMINE_HEIGHT          = 1;
static constexpr int          PREMINE_VERSION         = 1;
static constexpr const char*  PREMINE_BLOCK_HASH      = "0000000000000000000000000000000000000000000000000000000000000000";
static constexpr const char*  PREMINE_PREV_HASH       = "0000001c36abf27e2c233ff40ed0c08888926c24450f3bff82a047ae1528b76f";
static constexpr const char*  PREMINE_MERKLE_ROOT     = "0000000000000000000000000000000000000000000000000000000000000000";
static constexpr const char*  PREMINE_UTREEXO_ROOT    = "0000000000000000000000000000000000000000000000000000000000000000";
static constexpr uint64_t     PREMINE_TIMESTAMP       = 0;
static constexpr uint32_t     PREMINE_DIFFICULTY       = 0x1d31ffce;
static constexpr uint32_t     PREMINE_NONCE           = 0;

static constexpr const char*  PREMINE_COINBASE_HEX =
        "00";

static constexpr uint64_t     PREMINE_AMOUNT_UNA    = 0ULL;
static constexpr uint64_t     PREMINE_AMOUNT_DIN     = 0ULL;
static constexpr const char*  PREMINE_ADDRESS         = "";
static constexpr const char*  PREMINE_SCRIPTPUBKEY_HEX = "00";
static constexpr const char*  PREMINE_DERIVATION      = "";

// ---------------------------------------------------------------------------
// Compile-time sanity checks
// ---------------------------------------------------------------------------

// v7: no premine (amount is zero in the bundle; kept for schema compatibility).
static_assert(PREMINE_AMOUNT_UNA == 0ULL,
              "v7 ships without a premine — genesis_bundle.json must have amount_una=0");
static_assert(PREMINE_HEIGHT == 1,
              "Premine record must be at height 1 (schema slot, even when zero)");

}  // namespace chain_bundle
}  // namespace dinero
