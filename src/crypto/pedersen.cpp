#include "crypto/pedersen.h"
#include "crypto/evp_secp256k1.h"
#include <openssl/rand.h>
#include <cstring>
#include <memory>

extern "C" {

secp256k1_context* pedersen_get_context() {
    return dinero::crypto::GetSecp256k1ContextSignVerify();
}

int pedersen_commit(
    uint8_t* commitment_out,
    const uint8_t* blinding,
    uint64_t amount
) {
    secp256k1_context* ctx = pedersen_get_context();
    secp256k1_pedersen_commitment commit;

    // Create commitment using secp256k1-zkp
    if (!secp256k1_pedersen_commit(
            ctx,
            &commit,
            blinding,
            amount,
            &secp256k1_generator_const_h,  // Standard H generator
            &secp256k1_generator_const_g   // Standard G generator
        )) {
        return 0;
    }

    // Serialize commitment
    return secp256k1_pedersen_commitment_serialize(ctx, commitment_out, &commit);
}

int pedersen_commit_with_generator(
    uint8_t* commitment_out,
    const uint8_t* blinding,
    uint64_t amount,
    const uint8_t* generator
) {
    secp256k1_context* ctx = pedersen_get_context();
    secp256k1_pedersen_commitment commit;
    secp256k1_generator gen;

    // Parse generator (or use default)
    if (generator) {
        if (!secp256k1_generator_parse(ctx, &gen, generator)) {
            return 0;
        }
    } else {
        gen = secp256k1_generator_const_h;
    }

    // Create commitment
    if (!secp256k1_pedersen_commit(
            ctx,
            &commit,
            blinding,
            amount,
            &gen,
            &secp256k1_generator_const_g
        )) {
        return 0;
    }

    // Serialize commitment
    return secp256k1_pedersen_commitment_serialize(ctx, commitment_out, &commit);
}

int pedersen_verify_commitment_sum(
    const uint8_t* positive,
    size_t n_positive,
    const uint8_t* negative,
    size_t n_negative
) {
    secp256k1_context* ctx = pedersen_get_context();

    // Parse commitments
    std::vector<secp256k1_pedersen_commitment> pos_commits(n_positive);
    std::vector<const secp256k1_pedersen_commitment*> pos_ptrs(n_positive);

    for (size_t i = 0; i < n_positive; ++i) {
        if (!secp256k1_pedersen_commitment_parse(
                ctx,
                &pos_commits[i],
                positive + i * PEDERSEN_COMMITMENT_SIZE)) {
            return 0;
        }
        pos_ptrs[i] = &pos_commits[i];
    }

    std::vector<secp256k1_pedersen_commitment> neg_commits(n_negative);
    std::vector<const secp256k1_pedersen_commitment*> neg_ptrs(n_negative);

    for (size_t i = 0; i < n_negative; ++i) {
        if (!secp256k1_pedersen_commitment_parse(
                ctx,
                &neg_commits[i],
                negative + i * PEDERSEN_COMMITMENT_SIZE)) {
            return 0;
        }
        neg_ptrs[i] = &neg_commits[i];
    }

    // Verify sum using secp256k1
    return secp256k1_pedersen_verify_tally(
        ctx,
        pos_ptrs.data(),
        n_positive,
        neg_ptrs.data(),
        n_negative
    );
}

int pedersen_parse_commitment(
    secp256k1_pedersen_commitment* commitment_out,
    const uint8_t* input
) {
    secp256k1_context* ctx = pedersen_get_context();
    return secp256k1_pedersen_commitment_parse(ctx, commitment_out, input);
}

int pedersen_serialize_commitment(
    uint8_t* output,
    const secp256k1_pedersen_commitment* commitment
) {
    secp256k1_context* ctx = pedersen_get_context();
    return secp256k1_pedersen_commitment_serialize(ctx, output, commitment);
}

int pedersen_generate_blinding(uint8_t* blinding_out) {
    // Use OpenSSL for cryptographically secure random bytes
    return RAND_bytes(blinding_out, PEDERSEN_BLINDING_SIZE) == 1 ? 1 : 0;
}

int pedersen_blind_add(
    uint8_t* result_out,
    const uint8_t* a,
    const uint8_t* b
) {
    secp256k1_context* ctx = pedersen_get_context();

    // Convert to scalars
    secp256k1_scalar scalar_a, scalar_b, result;

    int overflow = 0;
    secp256k1_scalar_set_b32(&scalar_a, a, &overflow);
    if (overflow) return 0;

    secp256k1_scalar_set_b32(&scalar_b, b, &overflow);
    if (overflow) return 0;

    // Add: result = a + b (mod n)
    secp256k1_scalar_add(&result, &scalar_a, &scalar_b);

    // Convert back to bytes
    secp256k1_scalar_get_b32(result_out, &result);

    return 1;
}

int pedersen_blind_subtract(
    uint8_t* result_out,
    const uint8_t* a,
    const uint8_t* b
) {
    secp256k1_context* ctx = pedersen_get_context();

    // Convert to scalars
    secp256k1_scalar scalar_a, scalar_b, neg_b, result;

    int overflow = 0;
    secp256k1_scalar_set_b32(&scalar_a, a, &overflow);
    if (overflow) return 0;

    secp256k1_scalar_set_b32(&scalar_b, b, &overflow);
    if (overflow) return 0;

    // Negate b: neg_b = -b (mod n)
    secp256k1_scalar_negate(&neg_b, &scalar_b);

    // Subtract: result = a + (-b) = a - b (mod n)
    secp256k1_scalar_add(&result, &scalar_a, &neg_b);

    // Convert back to bytes
    secp256k1_scalar_get_b32(result_out, &result);

    return 1;
}

int pedersen_blind_sum(
    uint8_t* result_out,
    const uint8_t** positive,
    size_t n_positive,
    const uint8_t** negative,
    size_t n_negative
) {
    secp256k1_context* ctx = pedersen_get_context();
    secp256k1_scalar result;

    // Initialize to zero
    secp256k1_scalar_clear(&result);

    // Add positive blinds
    for (size_t i = 0; i < n_positive; ++i) {
        secp256k1_scalar scalar;
        int overflow = 0;
        secp256k1_scalar_set_b32(&scalar, positive[i], &overflow);
        if (overflow) return 0;

        secp256k1_scalar_add(&result, &result, &scalar);
    }

    // Subtract negative blinds
    for (size_t i = 0; i < n_negative; ++i) {
        secp256k1_scalar scalar, neg_scalar;
        int overflow = 0;
        secp256k1_scalar_set_b32(&scalar, negative[i], &overflow);
        if (overflow) return 0;

        secp256k1_scalar_negate(&neg_scalar, &scalar);
        secp256k1_scalar_add(&result, &result, &neg_scalar);
    }

    // Convert result to bytes
    secp256k1_scalar_get_b32(result_out, &result);

    return 1;
}

const char* pedersen_version() {
    return "Dinero Pedersen Commitments 1.0.0 (secp256k1-zkp)";
}

} // extern "C"
