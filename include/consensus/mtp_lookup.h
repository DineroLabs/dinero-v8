#pragma once

// ============================================================================
// BIP68 MTP Lookup Factory (Phase 23.3: Time-based Sequence Locks)
// ============================================================================
//
// This header declares the MTP lookup factory function used for BIP68
// time-based relative locktimes. The implementation lives in
// src/chainstate/mtp_lookup.cpp to avoid circular dependencies.
//
// Layering:
//   consensus/tx_validation.h defines MtpLookupFn type
//   consensus/mtp_lookup.h declares CreateMtpLookup (this file)
//   chainstate/mtp_lookup.cpp implements CreateMtpLookup
//
// This separation ensures dinero_consensus doesn't depend on dinero_chainstate.

#include "consensus/tx_validation.h"  // For MtpLookupFn

namespace dinero {

// Forward declaration - ChainDB is in dinero::storage
class ChainDB;

namespace consensus {

/**
 * Create MTP lookup function for BIP68 time-based sequence locks
 *
 * This factory creates a function that looks up the Median Time Past (MTP)
 * for a given block height. Required for BIP68 time-based relative locktimes.
 *
 * Implementation:
 * 1. Uses ChainDB::getBlockHashByHeight() to get block hash at height
 * 2. Uses FindBlockIndex() to get CBlockIndex*
 * 3. Calls GetMedianTimePast() on the block index
 *
 * If ChainDB is nullptr, returns nullptr (fail-closed behavior).
 * If height lookup fails, the returned function returns std::nullopt.
 *
 * @param chain_db  Pointer to ChainDB (can be nullptr for fail-closed)
 * @return          MtpLookupFn that can be used in TxValidationContext
 */
MtpLookupFn CreateMtpLookup(::dinero::ChainDB* chain_db);

} // namespace consensus
} // namespace dinero
