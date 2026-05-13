/*
 * Copyright (c) 2026 Dinero Developers
 * Distributed under the MIT software license.
 *
 * MSM Shim — compiled as part of secp256k1-zkp library to access
 * secp256k1_ecmult_multi_var (Pippenger/Strauss multi-scalar multiplication).
 *
 * This file MUST be compiled as part of the secp256k1 CMake target so that
 * it inherits the correct compile definitions (ECMULT_WINDOW_SIZE, etc.) and
 * can include the internal secp256k1 implementation headers.
 *
 * Exported symbol: dinero_secp256k1_msm
 */

#define SECP256K1_BUILD

/* Internal secp256k1-zkp implementation headers.
 * These include static functions for field, scalar, group, ecmult arithmetic.
 * Safe to include here because all functions are static (no link conflicts). */
#include "util.h"
#include "int128_impl.h"  /* secp256k1_u128_mul et al. — not pulled in by scalar_impl.h */
#include "assumptions.h"
#include "field_impl.h"
#include "scalar_impl.h"
#include "group_impl.h"
#include "ecmult_impl.h"
#include "scratch_impl.h"

/* Public header for secp256k1_pubkey, secp256k1_scratch_space, etc. */
#include <secp256k1.h>

#include <stdlib.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * pubkey_load / pubkey_save
 * Duplicated from secp256k1.c (they are static there and not accessible).
 * ----------------------------------------------------------------------- */

static void dinero_pubkey_load(secp256k1_ge* ge, const secp256k1_pubkey* pubkey) {
    if (sizeof(secp256k1_ge_storage) == 64) {
        secp256k1_ge_storage s;
        memcpy(&s, &pubkey->data[0], sizeof(s));
        secp256k1_ge_from_storage(ge, &s);
    } else {
        secp256k1_fe x, y;
        secp256k1_fe_set_b32_limit(&x, pubkey->data);
        secp256k1_fe_set_b32_limit(&y, pubkey->data + 32);
        secp256k1_ge_set_xy(ge, &x, &y);
    }
}

static void dinero_pubkey_save(secp256k1_pubkey* pubkey, secp256k1_ge* ge) {
    if (sizeof(secp256k1_ge_storage) == 64) {
        secp256k1_ge_storage s;
        secp256k1_ge_to_storage(&s, ge);
        memcpy(&pubkey->data[0], &s, sizeof(s));
    } else {
        secp256k1_fe_normalize_var(&ge->x);
        secp256k1_fe_normalize_var(&ge->y);
        secp256k1_fe_get_b32(&pubkey->data[0], &ge->x);
        secp256k1_fe_get_b32(&pubkey->data[32], &ge->y);
    }
}

/* -----------------------------------------------------------------------
 * ecmult_multi_var callback
 * ----------------------------------------------------------------------- */

typedef struct {
    const secp256k1_scalar* scalars;
    const secp256k1_ge*     points;
} dinero_msm_cbdata;

static int dinero_msm_cb(secp256k1_scalar *sc, secp256k1_ge *pt,
                          size_t idx, void *data) {
    const dinero_msm_cbdata* d = (const dinero_msm_cbdata*)data;
    *sc = d->scalars[idx];
    *pt = d->points[idx];
    return 1;
}

/* Null error callback — used so we don't need ctx internals. */
static void dinero_null_error_fn(const char* text, void* data) {
    (void)text; (void)data;
}
static const secp256k1_callback dinero_null_cb = {dinero_null_error_fn, NULL};

/* -----------------------------------------------------------------------
 * dinero_secp256k1_msm
 *
 * Multi-scalar multiplication: result = sum(scalar[i] * point[i])
 *
 * Parameters:
 *   ctx     - secp256k1 context (verify capable)
 *   scalars - n × 32-byte scalars in big-endian format
 *   points  - n secp256k1_pubkey points (all non-identity)
 *   n       - number of pairs; must be > 0
 *   result  - output pubkey; set to all-zeros (infinity) on failure
 *
 * Returns 1 on success, 0 if result is point at infinity or n==0.
 *
 * Uses secp256k1_ecmult_multi_var which selects Strauss (n<88) or
 * Pippenger (n>=88) automatically for best performance.
 * ----------------------------------------------------------------------- */
int dinero_secp256k1_msm(
    const secp256k1_context* ctx,
    const unsigned char (*scalars_bytes)[32],
    const secp256k1_pubkey* points,
    size_t n,
    secp256k1_pubkey* result
) {
    secp256k1_scalar* sc_arr;
    secp256k1_ge*     ge_arr;
    secp256k1_gej     result_j;
    dinero_msm_cbdata cbdata;
    secp256k1_ge result_ge;
    secp256k1_scratch* scratch;
    size_t scratch_sz;
    size_t i;
    int ok;
    int bw;

    if (n == 0) return 0;

    sc_arr = (secp256k1_scalar*)checked_malloc(&dinero_null_cb,
                                               n * sizeof(secp256k1_scalar));
    ge_arr = (secp256k1_ge*)checked_malloc(&dinero_null_cb,
                                           n * sizeof(secp256k1_ge));
    if (!sc_arr || !ge_arr) {
        free(sc_arr);
        free(ge_arr);
        return 0;
    }

    /* big-endian bytes → secp256k1_scalar */
    for (i = 0; i < n; ++i) {
        secp256k1_scalar_set_b32(&sc_arr[i], scalars_bytes[i], NULL);
    }

    /* secp256k1_pubkey → secp256k1_ge */
    for (i = 0; i < n; ++i) {
        dinero_pubkey_load(&ge_arr[i], &points[i]);
    }

    cbdata.scalars = sc_arr;
    cbdata.points  = ge_arr;

    /* Allocate exactly the scratch space Pippenger needs to process all n points
     * in a single batch.  The previous formula (n * sizeof(secp256k1_gej)) was
     * ~30% of what Pippenger requires, forcing secp256k1_ecmult_multi_var to
     * split n points into ~4 sub-batches and pay the bucket-reduction overhead 4×.
     *
     * secp256k1_pippenger_scratch_size(n, bw) returns the exact amount needed:
     *   (sizeof(gej) << bw) + sizeof(pippenger_state) + (2n+2) * entry_size
     * For n=64K, bw=12: ~26MB vs the old 8MB. */
    bw = secp256k1_pippenger_bucket_window(n);
    scratch_sz = secp256k1_pippenger_scratch_size(n, bw);
    scratch = secp256k1_scratch_space_create(ctx, scratch_sz);
    if (!scratch) {
        free(sc_arr);
        free(ge_arr);
        return 0;
    }

    secp256k1_gej_set_infinity(&result_j);
    ok = secp256k1_ecmult_multi_var(&dinero_null_cb, scratch, &result_j, NULL,
                                    dinero_msm_cb, &cbdata, n);
    secp256k1_scratch_space_destroy(ctx, scratch);
    free(sc_arr);
    free(ge_arr);

    if (!ok || secp256k1_gej_is_infinity(&result_j)) {
        memset(result, 0, sizeof(*result));
        return 0;
    }

    secp256k1_ge_set_gej(&result_ge, &result_j);
    dinero_pubkey_save(result, &result_ge);
    return 1;
}
