/*
 * Dinero v7: explicit seeded ML-DSA-65 keypair.
 *
 * Replicates the body of PQCLEAN_MLDSA65_CLEAN_crypto_sign_keypair with the
 * 32-byte seed provided as an explicit parameter instead of being drawn
 * from `randombytes()`. The output is bit-identical to what PQClean would
 * produce if its one-off `randombytes(seedbuf, SEEDBYTES)` call had
 * returned the caller-provided seed bytes.
 *
 * Why this file exists
 * --------------------
 * The first implementation used a thread-local override of the
 * `randombytes` symbol to make keygen deterministic. That worked (and
 * produced cross-arch bit-identical pubkeys on Apple Silicon and EPYC-
 * Rome, confirmed 2026-04-16) but was "magic at a distance": the caller
 * could not tell from the function signature that the RNG was being
 * redirected. Reviewer caution was correct: an explicit seeded API is
 * the right long-term shape.
 *
 * This file is the explicit path. The thread-local randombytes hook has
 * been removed; upstream PQClean's `common/randombytes.c` is back in the
 * pqclean_ml_dsa_65 library source list.
 *
 * Correctness
 * -----------
 * The function below is a line-for-line transcription of PQClean's
 * `crypto_sign_keypair` (third_party/pqclean/crypto_sign/ml-dsa-65/clean/
 * sign.c) with only the `randombytes(seedbuf, SEEDBYTES)` call replaced
 * by `memcpy(seedbuf, seed, SEEDBYTES)`. The known-answer test at
 * tools/pq_bench/pq_roundtrip_test.cpp T17 pins the first 32 bytes of the
 * resulting pubkey and is enforced across Apple Silicon and EPYC-Rome.
 * Any drift fails the build's self-test.
 *
 * Upstream-pin dependency
 * -----------------------
 * This file consumes internal PQClean headers (polyvec.h, params.h, etc.)
 * and thus is tightly coupled to the pinned upstream commit. If
 * third_party/pqclean/ is bumped, this file must be re-verified against
 * the new `crypto_sign_keypair`. The existing known-answer pin catches
 * any algorithmic drift automatically.
 */

#include "api.h"
#include "fips202.h"
#include "packing.h"
#include "params.h"
#include "poly.h"
#include "polyvec.h"
#include "symmetric.h"

#include <stdint.h>
#include <string.h>

int dinero_pq_mldsa65_keypair_from_seed(uint8_t *pk,
                                        uint8_t *sk,
                                        const uint8_t seed[SEEDBYTES]) {
    uint8_t seedbuf[2 * SEEDBYTES + CRHBYTES];
    uint8_t tr[TRBYTES];
    const uint8_t *rho, *rhoprime, *key;
    polyvecl mat[K];
    polyvecl s1, s1hat;
    polyveck s2, t1, t0;

    /* Explicit seed — the ONLY deviation from PQClean's crypto_sign_keypair
     * is this line, where `randombytes(seedbuf, SEEDBYTES)` was replaced
     * with a copy from the caller-provided seed. */
    memcpy(seedbuf, seed, SEEDBYTES);

    seedbuf[SEEDBYTES + 0] = K;
    seedbuf[SEEDBYTES + 1] = L;
    shake256(seedbuf, 2 * SEEDBYTES + CRHBYTES, seedbuf, SEEDBYTES + 2);
    rho      = seedbuf;
    rhoprime = rho + SEEDBYTES;
    key      = rhoprime + CRHBYTES;

    /* Expand matrix */
    PQCLEAN_MLDSA65_CLEAN_polyvec_matrix_expand(mat, rho);

    /* Sample short vectors s1 and s2 */
    PQCLEAN_MLDSA65_CLEAN_polyvecl_uniform_eta(&s1, rhoprime, 0);
    PQCLEAN_MLDSA65_CLEAN_polyveck_uniform_eta(&s2, rhoprime, L);

    /* Matrix-vector multiplication */
    s1hat = s1;
    PQCLEAN_MLDSA65_CLEAN_polyvecl_ntt(&s1hat);
    PQCLEAN_MLDSA65_CLEAN_polyvec_matrix_pointwise_montgomery(&t1, mat, &s1hat);
    PQCLEAN_MLDSA65_CLEAN_polyveck_reduce(&t1);
    PQCLEAN_MLDSA65_CLEAN_polyveck_invntt_tomont(&t1);

    /* Add error vector s2 */
    PQCLEAN_MLDSA65_CLEAN_polyveck_add(&t1, &t1, &s2);

    /* Extract t1 and write public key */
    PQCLEAN_MLDSA65_CLEAN_polyveck_caddq(&t1);
    PQCLEAN_MLDSA65_CLEAN_polyveck_power2round(&t1, &t0, &t1);
    PQCLEAN_MLDSA65_CLEAN_pack_pk(pk, rho, &t1);

    /* Compute H(rho, t1) and write secret key */
    shake256(tr, TRBYTES, pk, PQCLEAN_MLDSA65_CLEAN_CRYPTO_PUBLICKEYBYTES);
    PQCLEAN_MLDSA65_CLEAN_pack_sk(sk, rho, tr, key, &t0, &s1, &s2);

    return 0;
}
