#pragma once
/*
 * ShieldedProverKit C ABI.
 *
 * This is the native boundary exported to iOS. Keep it plain C:
 * Swift should not need C++ types, daemon internals, or duplicated
 * transaction-sighash logic to build a mobile shielded spend.
 */

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  define DINERO_SHIELDED_PROVERKIT_API __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#  define DINERO_SHIELDED_PROVERKIT_API __attribute__((visibility("default")))
#else
#  define DINERO_SHIELDED_PROVERKIT_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum dinero_shielded_status {
    DINERO_SHIELDED_OK = 0,
    DINERO_SHIELDED_ERR_INVALID_ARGUMENT = 1,
    DINERO_SHIELDED_ERR_DESERIALIZE_TX = 2,
    DINERO_SHIELDED_ERR_BUILD_FAILED = 3,
    DINERO_SHIELDED_ERR_ALLOCATION = 4,
    DINERO_SHIELDED_ERR_EXCEPTION = 5,
    /* out_addr buffer too small; *out_addr_len is set to the required
     * size (including the NUL terminator) so the caller can retry. */
    DINERO_SHIELDED_ERR_BUFFER_TOO_SMALL = 6,
};

typedef struct dinero_shielded_spend_note {
    uint8_t rcm[32];
    uint8_t d[32];
    uint64_t leaf_index;
    uint64_t value_una;
    uint8_t anchor[32];
    uint8_t merkle_path[32][32];
} dinero_shielded_spend_note;

typedef struct dinero_shielded_unshield_request {
    uint8_t version;
    /*
     * Serialized unsigned shielded transaction envelope. For v5/v6 this must
     * include the explicit-fee field and an empty length-prefixed shielded
     * bundle slot before locktime. Zero-input unshield envelopes use the
     * SegWit marker/flag prefix (00 01) before the empty input count so the
     * legacy parser does not confuse vin_count=0/out_count=1 for a marker.
     * The prover replaces the empty bundle slot with the returned bytes.
     */
    const uint8_t* serialized_unsigned_tx;
    size_t serialized_unsigned_tx_len;
    uint64_t fee_una;
    const dinero_shielded_spend_note* note;
} dinero_shielded_unshield_request;

typedef struct dinero_shielded_unshield_result {
    uint8_t nullifier[32];
    uint8_t anchor[32];
    uint8_t* bundle_bytes;
    size_t bundle_len;
    char* error;
} dinero_shielded_unshield_result;

DINERO_SHIELDED_PROVERKIT_API int
dinero_shielded_compute_note_commitment(const uint8_t d[32],
                                        const uint8_t rcm[32],
                                        uint64_t value_una,
                                        uint8_t out_commitment[32]);

DINERO_SHIELDED_PROVERKIT_API int
dinero_shielded_compute_nullifier(const uint8_t rcm[32],
                                  uint64_t leaf_index,
                                  uint8_t out_nullifier[32]);

DINERO_SHIELDED_PROVERKIT_API int dinero_shielded_build_unshield_bundle(
    const dinero_shielded_unshield_request* req,
    dinero_shielded_unshield_result* out);

DINERO_SHIELDED_PROVERKIT_API void
dinero_shielded_free_result(dinero_shielded_unshield_result* out);

/*
 * Derive the diversified shielded receive address for (dk, ivk, j).
 *
 * dk32/ivk32: raw 32-byte keys (derived client-side; the wallet seed
 * never crosses this ABI). hrp: NUL-terminated "dins" | "tdins" | "rdins".
 * On success writes the bech32m address (NUL-terminated) into out_addr and
 * sets *out_addr_len to the length written, excluding the NUL terminator.
 *
 * *out_addr_len must be set by the caller on entry to the capacity of
 * out_addr (including room for the NUL terminator). If the buffer is too
 * small, returns DINERO_SHIELDED_ERR_BUFFER_TOO_SMALL and sets
 * *out_addr_len to the required capacity (including the NUL terminator);
 * out_addr is left untouched.
 *
 * Returns 0 (DINERO_SHIELDED_OK) on success, another dinero_shielded_status
 * value otherwise (see enum above) — always >= 0, matching the other four
 * exports in this header. Callers MUST check `!= DINERO_SHIELDED_OK`, not
 * `< 0`. No exceptions cross this ABI and no heap allocation is returned
 * to the caller.
 */
DINERO_SHIELDED_PROVERKIT_API int32_t dinero_shielded_derive_address(
    const uint8_t* dk32, const uint8_t* ivk32, uint64_t j,
    const char* hrp, char* out_addr, size_t* out_addr_len);

#ifdef __cplusplus
} // extern "C"
#endif
