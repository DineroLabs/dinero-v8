#!/usr/bin/env python3
"""Generate chain_bundle_generated.h from genesis_bundle.json.

Usage:
    python3 tools/gen_constants_from_bundle.py docs/chain/genesis_bundle.json \
        > include/consensus/chain_bundle_generated.h
"""

import json
import sys
import re
from pathlib import Path


def die(msg: str) -> None:
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def validate_hex(value: str, name: str, expected_len: int | None = None) -> None:
    """Validate that a string is valid hex and optionally check its length."""
    if not re.fullmatch(r"[0-9a-fA-F]+", value):
        die(f"{name}: not valid hex: {value!r}")
    if expected_len is not None and len(value) != expected_len:
        die(f"{name}: expected {expected_len} hex chars, got {len(value)}")


def require(obj: dict, key: str, section: str) -> object:
    """Require a key exists in a dict, or die."""
    if key not in obj:
        die(f"missing required field '{key}' in '{section}'")
    return obj[key]


def split_hex_literal(hex_str: str, width: int = 70) -> str:
    """Split a long hex string into C++ string literal concatenation segments."""
    if len(hex_str) <= width:
        return f'"{hex_str}"'
    parts = []
    for i in range(0, len(hex_str), width):
        parts.append(f'"{hex_str[i:i+width]}"')
    return "\n        ".join(parts)


def main() -> None:
    if len(sys.argv) != 2:
        die(f"usage: {sys.argv[0]} <genesis_bundle.json>")

    json_path = Path(sys.argv[1])
    if not json_path.is_file():
        die(f"file not found: {json_path}")

    try:
        with open(json_path, "r") as f:
            bundle = json.load(f)
    except json.JSONDecodeError as e:
        die(f"malformed JSON: {e}")

    # --- Validate top-level structure ---
    if "genesis" not in bundle:
        die("missing 'genesis' section")
    if "premine" not in bundle:
        die("missing 'premine' section")

    gen = bundle["genesis"]
    pre = bundle["premine"]

    # --- Extract and validate genesis fields ---
    gen_height      = require(gen, "height", "genesis")
    gen_version     = require(gen, "version", "genesis")
    gen_prev_hash   = require(gen, "prev_block_hash", "genesis")
    gen_merkle      = require(gen, "merkle_root", "genesis")
    gen_utreexo     = require(gen, "utreexo_root", "genesis")
    gen_timestamp   = require(gen, "timestamp", "genesis")
    gen_difficulty   = require(gen, "difficulty", "genesis")
    gen_nonce       = require(gen, "nonce", "genesis")
    gen_block_hash  = require(gen, "block_hash", "genesis")
    gen_coinbase    = require(gen, "coinbase_hex", "genesis")
    gen_motto       = require(gen, "motto", "genesis")

    validate_hex(gen_prev_hash, "genesis.prev_block_hash", 64)
    validate_hex(gen_merkle, "genesis.merkle_root", 64)
    validate_hex(gen_utreexo, "genesis.utreexo_root", 64)
    validate_hex(gen_block_hash, "genesis.block_hash", 64)
    validate_hex(gen_difficulty, "genesis.difficulty")
    validate_hex(gen_coinbase, "genesis.coinbase_hex")

    gen_difficulty_u32 = int(gen_difficulty, 16)

    # --- Extract and validate premine fields ---
    pre_height      = require(pre, "height", "premine")
    pre_version     = require(pre, "version", "premine")
    pre_prev_hash   = require(pre, "prev_block_hash", "premine")
    pre_merkle      = require(pre, "merkle_root", "premine")
    pre_utreexo     = require(pre, "utreexo_root", "premine")
    pre_timestamp   = require(pre, "timestamp", "premine")
    pre_difficulty   = require(pre, "difficulty", "premine")
    pre_nonce       = require(pre, "nonce", "premine")
    pre_block_hash  = require(pre, "block_hash", "premine")
    pre_coinbase    = require(pre, "coinbase_hex", "premine")
    pre_amount_una = require(pre, "amount_una", "premine")
    pre_amount_din  = require(pre, "amount_din", "premine")
    pre_address     = require(pre, "address", "premine")
    pre_scriptpk    = require(pre, "scriptPubKey_hex", "premine")
    pre_derivation  = require(pre, "derivation", "premine")

    validate_hex(pre_prev_hash, "premine.prev_block_hash", 64)
    validate_hex(pre_merkle, "premine.merkle_root", 64)
    validate_hex(pre_utreexo, "premine.utreexo_root", 64)
    validate_hex(pre_block_hash, "premine.block_hash", 64)
    validate_hex(pre_difficulty, "premine.difficulty")
    validate_hex(pre_coinbase, "premine.coinbase_hex")
    validate_hex(pre_scriptpk, "premine.scriptPubKey_hex")

    pre_difficulty_u32 = int(pre_difficulty, 16)

    # --- Generate header ---
    gen_coinbase_lit = split_hex_literal(gen_coinbase)
    pre_coinbase_lit = split_hex_literal(pre_coinbase)

    header = f"""\
#pragma once

// =============================================================================
// AUTO-GENERATED from genesis_bundle.json -- DO NOT EDIT
//
// Regenerate with:
//   python3 tools/gen_constants_from_bundle.py docs/chain/genesis_bundle.json \\
//       > include/consensus/chain_bundle_generated.h
// =============================================================================

#include <cstdint>
#include <string>

namespace dinero {{
namespace chain_bundle {{

// ---------------------------------------------------------------------------
// Genesis block (height 0)
// ---------------------------------------------------------------------------

static constexpr int          GENESIS_HEIGHT       = {gen_height};
static constexpr int          GENESIS_VERSION      = {gen_version};
static constexpr const char*  GENESIS_BLOCK_HASH   = "{gen_block_hash}";
static constexpr const char*  GENESIS_MERKLE_ROOT  = "{gen_merkle}";
static constexpr const char*  GENESIS_UTREEXO_ROOT = "{gen_utreexo}";
static constexpr uint64_t     GENESIS_TIMESTAMP    = {gen_timestamp};
static constexpr uint32_t     GENESIS_DIFFICULTY    = 0x{gen_difficulty};
static constexpr uint32_t     GENESIS_NONCE        = {gen_nonce};

static constexpr const char*  GENESIS_COINBASE_HEX =
        {gen_coinbase_lit};

static constexpr const char*  GENESIS_MOTTO =
        "{gen_motto}";

// ---------------------------------------------------------------------------
// Premine block (height 1)
// ---------------------------------------------------------------------------

static constexpr int          PREMINE_HEIGHT          = {pre_height};
static constexpr int          PREMINE_VERSION         = {pre_version};
static constexpr const char*  PREMINE_BLOCK_HASH      = "{pre_block_hash}";
static constexpr const char*  PREMINE_PREV_HASH       = "{pre_prev_hash}";
static constexpr const char*  PREMINE_MERKLE_ROOT     = "{pre_merkle}";
static constexpr const char*  PREMINE_UTREEXO_ROOT    = "{pre_utreexo}";
static constexpr uint64_t     PREMINE_TIMESTAMP       = {pre_timestamp};
static constexpr uint32_t     PREMINE_DIFFICULTY       = 0x{pre_difficulty};
static constexpr uint32_t     PREMINE_NONCE           = {pre_nonce};

static constexpr const char*  PREMINE_COINBASE_HEX =
        {pre_coinbase_lit};

static constexpr uint64_t     PREMINE_AMOUNT_UNA    = {pre_amount_una}ULL;
static constexpr uint64_t     PREMINE_AMOUNT_DIN     = {pre_amount_din}ULL;
static constexpr const char*  PREMINE_ADDRESS         = "{pre_address}";
static constexpr const char*  PREMINE_SCRIPTPUBKEY_HEX = "{pre_scriptpk}";
static constexpr const char*  PREMINE_DERIVATION      = "{pre_derivation}";

// ---------------------------------------------------------------------------
// Compile-time sanity checks
// ---------------------------------------------------------------------------

// v7: no premine (amount is zero in the bundle; kept for schema compatibility).
static_assert(PREMINE_AMOUNT_UNA == 0ULL,
              "v7 ships without a premine — genesis_bundle.json must have amount_una=0");
static_assert(PREMINE_HEIGHT == 1,
              "Premine record must be at height 1 (schema slot, even when zero)");

}}  // namespace chain_bundle
}}  // namespace dinero
"""

    sys.stdout.write(header)


if __name__ == "__main__":
    main()
