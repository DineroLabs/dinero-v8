/**
 * V7 PQ Scheme Registry — genesis data + lookup.
 *
 * Any change to the values in this file is a consensus change. Do not tune
 * these numbers casually; they are set by the V7 spec and confirmed against
 * the weakest fleet node's ML-DSA verify throughput.
 */

#include "consensus/pq/scheme_registry.h"

#include <cstdint>

namespace dinero::consensus::pq {

namespace {

// ---------------------------------------------------------------------------
// Genesis registry table.
//
// One row per scheme_id value we care about. Unassigned values (0x04..0xFE
// and 0xFF) resolve to kReservedSentinel below via GetSchemeParams.
//
// Values sourced from docs/consensus/V7_GENESIS_SPEC.md § "Signature Scheme
// Registry" and § "Block Weight And Fee Policy". Any drift between this
// table and the spec is a bug in one or the other.
// ---------------------------------------------------------------------------

constexpr PQSchemeParams kMlDsa65 = {
    /* scheme_id          = */ SCHEME_ID_ML_DSA_65,
    /* name               = */ "ML-DSA-65",
    /* state              = */ SchemeState::Accept,
    /* pubkey_bytes_max   = */ 1952,
    /* signature_bytes_max= */ 3309,
    /* witness_byte_weight= */ 1,
    /* verify_cost_weight = */ 25,
    /* activation_height  = */ 0,   // genesis
};

constexpr PQSchemeParams kFalcon512 = {
    /* scheme_id          = */ SCHEME_ID_FALCON_512,
    /* name               = */ "FALCON-512",
    /* state              = */ SchemeState::DarkReserved,
    /* pubkey_bytes_max   = */ 897,
    /* signature_bytes_max= */ 666,
    /* witness_byte_weight= */ 0,       // unused while DarkReserved
    /* verify_cost_weight = */ 0,       // unused while DarkReserved
    /* activation_height  = */ 0xFFFFFFFFu, // no activation scheduled
};

constexpr PQSchemeParams kSphincs128 = {
    /* scheme_id          = */ SCHEME_ID_SPHINCS_128,
    /* name               = */ "SPHINCS+-128s",
    /* state              = */ SchemeState::DarkReserved,
    /* pubkey_bytes_max   = */ 32,
    /* signature_bytes_max= */ 7856,
    /* witness_byte_weight= */ 0,       // unused while DarkReserved
    /* verify_cost_weight = */ 0,       // unused while DarkReserved
    /* activation_height  = */ 0xFFFFFFFFu,
};

// Sentinel returned for every unassigned scheme_id (0x00 and 0x04..0xFF).
// Consensus code that correctly uses IsSchemeAcceptedAtHeight() will reject
// spends for any Reserved row; the zero parameters make a stray unchecked
// use obviously broken (zero weight, zero byte limit).
constexpr PQSchemeParams kReservedSentinel = {
    /* scheme_id          = */ 0x00,
    /* name               = */ "RESERVED",
    /* state              = */ SchemeState::Reserved,
    /* pubkey_bytes_max   = */ 0,
    /* signature_bytes_max= */ 0,
    /* witness_byte_weight= */ 0,
    /* verify_cost_weight = */ 0,
    /* activation_height  = */ 0xFFFFFFFFu,
};

} // namespace

const PQSchemeParams& GetSchemeParams(uint8_t scheme_id) {
    switch (scheme_id) {
        case SCHEME_ID_ML_DSA_65:   return kMlDsa65;
        case SCHEME_ID_FALCON_512:  return kFalcon512;
        case SCHEME_ID_SPHINCS_128: return kSphincs128;
        default:                    return kReservedSentinel;
    }
}

bool IsSchemeAcceptedAtHeight(uint8_t scheme_id, uint32_t height) {
    const PQSchemeParams& row = GetSchemeParams(scheme_id);
    if (row.state != SchemeState::Accept) {
        return false;
    }
    return height >= row.activation_height;
}

bool IsSchemeDarkReserved(uint8_t scheme_id) {
    return GetSchemeParams(scheme_id).state == SchemeState::DarkReserved;
}

} // namespace dinero::consensus::pq
