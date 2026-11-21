#pragma once
// C ABI of rust/zcash_swap: Zcash v5 transparent transactions for DIN <-> ZEC
// swaps, built on librustzcash (zcash_primitives, zcash_transparent,
// zcash_address). See docs/design/din-zec-atomic-swaps.md.
//
// No secret crosses this boundary: C++ signs the sighash returned here with
// libsecp256k1. Every function returns DZS_OK or an error code and never
// unwinds into C++.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DZS_OK = 0,
    DZS_ERR_ARG = 1,             // null pointer, empty input or output list, value out of range
    DZS_ERR_BRANCH = 2,          // unknown consensus branch id, or v5 is not valid in it
    DZS_ERR_PARSE = 3,           // the bytes are not exactly one v5 transaction
    DZS_ERR_BUFFER = 4,          // output buffer too small; *out_len holds the size needed
    DZS_ERR_INDEX = 5,           // input index out of range, or prevouts count != input count
    DZS_ERR_ADDRESS = 6,         // not a transparent P2PKH or P2SH address
    DZS_ERR_PANIC = 7,           // a bug: the Rust side panicked and the panic was caught
    DZS_ERR_SCRIPT_INVALID = 8,  // dzs_verify_input: the input's scripts do not verify
};

enum { DZS_NET_MAIN = 0, DZS_NET_TEST = 1, DZS_NET_REGTEST = 2 };
enum { DZS_ADDR_P2PKH = 0, DZS_ADDR_P2SH = 1 };

typedef struct {
    uint8_t prev_txid[32];  // wire byte order (reverse of the usual hex)
    uint32_t prev_index;
    uint32_t sequence;
    const uint8_t* script_sig;  // may be null when script_sig_len is 0
    size_t script_sig_len;
} dzs_txin;

typedef struct {
    uint64_t value_zat;
    const uint8_t* script_pubkey;
    size_t script_pubkey_len;
} dzs_txout;

// A v5 transaction with only a transparent bundle. expiry_height 0 means it
// never expires (ZIP 203); otherwise it must be below 500,000,000.
int32_t dzs_tx_build(uint32_t consensus_branch_id, uint32_t lock_time, uint32_t expiry_height,
                     const dzs_txin* vin, size_t vin_len, const dzs_txout* vout, size_t vout_len,
                     uint8_t* out, size_t out_cap, size_t* out_len);

// ZIP 244 txid of a serialized v5 transaction, wire byte order.
int32_t dzs_txid(const uint8_t* tx, size_t tx_len, uint8_t out[32]);

// ZIP 244 SIGHASH_ALL for transparent input `input_index`. `prevouts` are the
// outputs spent by every input, in input order (the digest commits to all of
// their amounts and scripts). scriptSigs are not committed to, so the unsigned
// transaction gives the same hash as the signed one.
int32_t dzs_sighash_all(const uint8_t* tx, size_t tx_len, size_t input_index,
                        const dzs_txout* prevouts, size_t prevouts_len, uint8_t out[32]);

// t-addresses. Regtest encodes like testnet, so decoding never returns
// DZS_NET_REGTEST. out is not NUL-terminated.
int32_t dzs_taddr_encode(uint8_t network, uint8_t kind, const uint8_t hash[20], char* out,
                         size_t out_cap, size_t* out_len);
int32_t dzs_taddr_decode(const char* s, size_t s_len, uint8_t* network, uint8_t* kind,
                         uint8_t hash[20]);

// Only in builds with the `script-verify` cargo feature (tests): runs input
// `input_index` through the zcash_script interpreter with zcashd's standard
// flags (P2SH, CLTV, strict DER, low S, minimal pushes, clean stack).
int32_t dzs_verify_input(const uint8_t* tx, size_t tx_len, size_t input_index,
                         const dzs_txout* prevouts, size_t prevouts_len);

#ifdef __cplusplus
}
#endif
