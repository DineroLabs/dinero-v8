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

static bool IsValidShieldedHrpAbi(const std::string& hrp) {
    return hrp == deriv::kHrpMainnet || hrp == deriv::kHrpTestnet ||
           hrp == deriv::kHrpRegtest;
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

extern "C" int dinero_shielded_compute_auth_note(
    const uint8_t ak32[32], const uint8_t nvk32[32], const uint8_t d11[11],
    const uint8_t rcm32[32], uint64_t value_una, uint64_t leaf_index,
    uint8_t out_commitment32[32], uint8_t out_nullifier32[32]) {
    if (!ak32 || !nvk32 || !d11 || !rcm32 ||
        !out_commitment32 || !out_nullifier32) {
        return DINERO_SHIELDED_ERR_INVALID_ARGUMENT;
    }
    try {
        const auto ak = CopyHash(ak32);
        auto nvk = CopyHash(nvk32);
        HashCleanser nvk_guard{&nvk};
        auto rcm = CopyHash(rcm32);
        HashCleanser rcm_guard{&rcm};
        deriv::Diversifier d{};
        std::memcpy(d.data(), d11, d.size());
        sh::Hash packed_d{};
        std::memcpy(packed_d.data(), d.data(), d.size());
        const auto pk = deriv::DeriveDiversifiedSpendPublicKey(ak, d);
        auto nfk = deriv::DeriveDiversifiedNullifierKey(nvk, d);
        HashCleanser nfk_guard{&nfk};
        const auto recipient = sh::AuthRecipientCommitmentKey(
            pk, deriv::NullifierKeyCommitment(nfk));
        const auto cm = sh::NoteCommitment(packed_d, recipient, ValueToHash(value_una), rcm);
        const auto nf = sh::ComputeNullifier(nfk, leaf_index);
        std::memcpy(out_commitment32, cm.data(), cm.size());
        std::memcpy(out_nullifier32, nf.data(), nf.size());
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

static int BuildUnshieldBundle(
    const dinero_shielded_unshield_request* req,
    dinero_shielded_unshield_result* out,
    const dinero_shielded_auth_unshield_request* auth) {
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
        HashCleanser rcm_guard{&rcm};
        sh::Hash sk_note{};
        HashCleanser sk_note_guard{&sk_note};
        if (!auth) sk_note = deriv::DeriveNoteSpendKey(rcm);

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
        HashCleanser input_nullifier_guard{&input.nullifier_key};
        if (auth) {
            for (size_t i = 11; i < input.d.size(); ++i) {
                if (input.d[i] != 0) {
                    return Fail(out, DINERO_SHIELDED_ERR_INVALID_ARGUMENT,
                                "Auth diversifier has nonzero padding");
                }
            }
            deriv::Diversifier d{};
            std::memcpy(d.data(), input.d.data(), d.size());
            auto ask = CopyHash(auth->ask32);
            HashCleanser ask_guard{&ask};
            auto nvk = CopyHash(auth->nvk32);
            HashCleanser nvk_guard{&nvk};
            auto spend = deriv::DeriveDiversifiedSpendKey(ask, CopyHash(auth->ak32), d);
            HashCleanser spend_guard{&spend.s};
            input.secret_key = spend.s;
            input.nullifier_key = deriv::DeriveDiversifiedNullifierKey(nvk, d);
            input.key_scheme = dinero::wallet::NoteKeyScheme::Auth;
        }

        for (size_t i = 0; i < sh::TREE_DEPTH; ++i) {
            input.merkle_path[i] = CopyHash(note->merkle_path[i]);
        }

        auto built = ops::BuildUnshieldBundleForTx(tx, input, req->fee_una,
                                                   /*cv_bound=*/auth != nullptr);

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

extern "C" int dinero_shielded_build_unshield_bundle(
    const dinero_shielded_unshield_request* req,
    dinero_shielded_unshield_result* out) {
    return BuildUnshieldBundle(req, out, nullptr);
}

extern "C" int dinero_shielded_build_auth_unshield_bundle(
    const dinero_shielded_auth_unshield_request* req,
    dinero_shielded_unshield_result* out) {
    if (!req) {
        if (out) ResetResult(out);
        return DINERO_SHIELDED_ERR_INVALID_ARGUMENT;
    }
    return BuildUnshieldBundle(&req->base, out, req);
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

extern "C" int32_t dinero_shielded_derive_address(
    const uint8_t* dk32, const uint8_t* ivk32, uint64_t j,
    const char* hrp, char* out_addr, size_t* out_addr_len) {
    (void)dk32;
    (void)ivk32;
    (void)j;
    (void)hrp;
    (void)out_addr;
    (void)out_addr_len;
    return DINERO_SHIELDED_ERR_INVALID_ARGUMENT;
}

extern "C" int32_t dinero_shielded_derive_address_v2(
    const uint8_t* dk32, const uint8_t* ivk32, const uint8_t* ak32,
    const uint8_t* nvk32, uint64_t j, const char* hrp,
    char* out_addr, size_t* out_addr_len) {
    if (!dk32 || !ivk32 || !ak32 || !nvk32 || !hrp || !out_addr ||
        !out_addr_len) {
        return DINERO_SHIELDED_ERR_INVALID_ARGUMENT;
    }

    try {
        const std::string hrp_str(hrp);
        if (!IsValidShieldedHrpAbi(hrp_str)) {
            return DINERO_SHIELDED_ERR_INVALID_ARGUMENT;
        }

        sh::Hash dk = CopyHash(dk32);
        sh::Hash ivk = CopyHash(ivk32);
        sh::Hash ak = CopyHash(ak32);
        sh::Hash nvk = CopyHash(nvk32);
        HashCleanser dk_guard{&dk};
        HashCleanser ivk_guard{&ivk};
        HashCleanser nvk_guard{&nvk};

        const deriv::Diversifier d = deriv::ChaCha20Diversifier(dk, j);
        // Public viewing material can construct and authenticate the address,
        // but cannot derive the spend scalar because ask never crosses ABI.
        const sh::Hash p_d = deriv::HashToPoint(d, deriv::kDstDiv);
        const sh::Hash pk_d = deriv::DerivePkD(ivk, p_d);
        const sh::Hash pk_d_spend =
            deriv::DeriveDiversifiedSpendPublicKey(ak, d);
        sh::Hash nfk = deriv::DeriveDiversifiedNullifierKey(nvk, d);
        HashCleanser nfk_guard{&nfk};
        const sh::Hash nfk_commitment = deriv::NullifierKeyCommitment(nfk);
        const deriv::AddressPayload payload =
            deriv::BuildAddressPayload(d, pk_d, pk_d_spend, nfk_commitment);
        const std::string address =
            deriv::EncodeShieldedAddress(payload, hrp_str);

        const size_t needed = address.size() + 1;  // include NUL terminator
        if (*out_addr_len < needed) {
            *out_addr_len = needed;
            return DINERO_SHIELDED_ERR_BUFFER_TOO_SMALL;
        }

        std::memcpy(out_addr, address.c_str(), needed);
        *out_addr_len = address.size();
        return DINERO_SHIELDED_OK;
    } catch (...) {
        return DINERO_SHIELDED_ERR_EXCEPTION;
    }
}
