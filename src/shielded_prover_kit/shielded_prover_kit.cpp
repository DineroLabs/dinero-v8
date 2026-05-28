#include "shielded_prover_kit/shielded_prover_kit.h"

#include "consensus/shielded/commitment_tree.h"
#include "primitives/transaction.h"
#include "wallet/shielded_derivation.h"
#include "wallet/shielded_wallet_ops.h"

#include <openssl/crypto.h>

#include <cstdlib>
#include <cstring>
#include <exception>
#include <new>
#include <string>
#include <vector>

namespace {

namespace sh = dinero::consensus::shielded;
namespace deriv = dinero::wallet::shielded;
namespace ops = dinero::wallet::shielded_ops;

static sh::Hash CopyHash(const uint8_t* bytes) {
    sh::Hash h{};
    std::memcpy(h.data(), bytes, h.size());
    return h;
}

static sh::Hash ValueToHash(uint64_t v) {
    sh::Hash h{};
    for (int i = 0; i < 8; ++i) {
        h[31 - i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
    }
    return h;
}

static void CleanseHash(sh::Hash& h) {
    OPENSSL_cleanse(h.data(), h.size());
}

struct HashCleanser {
    sh::Hash* value = nullptr;
    ~HashCleanser() {
        if (value) {
            CleanseHash(*value);
        }
    }
};

static char* CopyError(const std::string& message) {
    char* out = static_cast<char*>(std::malloc(message.size() + 1));
    if (!out) {
        return nullptr;
    }
    std::memcpy(out, message.c_str(), message.size() + 1);
    return out;
}

static void ResetResult(dinero_shielded_unshield_result* out) {
    if (!out) {
        return;
    }
    std::memset(out->nullifier, 0, sizeof(out->nullifier));
    std::memset(out->anchor, 0, sizeof(out->anchor));
    out->bundle_bytes = nullptr;
    out->bundle_len = 0;
    out->error = nullptr;
}

static int Fail(dinero_shielded_unshield_result* out,
                dinero_shielded_status code,
                const std::string& message) {
    if (out) {
        out->error = CopyError(message);
        if (!out->error) {
            return DINERO_SHIELDED_ERR_ALLOCATION;
        }
    }
    return code;
}

static int CopyBundleResult(const dinero::Transaction& tx,
                            const ops::AttachUnshieldResult& built,
                            dinero_shielded_unshield_result* out) {
    if (tx.shielded_bundle_bytes.empty()) {
        return Fail(out, DINERO_SHIELDED_ERR_BUILD_FAILED,
                    "unshield builder returned empty bundle bytes");
    }

    uint8_t* bytes = static_cast<uint8_t*>(
        std::malloc(tx.shielded_bundle_bytes.size()));
    if (!bytes) {
        return Fail(out, DINERO_SHIELDED_ERR_ALLOCATION,
                    "failed to allocate bundle bytes");
    }

    std::memcpy(bytes,
                tx.shielded_bundle_bytes.data(),
                tx.shielded_bundle_bytes.size());
    std::memcpy(out->nullifier, built.nullifier.data(), built.nullifier.size());
    std::memcpy(out->anchor, built.anchor.data(), built.anchor.size());
    out->bundle_bytes = bytes;
    out->bundle_len = tx.shielded_bundle_bytes.size();
    return DINERO_SHIELDED_OK;
}

} // namespace

extern "C" int dinero_shielded_compute_note_commitment(
    const uint8_t d[32],
    const uint8_t rcm[32],
    uint64_t value_una,
    uint8_t out_commitment[32]) {
    if (!d || !rcm || !out_commitment) {
        return DINERO_SHIELDED_ERR_INVALID_ARGUMENT;
    }

    try {
        sh::Hash d_hash = CopyHash(d);
        sh::Hash rcm_hash = CopyHash(rcm);
        sh::Hash sk_note = deriv::DeriveNoteSpendKey(rcm_hash);
        const sh::Hash pk_note = sh::PoseidonHash2(sk_note, sh::Hash{});
        const sh::Hash cm = sh::NoteCommitment(d_hash, pk_note,
                                               ValueToHash(value_una),
                                               rcm_hash);
        std::memcpy(out_commitment, cm.data(), cm.size());
        CleanseHash(sk_note);
        return DINERO_SHIELDED_OK;
    } catch (...) {
        return DINERO_SHIELDED_ERR_EXCEPTION;
    }
}

extern "C" int dinero_shielded_compute_nullifier(
    const uint8_t rcm[32],
    uint64_t leaf_index,
    uint8_t out_nullifier[32]) {
    if (!rcm || !out_nullifier) {
        return DINERO_SHIELDED_ERR_INVALID_ARGUMENT;
    }

    try {
        sh::Hash rcm_hash = CopyHash(rcm);
        sh::Hash sk_note = deriv::DeriveNoteSpendKey(rcm_hash);
        const sh::Hash nf = sh::ComputeNullifier(sk_note, leaf_index);
        std::memcpy(out_nullifier, nf.data(), nf.size());
        CleanseHash(sk_note);
        return DINERO_SHIELDED_OK;
    } catch (...) {
        return DINERO_SHIELDED_ERR_EXCEPTION;
    }
}

extern "C" int dinero_shielded_build_unshield_bundle(
    const dinero_shielded_unshield_request* req,
    dinero_shielded_unshield_result* out) {
    if (!out) {
        return DINERO_SHIELDED_ERR_INVALID_ARGUMENT;
    }
    ResetResult(out);

    if (!req || !req->note || !req->serialized_unsigned_tx ||
        req->serialized_unsigned_tx_len == 0) {
        return Fail(out, DINERO_SHIELDED_ERR_INVALID_ARGUMENT,
                    "missing request, note, or serialized unsigned tx");
    }

    try {
        dinero::Transaction tx;
        const std::vector<uint8_t> tx_bytes(
            req->serialized_unsigned_tx,
            req->serialized_unsigned_tx + req->serialized_unsigned_tx_len);

        size_t consumed = 0;
        if (!dinero::TransactionSerializer::Deserialize(tx, tx_bytes, consumed) ||
            consumed != tx_bytes.size()) {
            return Fail(out, DINERO_SHIELDED_ERR_DESERIALIZE_TX,
                        "failed to deserialize unsigned transaction");
        }
        if (req->version != 0 &&
            static_cast<uint8_t>(tx.version) != req->version) {
            return Fail(out, DINERO_SHIELDED_ERR_INVALID_ARGUMENT,
                        "request version does not match serialized tx version");
        }

        const auto* note = req->note;
        sh::Hash rcm = CopyHash(note->rcm);
        sh::Hash sk_note = deriv::DeriveNoteSpendKey(rcm);
        HashCleanser sk_note_guard{&sk_note};

        ops::UnshieldNoteInput input;
        input.secret_key = sk_note;
        input.randomness = rcm;
        input.d = CopyHash(note->d);
        input.anchor = CopyHash(note->anchor);
        input.leaf_index = note->leaf_index;
        input.value_una = note->value_una;
        HashCleanser input_secret_guard{&input.secret_key};
        HashCleanser input_randomness_guard{&input.randomness};
        HashCleanser input_d_guard{&input.d};
        for (size_t i = 0; i < sh::TREE_DEPTH; ++i) {
            input.merkle_path[i] = CopyHash(note->merkle_path[i]);
        }

        auto built = ops::BuildUnshieldBundleForTx(tx, input, req->fee_una);

        if (built.status != ops::OpStatus::Ok) {
            const std::string message = built.error.empty()
                ? "unshield bundle build failed"
                : built.error;
            return Fail(out, DINERO_SHIELDED_ERR_BUILD_FAILED, message);
        }

        return CopyBundleResult(tx, built, out);
    } catch (const std::bad_alloc&) {
        return Fail(out, DINERO_SHIELDED_ERR_ALLOCATION,
                    "allocation failure");
    } catch (const std::exception& e) {
        return Fail(out, DINERO_SHIELDED_ERR_EXCEPTION, e.what());
    } catch (...) {
        return Fail(out, DINERO_SHIELDED_ERR_EXCEPTION,
                    "unknown exception");
    }
}

extern "C" void dinero_shielded_free_result(
    dinero_shielded_unshield_result* out) {
    if (!out) {
        return;
    }
    if (out->bundle_bytes) {
        OPENSSL_cleanse(out->bundle_bytes, out->bundle_len);
    }
    std::free(out->bundle_bytes);
    std::free(out->error);
    ResetResult(out);
}
