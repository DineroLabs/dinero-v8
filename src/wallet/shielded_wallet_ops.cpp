/**
 * Shielded wallet operations — shield + unshield handlers.
 * See include/wallet/shielded_wallet_ops.h.
 */

#include "wallet/shielded_wallet_ops.h"
#include "wallet/shielded_derivation.h"

#include "consensus/shielded/binding_sig.h"
#include "consensus/shielded/bundle_builder.h"
#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/pedersen_commit.h"
#include "consensus/shielded/pedersen_generators.h"
#include "consensus/shielded/shielded_circuit.h"
#include "consensus/shielded/shielded_serialization.h"

#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace dinero::wallet::shielded_ops {

namespace sh = consensus::shielded;

namespace {

// Fails CLOSED. RAND_bytes returns 0 on failure and leaves the buffer
// untouched, so ignoring it yields an ALL-ZERO hash — and this feeds the cv
// blinding factor (rcv) and the range-proof nonce of every bundle the wallet
// builds. With rcv = 0 the commitment degenerates to cv = value*V, a
// deterministic function of the amount that is trivially brute-forced over
// the plausible value range: total loss of shielded confidentiality, silently,
// on a bundle that still verifies. Throwing matches the same module's
// treatment of an RNG failure at shielded_derivation.cpp (esk generation).
sh::Hash RandomHash() {
    sh::Hash h{};
    if (RAND_bytes(h.data(), sh::HASH_BYTES) != 1) {
        throw std::runtime_error("shielded: RAND_bytes failed (refusing to "
                                 "build a bundle with zero blinding)");
    }
    return h;
}

// Encode a uint64 value into a 32-byte Hash matching the Scalar
// big-endian layout used by the shielded circuit (Scalar::Scalar(uint64_t)
// puts the value in data_[24..31]). Phase 2 wave 4 fix: previous
// implementation packed bytes at h[0..7] which the BIG-ENDIAN Scalar
// interpretation turned into an ~2^248-sized field element instead of
// the small value the wallet intended. With range_check_limb gated to
// 64 bits, the old encoding fails the proof; this encoding satisfies it.
sh::Hash ValueToHash(uint64_t v) {
    sh::Hash h{};
    for (int i = 0; i < 8; ++i) {
        h[31 - i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
    }
    return h;
}

// Audit Critical #1: compute the Pedersen value commitment cv = rcv·G + value·V
// for a cv-bound proof's public input. This MUST be byte-identical to the cv
// the bundle publishes, which BuildShieldedBundle forms via the SAME routine
// (sh::PedersenCommit on the SAME (rcv, value)). Using the identical rcv +
// value + PedersenCommit here guarantees the verifier's
// ec_equal(val·V + rcv·G, cv_pub) holds. Returns false if the generator
// derivation is unavailable (callers already gate on PedersenGeneratorsReady).
bool ComputeBundleCv(const sh::Hash& rcv, uint64_t value_una,
                     sh::ValueCommitment& cv_out) {
    return sh::PedersenCommit(rcv, value_una, cv_out) == sh::PedersenResult::Ok;
}

} // namespace

ShieldResult Shield(ShieldParams params,
                    ShieldedNoteStore& store,
                    sh::CommitmentTree& tree) {
    ShieldResult out;

    if (params.value_una == 0) {
        out.status = OpStatus::InvalidParams;
        out.error = "value must be positive";
        return out;
    }

    // 1. Generate fresh secret key
    sh::Hash secret_key = RandomHash();

    // 2. Derive public key: pk = Poseidon(sk, 0)
    sh::Hash zero{};
    sh::Hash public_key = sh::PoseidonHash2(secret_key, zero);

    // 3. Random note randomness
    sh::Hash randomness = RandomHash();

    // 4. Compute commitment
    sh::Hash value_hash = ValueToHash(params.value_una);
    // Phase 2 wave 5: diversifier defaults to zeros until the wallet
    // surfaces shielded addresses (Phase 5). The address-binding tag
    // still applies — every commitment produced under d=zeros is bound
    // to that specific d, so a real-d note constructed later can never
    // collide with a placeholder-d note.
    sh::Hash diversifier{};
    sh::Hash commitment = sh::NoteCommitment(diversifier, public_key, value_hash, randomness);

    // 5. Append to commitment tree (get leaf index)
    uint64_t leaf_index = tree.Append(commitment);

    // 6. Generate output proof
    sh::OutputWitness ow;
    ow.value      = value_hash;
    ow.public_key = public_key;
    ow.randomness = randomness;
    ow.d          = diversifier;

    sh::OutputPublicInputs opi;
    opi.commitment = commitment;

    auto proof = sh::ProveOutput(ow, opi, nullptr);
    if (proof.empty()) {
        out.status = OpStatus::ProofError;
        out.error = "output proof generation failed";
        return out;
    }

    // 7. Store the note
    if (!store.AddNote(params.value_una, secret_key, public_key,
                       randomness, commitment, leaf_index,
                       params.current_height)) {
        out.status = OpStatus::StoreError;
        out.error = "failed to persist shielded note";
        return out;
    }

    out.status       = OpStatus::Ok;
    out.commitment   = commitment;
    out.nullifier_key = secret_key;
    out.leaf_index   = leaf_index;
    out.output_proof = std::move(proof);

    // Scrub secret key from stack (it's now in the store only)
    OPENSSL_cleanse(secret_key.data(), secret_key.size());

    return out;
}

// ── Issue #273: size-aware fee floor ─────────────────────────────────

uint64_t RequiredFeeForTx(const dinero::Transaction& tx, double min_fee_rate) {
    if (min_fee_rate <= 0.0) {
        return kFeeSizingMarginUna;
    }
    const double floor_fee =
        std::ceil(min_fee_rate * static_cast<double>(tx.GetVirtualSize()));
    return static_cast<uint64_t>(floor_fee) + kFeeSizingMarginUna;
}

AttachShieldResult BuildShieldBundleForTx(dinero::Transaction& tx,
                                          uint64_t value_una,
                                          bool cv_bound) {
    AttachShieldResult out;

    if (value_una == 0) {
        out.status = OpStatus::InvalidParams;
        out.error = "value must be positive";
        return out;
    }
    if (!dinero::Transaction::IsShieldedVersion(tx.version)) {
        out.status = OpStatus::InvalidParams;
        out.error = "tx.version must be shielded (5 or 6)";
        return out;
    }

    if (!sh::PedersenGeneratorsReady()) {
        // Lazy-initialize generator lookup. Idempotent; cheap on hot path.
        (void)sh::PedersenGeneratorV();
        if (!sh::PedersenGeneratorsReady()) {
            out.status = OpStatus::InternalError;
            out.error = "pedersen_generators_not_ready";
            return out;
        }
    }

    // 1. Generate a fresh note (sk, pk, randomness, commitment).
    sh::Hash secret_key = RandomHash();
    sh::Hash zero{};
    sh::Hash public_key = sh::PoseidonHash2(secret_key, zero);
    sh::Hash randomness = RandomHash();
    sh::Hash value_hash = ValueToHash(value_una);
    // Phase 2 wave 5: zero diversifier until shielded address surfacing
    // (Phase 5). Address-binding tag still applies.
    sh::Hash diversifier{};
    sh::Hash commitment = sh::NoteCommitment(diversifier, public_key,
                                             value_hash, randomness);

    // 2. Spartan output proof for the new note. Generate the value-commitment
    //    blind up front so the SAME rcv binds the proof's cv (when cv-bound)
    //    AND forms the bundle's published cv below — they MUST be identical.
    sh::Hash rcv = RandomHash();

    sh::OutputWitness ow{};
    ow.value      = value_hash;
    ow.public_key = public_key;
    ow.randomness = randomness;
    ow.d          = diversifier;
    ow.rcv        = rcv;

    sh::OutputPublicInputs opi{};
    opi.commitment = commitment;
    if (cv_bound && !ComputeBundleCv(rcv, value_una, opi.cv)) {
        out.status = OpStatus::InternalError;
        out.error = "cv_commit_failed";
        OPENSSL_cleanse(secret_key.data(), secret_key.size());
        return out;
    }

    auto output_proof = sh::ProveOutput(ow, opi, nullptr,
                                        /*bind_public_inputs=*/true, cv_bound);
    if (output_proof.empty()) {
        out.status = OpStatus::ProofError;
        out.error = "output proof generation failed";
        OPENSSL_cleanse(secret_key.data(), secret_key.size());
        return out;
    }

    // 3. Wire one PlannedOutput (SAME rcv → bundle cv == proof cv).
    sh::PlannedOutput planned{};
    planned.commitment     = commitment;
    planned.value_una      = value_una;
    planned.rcv            = rcv;
    planned.encrypted_note = std::vector<uint8_t>(96, 0);  // TODO Phase 5: real ECDH ciphertext
    planned.output_proof   = std::move(output_proof);
    planned.nonce          = RandomHash();

    // 4. Compute transparent-envelope sighash. tx.shielded_bundle_bytes is
    //    expected empty here; the sighash function only hashes
    //    vins/vouts/locktime/version (not bundle bytes, not witnesses).
    const sh::Hash tx_sighash = sh::ComputeShieldedTxSighash(tx);

    // 5. Build the bundle (no spends, one output, value_balance = +value).
    sh::ShieldedBundle bundle{};
    const auto build_rc = sh::BuildShieldedBundle({}, {planned},
                                                  tx_sighash, bundle);
    if (build_rc != sh::BundleBuildResult::Ok) {
        out.status = OpStatus::InternalError;
        out.error = "build_shielded_bundle_failed:" +
                    std::to_string(static_cast<int>(build_rc));
        OPENSSL_cleanse(secret_key.data(), secret_key.size());
        return out;
    }

    if (bundle.value_balance != static_cast<int64_t>(value_una)) {
        out.status = OpStatus::InternalError;
        out.error = "bundle_value_balance_mismatch";
        OPENSSL_cleanse(secret_key.data(), secret_key.size());
        return out;
    }

    auto bundle_bytes = sh::SerializeShieldedBundle(bundle);
    if (bundle_bytes.empty()) {
        out.status = OpStatus::InternalError;
        out.error = "bundle_serialization_failed";
        OPENSSL_cleanse(secret_key.data(), secret_key.size());
        return out;
    }
    tx.shielded_bundle_bytes = std::move(bundle_bytes);

    out.status        = OpStatus::Ok;
    out.commitment    = commitment;
    out.nullifier_key = secret_key;
    out.public_key    = public_key;
    out.randomness    = randomness;
    out.bundle_bytes  = tx.shielded_bundle_bytes.size();

    OPENSSL_cleanse(secret_key.data(), secret_key.size());
    return out;
}

AttachUnshieldResult BuildUnshieldBundleForTx(dinero::Transaction& tx,
                                              const UnshieldNoteInput& note,
                                              uint64_t fee_una,
                                              bool cv_bound) {
    AttachUnshieldResult out;

    if (note.value_una == 0) {
        out.status = OpStatus::InvalidParams;
        out.error = "note value must be positive";
        return out;
    }
    if (fee_una >= note.value_una) {
        out.status = OpStatus::InvalidParams;
        out.error = "fee must be strictly less than note value";
        return out;
    }
    if (!dinero::Transaction::IsShieldedVersion(tx.version)) {
        out.status = OpStatus::InvalidParams;
        out.error = "tx.version must be shielded (5 or 6)";
        return out;
    }
    if (!tx.vin.empty()) {
        out.status = OpStatus::InvalidParams;
        out.error = "unshield tx must have empty vin";
        return out;
    }
    if (tx.vout.size() != 1) {
        out.status = OpStatus::InvalidParams;
        out.error = "unshield tx must have exactly one transparent recipient vout";
        return out;
    }
    const uint64_t expected_recipient_una = note.value_una - fee_una;
    if (tx.vout[0].value.GetUna() != expected_recipient_una) {
        out.status = OpStatus::InvalidParams;
        out.error = "tx.vout[0].value must equal note.value_una - fee_una";
        return out;
    }

    if (!sh::PedersenGeneratorsReady()) {
        (void)sh::PedersenGeneratorV();
        if (!sh::PedersenGeneratorsReady()) {
            out.status = OpStatus::InternalError;
            out.error = "pedersen_generators_not_ready";
            return out;
        }
    }

    // 1. Reconstruct value_hash to feed the spend witness.
    sh::Hash value_hash = ValueToHash(note.value_una);

    // 2. Generate the Spartan spend proof. Fresh value-commitment blind up
    //    front so the SAME rcv binds the proof's cv (when cv-bound) AND forms
    //    the bundle's published cv below.
    sh::Hash rcv = RandomHash();

    sh::SpendWitness sw{};
    sw.secret_key  = note.secret_key;
    sw.leaf_index  = note.leaf_index;
    sw.value       = value_hash;
    sw.randomness  = note.randomness;
    sw.d           = note.d;
    sw.rcv         = rcv;
    sw.merkle_path = note.merkle_path;

    sh::SpendPublicInputs spi{};
    spi.nullifier = sh::ComputeNullifier(note.secret_key, note.leaf_index);
    spi.anchor    = note.anchor;
    if (cv_bound && !ComputeBundleCv(rcv, note.value_una, spi.cv)) {
        out.status = OpStatus::InternalError;
        out.error = "cv_commit_failed";
        return out;
    }

    // The proof variant must match the NOTE's convention, not a separately
    // passed flag: an auth note is unspendable by the legacy circuit and
    // vice-versa. Derived from the note so the two cannot drift apart.
    const bool note_spend_auth =
        (note.key_scheme == NoteKeyScheme::Auth);
    auto spend_proof = sh::ProveSpend(sw, spi, nullptr,
                                      /*bind_public_inputs=*/true, cv_bound,
                                      note_spend_auth);
    if (spend_proof.empty()) {
        out.status = OpStatus::ProofError;
        out.error = "spend proof generation failed";
        return out;
    }

    // 3. Wire the PlannedSpend (SAME rcv → bundle cv == proof cv).
    sh::PlannedSpend planned{};
    planned.nullifier   = spi.nullifier;
    planned.anchor      = spi.anchor;
    planned.value_una   = note.value_una;
    planned.rcv         = rcv;
    planned.spend_proof = std::move(spend_proof);
    planned.nonce       = RandomHash();

    // 4. Compute transparent-envelope sighash (commits to vout, locktime,
    //    version, explicit_fee — recipient swap or fee tweak invalidates).
    const sh::Hash tx_sighash = sh::ComputeShieldedTxSighash(tx);

    // 5. Build bundle: one spend, zero outputs, value_balance = -note_value.
    sh::ShieldedBundle bundle{};
    const auto build_rc = sh::BuildShieldedBundle({planned}, {},
                                                  tx_sighash, bundle);
    if (build_rc != sh::BundleBuildResult::Ok) {
        out.status = OpStatus::InternalError;
        out.error = "build_shielded_bundle_failed:" +
                    std::to_string(static_cast<int>(build_rc));
        return out;
    }

    if (bundle.value_balance != -static_cast<int64_t>(note.value_una)) {
        out.status = OpStatus::InternalError;
        out.error = "bundle_value_balance_mismatch";
        return out;
    }

    auto bundle_bytes = sh::SerializeShieldedBundle(bundle);
    if (bundle_bytes.empty()) {
        out.status = OpStatus::InternalError;
        out.error = "bundle_serialization_failed";
        return out;
    }
    tx.shielded_bundle_bytes = std::move(bundle_bytes);

    out.status       = OpStatus::Ok;
    out.nullifier    = spi.nullifier;
    out.anchor       = spi.anchor;
    out.bundle_bytes = tx.shielded_bundle_bytes.size();
    return out;
}

UnshieldResult Unshield(UnshieldParams params,
                        ShieldedNoteStore& store,
                        const sh::CommitmentTree& tree) {
    UnshieldResult out;

    auto note_opt = store.GetByLeafIndex(params.leaf_index);
    if (!note_opt) {
        out.status = OpStatus::InvalidParams;
        out.error = "no note at leaf_index " + std::to_string(params.leaf_index);
        return out;
    }
    const auto& note = *note_opt;

    if (note.spent) {
        out.status = OpStatus::InvalidParams;
        out.error = "note already spent";
        return out;
    }

    // Build spend witness
    sh::SpendWitness sw;
    sw.secret_key = note.secret_key;
    sw.leaf_index = note.leaf_index;
    sw.value      = ValueToHash(note.value_una);
    sw.randomness = note.randomness;
    sw.d.fill(0);  // Phase 2 wave 5: matches the d used at shield time.
    auto auth_path = tree.GetAuthPath(note.leaf_index);
    if (!auth_path) {
        out.status = OpStatus::ProofError;
        out.error = "failed to build Merkle authentication path";
        return out;
    }
    sw.merkle_path = auth_path->siblings;

    sh::SpendPublicInputs spi;
    spi.nullifier = sh::ComputeNullifier(note.secret_key, note.leaf_index);
    spi.anchor    = tree.Root();

    auto proof = sh::ProveSpend(sw, spi, nullptr);
    if (proof.empty()) {
        out.status = OpStatus::ProofError;
        out.error = "spend proof generation failed";
        return out;
    }

    // Mark spent in wallet store
    store.MarkSpent(note.leaf_index);

    out.status      = OpStatus::Ok;
    out.nullifier   = spi.nullifier;
    out.anchor      = spi.anchor;
    out.value_una   = note.value_una;
    out.spend_proof = std::move(proof);

    return out;
}

// ── Phase 3 wave 3d: shielded→shielded self-transfer ─────────────────

AttachTransferResult BuildTransferBundleForTx(dinero::Transaction& tx,
                                              const UnshieldNoteInput& note,
                                              uint64_t fee_una,
                                              bool cv_bound) {
    AttachTransferResult out;

    if (note.value_una == 0) {
        out.status = OpStatus::InvalidParams;
        out.error = "note value must be positive";
        return out;
    }
    if (fee_una >= note.value_una) {
        out.status = OpStatus::InvalidParams;
        out.error = "fee must be strictly less than note value";
        return out;
    }
    if (!dinero::Transaction::IsShieldedVersion(tx.version)) {
        out.status = OpStatus::InvalidParams;
        out.error = "tx.version must be shielded (5 or 6)";
        return out;
    }
    if (!tx.vin.empty()) {
        out.status = OpStatus::InvalidParams;
        out.error = "transfer tx must have empty vin";
        return out;
    }
    if (!tx.vout.empty()) {
        out.status = OpStatus::InvalidParams;
        out.error = "transfer tx must have empty vout";
        return out;
    }

    if (!sh::PedersenGeneratorsReady()) {
        (void)sh::PedersenGeneratorV();
        if (!sh::PedersenGeneratorsReady()) {
            out.status = OpStatus::InternalError;
            out.error = "pedersen_generators_not_ready";
            return out;
        }
    }

    const uint64_t out_value_una = note.value_una - fee_una;

    // ── Spend side: reconstruct pk + value_hash, generate spend proof.
    sh::Hash zero{};
    sh::Hash output_d{};
    sh::Hash spend_pk = sh::PoseidonHash2(note.secret_key, zero);
    sh::Hash spend_value_hash = ValueToHash(note.value_una);
    sh::Hash spend_d = note.d;

    sh::Hash spend_rcv = RandomHash();
    sh::SpendWitness sw{};
    sw.secret_key  = note.secret_key;
    sw.leaf_index  = note.leaf_index;
    sw.value       = spend_value_hash;
    sw.randomness  = note.randomness;
    sw.d           = spend_d;
    sw.rcv         = spend_rcv;
    sw.merkle_path = note.merkle_path;

    sh::SpendPublicInputs spi{};
    spi.nullifier = sh::ComputeNullifier(note.secret_key, note.leaf_index);
    spi.anchor    = note.anchor;
    if (cv_bound && !ComputeBundleCv(spend_rcv, note.value_una, spi.cv)) {
        out.status = OpStatus::InternalError;
        out.error = "cv_commit_failed";
        return out;
    }

    // The proof variant must match the NOTE's convention, not a separately
    // passed flag: an auth note is unspendable by the legacy circuit and
    // vice-versa. Derived from the note so the two cannot drift apart.
    const bool note_spend_auth =
        (note.key_scheme == NoteKeyScheme::Auth);
    auto spend_proof = sh::ProveSpend(sw, spi, nullptr,
                                      /*bind_public_inputs=*/true, cv_bound,
                                      note_spend_auth);
    if (spend_proof.empty()) {
        out.status = OpStatus::ProofError;
        out.error = "spend proof generation failed";
        return out;
    }
    (void)spend_pk;  // pk derived inside circuit; only used for self-check

    sh::PlannedSpend planned_spend{};
    planned_spend.nullifier   = spi.nullifier;
    planned_spend.anchor      = spi.anchor;
    planned_spend.value_una   = note.value_una;
    planned_spend.rcv         = spend_rcv;
    planned_spend.spend_proof = std::move(spend_proof);
    planned_spend.nonce       = RandomHash();

    // ── Output side: fresh self-note (sk, pk, randomness, commitment).
    sh::Hash out_secret_key = RandomHash();
    sh::Hash out_public_key = sh::PoseidonHash2(out_secret_key, zero);
    sh::Hash out_randomness = RandomHash();
    sh::Hash out_value_hash = ValueToHash(out_value_una);
    sh::Hash out_d{};
    sh::Hash out_commitment = sh::NoteCommitment(out_d, out_public_key,
                                                 out_value_hash, out_randomness);

    sh::Hash out_rcv = RandomHash();
    sh::OutputWitness ow{};
    ow.value      = out_value_hash;
    ow.public_key = out_public_key;
    ow.randomness = out_randomness;
    ow.d          = out_d;
    ow.rcv        = out_rcv;

    sh::OutputPublicInputs opi{};
    opi.commitment = out_commitment;
    if (cv_bound && !ComputeBundleCv(out_rcv, out_value_una, opi.cv)) {
        out.status = OpStatus::InternalError;
        out.error = "cv_commit_failed";
        OPENSSL_cleanse(out_secret_key.data(), out_secret_key.size());
        return out;
    }

    auto output_proof = sh::ProveOutput(ow, opi, nullptr,
                                        /*bind_public_inputs=*/true, cv_bound);
    if (output_proof.empty()) {
        out.status = OpStatus::ProofError;
        out.error = "output proof generation failed";
        OPENSSL_cleanse(out_secret_key.data(), out_secret_key.size());
        return out;
    }

    sh::PlannedOutput planned_output{};
    planned_output.commitment     = out_commitment;
    planned_output.value_una      = out_value_una;
    planned_output.rcv            = out_rcv;
    planned_output.encrypted_note = std::vector<uint8_t>(96, 0);  // TODO Phase 5
    planned_output.output_proof   = std::move(output_proof);
    planned_output.nonce          = RandomHash();

    // ── Sighash: empty vin/vout, locktime, version, explicit_fee.
    const sh::Hash tx_sighash = sh::ComputeShieldedTxSighash(tx);

    // ── Bundle: one spend, one output, value_balance = -fee_una.
    //    sum(out) - sum(spend) = (note_value - fee) - note_value = -fee.
    sh::ShieldedBundle bundle{};
    const auto build_rc = sh::BuildShieldedBundle({planned_spend},
                                                  {planned_output},
                                                  tx_sighash, bundle);
    if (build_rc != sh::BundleBuildResult::Ok) {
        out.status = OpStatus::InternalError;
        out.error = "build_shielded_bundle_failed:" +
                    std::to_string(static_cast<int>(build_rc));
        OPENSSL_cleanse(out_secret_key.data(), out_secret_key.size());
        return out;
    }

    if (bundle.value_balance != -static_cast<int64_t>(fee_una)) {
        out.status = OpStatus::InternalError;
        out.error = "bundle_value_balance_mismatch";
        OPENSSL_cleanse(out_secret_key.data(), out_secret_key.size());
        return out;
    }

    auto bundle_bytes = sh::SerializeShieldedBundle(bundle);
    if (bundle_bytes.empty()) {
        out.status = OpStatus::InternalError;
        out.error = "bundle_serialization_failed";
        OPENSSL_cleanse(out_secret_key.data(), out_secret_key.size());
        return out;
    }
    tx.shielded_bundle_bytes = std::move(bundle_bytes);

    out.status         = OpStatus::Ok;
    out.spend_nullifier = spi.nullifier;
    out.spend_anchor    = spi.anchor;
    out.out_commitment  = out_commitment;
    out.out_secret_key  = out_secret_key;
    out.out_public_key  = out_public_key;
    out.out_randomness  = out_randomness;
    out.out_value_una   = out_value_una;
    out.bundle_bytes    = tx.shielded_bundle_bytes.size();
    return out;
}

// ── Phase 3 wave 3e: multi-spend + change-output self-transfer ───────

AttachMultiTransferResult BuildMultiTransferBundleForTx(
    dinero::Transaction& tx,
    const std::vector<UnshieldNoteInput>& spends,
    const std::vector<uint64_t>& output_values,
    uint64_t fee_una,
    bool cv_bound) {
    AttachMultiTransferResult out;

    if (spends.empty()) {
        out.status = OpStatus::InvalidParams;
        out.error = "at least one spend required";
        return out;
    }
    if (output_values.empty()) {
        out.status = OpStatus::InvalidParams;
        out.error = "at least one output required (use 3c unshield for zero-output)";
        return out;
    }
    if (spends.size() > sh::kMaxSpendsPerBundle) {
        out.status = OpStatus::InvalidParams;
        out.error = "spends exceed kMaxSpendsPerBundle";
        return out;
    }
    if (output_values.size() > sh::kMaxOutputsPerBundle) {
        out.status = OpStatus::InvalidParams;
        out.error = "outputs exceed kMaxOutputsPerBundle";
        return out;
    }
    if (!dinero::Transaction::IsShieldedVersion(tx.version)) {
        out.status = OpStatus::InvalidParams;
        out.error = "tx.version must be shielded (5 or 6)";
        return out;
    }
    if (!tx.vin.empty() || !tx.vout.empty()) {
        out.status = OpStatus::InvalidParams;
        out.error = "transfer tx must have empty vin/vout";
        return out;
    }

    // Balance check: sum(spend) == sum(output) + fee.
    uint64_t spend_sum = 0;
    for (const auto& s : spends) {
        if (s.value_una == 0) {
            out.status = OpStatus::InvalidParams;
            out.error = "spend value must be positive";
            return out;
        }
        spend_sum += s.value_una;
    }
    uint64_t out_sum = 0;
    for (uint64_t v : output_values) {
        if (v == 0) {
            out.status = OpStatus::InvalidParams;
            out.error = "output value must be positive";
            return out;
        }
        out_sum += v;
    }
    if (spend_sum != out_sum + fee_una) {
        out.status = OpStatus::InvalidParams;
        out.error = "sum(spend) != sum(output) + fee";
        return out;
    }

    if (!sh::PedersenGeneratorsReady()) {
        (void)sh::PedersenGeneratorV();
        if (!sh::PedersenGeneratorsReady()) {
            out.status = OpStatus::InternalError;
            out.error = "pedersen_generators_not_ready";
            return out;
        }
    }

    sh::Hash zero{};
    sh::Hash output_d{};
    // ── Spend side: per-note Spartan spend proof + PlannedSpend.
    std::vector<sh::PlannedSpend> planned_spends;
    planned_spends.reserve(spends.size());
    for (const auto& s : spends) {
        sh::Hash value_hash = ValueToHash(s.value_una);
        sh::Hash s_rcv = RandomHash();
        sh::SpendWitness sw{};
        sw.secret_key  = s.secret_key;
        sw.leaf_index  = s.leaf_index;
        sw.value       = value_hash;
        sw.randomness  = s.randomness;
        sw.d           = s.d;
        sw.rcv         = s_rcv;
        sw.merkle_path = s.merkle_path;

        sh::SpendPublicInputs spi{};
        spi.nullifier = sh::ComputeNullifier(s.secret_key, s.leaf_index);
        spi.anchor    = s.anchor;
        if (cv_bound && !ComputeBundleCv(s_rcv, s.value_una, spi.cv)) {
            out.status = OpStatus::InternalError;
            out.error = "cv_commit_failed";
            return out;
        }

        const bool s_spend_auth = (s.key_scheme == NoteKeyScheme::Auth);
        auto proof = sh::ProveSpend(sw, spi, nullptr,
                                    /*bind_public_inputs=*/true, cv_bound,
                                    s_spend_auth);
        if (proof.empty()) {
            out.status = OpStatus::ProofError;
            out.error = "spend proof generation failed";
            return out;
        }
        sh::PlannedSpend ps{};
        ps.nullifier   = spi.nullifier;
        ps.anchor      = spi.anchor;
        ps.value_una   = s.value_una;
        ps.rcv         = s_rcv;
        ps.spend_proof = std::move(proof);
        ps.nonce       = RandomHash();
        out.spend_nullifiers.push_back(spi.nullifier);
        out.spend_anchors.push_back(spi.anchor);
        planned_spends.push_back(std::move(ps));
    }

    // ── Output side: fresh self-note per output value.
    std::vector<sh::PlannedOutput> planned_outputs;
    planned_outputs.reserve(output_values.size());
    out.outputs.reserve(output_values.size());
    for (uint64_t value_una : output_values) {
        TransferOutputMaterial mat{};
        mat.secret_key = RandomHash();
        mat.public_key = sh::PoseidonHash2(mat.secret_key, zero);
        mat.randomness = RandomHash();
        mat.value_una  = value_una;
        sh::Hash value_hash = ValueToHash(value_una);
        mat.commitment = sh::NoteCommitment(output_d, mat.public_key,
                                            value_hash, mat.randomness);

        sh::Hash o_rcv = RandomHash();
        sh::OutputWitness ow{};
        ow.value      = value_hash;
        ow.public_key = mat.public_key;
        ow.randomness = mat.randomness;
        ow.d          = output_d;
        ow.rcv        = o_rcv;

        sh::OutputPublicInputs opi{};
        opi.commitment = mat.commitment;
        if (cv_bound && !ComputeBundleCv(o_rcv, value_una, opi.cv)) {
            out.status = OpStatus::InternalError;
            out.error = "cv_commit_failed";
            for (auto& m : out.outputs) {
                OPENSSL_cleanse(m.secret_key.data(), m.secret_key.size());
            }
            OPENSSL_cleanse(mat.secret_key.data(), mat.secret_key.size());
            return out;
        }

        auto proof = sh::ProveOutput(ow, opi, nullptr,
                                     /*bind_public_inputs=*/true, cv_bound);
        if (proof.empty()) {
            out.status = OpStatus::ProofError;
            out.error = "output proof generation failed";
            for (auto& m : out.outputs) {
                OPENSSL_cleanse(m.secret_key.data(), m.secret_key.size());
            }
            OPENSSL_cleanse(mat.secret_key.data(), mat.secret_key.size());
            return out;
        }

        sh::PlannedOutput po{};
        po.commitment     = mat.commitment;
        po.value_una      = value_una;
        po.rcv            = o_rcv;
        po.encrypted_note = std::vector<uint8_t>(96, 0);  // TODO Phase 5
        po.output_proof   = std::move(proof);
        po.nonce          = RandomHash();
        planned_outputs.push_back(std::move(po));
        out.outputs.push_back(std::move(mat));
    }

    // ── Sighash + bundle.
    const sh::Hash tx_sighash = sh::ComputeShieldedTxSighash(tx);
    sh::ShieldedBundle bundle{};
    const auto build_rc = sh::BuildShieldedBundle(planned_spends, planned_outputs,
                                                  tx_sighash, bundle);
    if (build_rc != sh::BundleBuildResult::Ok) {
        out.status = OpStatus::InternalError;
        out.error = "build_shielded_bundle_failed:" +
                    std::to_string(static_cast<int>(build_rc));
        for (auto& m : out.outputs) {
            OPENSSL_cleanse(m.secret_key.data(), m.secret_key.size());
        }
        return out;
    }
    if (bundle.value_balance != -static_cast<int64_t>(fee_una)) {
        out.status = OpStatus::InternalError;
        out.error = "bundle_value_balance_mismatch";
        for (auto& m : out.outputs) {
            OPENSSL_cleanse(m.secret_key.data(), m.secret_key.size());
        }
        return out;
    }

    auto bundle_bytes = sh::SerializeShieldedBundle(bundle);
    if (bundle_bytes.empty()) {
        out.status = OpStatus::InternalError;
        out.error = "bundle_serialization_failed";
        for (auto& m : out.outputs) {
            OPENSSL_cleanse(m.secret_key.data(), m.secret_key.size());
        }
        return out;
    }
    tx.shielded_bundle_bytes = std::move(bundle_bytes);

    out.status       = OpStatus::Ok;
    out.bundle_bytes = tx.shielded_bundle_bytes.size();
    return out;
}

// ── Phase 5 Wave 3d: addressed transfer (any-recipient + self change) ─

namespace shdrv = ::dinero::wallet::shielded;

// Shared addressed-recipient output construction. Extracted verbatim from
// BuildAddressedTransferBundleForTx so the transfer path AND the
// shield-to-recipient path share ONE definition of the commitment /
// encryption / proof convention. Keeping the RandomHash() call sequence
// (rcm[if random] → rcv → nonce) preserves the transfer builder's exact
// behavior.
AddressedRecipientOutput BuildAddressedRecipientOutput(
    const AddressedRecipient& recipient,
    const std::array<uint8_t, 512>* recipient_memo,
    bool cv_bound,
    bool spend_auth,
    const sh::Hash* rcm_override,
    const sh::Hash* esk_override) {
    AddressedRecipientOutput out;

    if (recipient.value_una == 0) {
        out.status = OpStatus::InvalidParams;
        out.error = "recipient value must be positive";
        return out;
    }

    sh::Hash zero{};
    //   d = recipient.d (packed into 32-byte hash for the on-chain
    //                    commitment formula)
    //   commitment = NoteCommitment(d_packed, pk_note, value, rcm)
    //   encrypted_note = EncryptNoteForRecipient(d, pk_d, plaintext)
    //
    // pk_note depends on the spend-authority rule:
    //
    //   LEGACY (spend_auth = false): pk_note = Poseidon(DeriveNoteSpendKey(rcm), 0).
    //     `rcm` is chosen HERE, by the sender, so the sender knows the spend key
    //     of the note they just sent and can spend it out from under the
    //     recipient at any time. The recipient's pk_d is used only to encrypt.
    //
    //   AUTH (spend_auth = true): pk_note = recipient.pk_d, straight from their
    //     address. Spending it requires `s` with s·G = pk_d, which only the
    //     recipient can derive (s = Poseidon(ivk, d)). The sender never learns
    //     it, so the note stops being sender-spendable.
    //
    // `rcm` stays random and stays in the plaintext either way — under AUTH it
    // is only the commitment's blinding factor, no longer a spend key.
    sh::Hash recipient_rcm = rcm_override ? *rcm_override : RandomHash();
    sh::Hash recipient_pk;
    if (spend_auth) {
        // Commit to the recipient's SPEND key, s·G, taken from their address.
        // Spending requires `s` = Poseidon(ivk, d), which only the recipient
        // can derive, so the sender cannot spend the note they just sent.
        //
        // NOT recipient.pk_d — that is ivk·P_d, the DISCOVERY key. Committing
        // to it would demand dlog_G(ivk·P_d) from the spender, which nobody
        // knows (dlog_G(P_d) is unknown by hash-to-point construction), making
        // the note unspendable by everyone. The two keys are 32 bytes apart in
        // the address payload and are NOT interchangeable.
        if (recipient.pk_d_spend == sh::Hash{}) {
            out.status = OpStatus::InvalidParams;
            out.error = "spend_auth_requires_pk_d_spend";
            return out;
        }
        recipient_pk = recipient.pk_d_spend;
    } else {
        sh::Hash recipient_sk = shdrv::DeriveNoteSpendKey(recipient_rcm);
        recipient_pk = sh::PoseidonHash2(recipient_sk, zero);
        OPENSSL_cleanse(recipient_sk.data(), recipient_sk.size());
    }
    sh::Hash recipient_value_hash = ValueToHash(recipient.value_una);
    sh::Hash recipient_d_packed{};
    std::memcpy(recipient_d_packed.data(), recipient.d.data(), recipient.d.size());
    sh::Hash recipient_commitment = sh::NoteCommitment(
        recipient_d_packed, recipient_pk,
        recipient_value_hash, recipient_rcm);

    sh::Hash recipient_rcv = RandomHash();
    sh::OutputWitness recipient_ow{};
    recipient_ow.value      = recipient_value_hash;
    recipient_ow.public_key = recipient_pk;
    recipient_ow.randomness = recipient_rcm;
    recipient_ow.d          = recipient_d_packed;
    recipient_ow.rcv        = recipient_rcv;
    sh::OutputPublicInputs recipient_opi{};
    recipient_opi.commitment = recipient_commitment;
    if (cv_bound && !ComputeBundleCv(recipient_rcv, recipient.value_una, recipient_opi.cv)) {
        out.status = OpStatus::InternalError;
        out.error = "cv_commit_failed";
        return out;
    }
    auto recipient_proof = sh::ProveOutput(recipient_ow, recipient_opi, nullptr,
                                           /*bind_public_inputs=*/true, cv_bound);
    if (recipient_proof.empty()) {
        out.status = OpStatus::ProofError;
        out.error = "recipient output proof generation failed";
        return out;
    }

    // Build encrypted_note for the recipient.
    shdrv::Diversifier dvf{};
    std::memcpy(dvf.data(), recipient.d.data(), recipient.d.size());
    shdrv::NotePlaintext nplain;
    nplain.d         = dvf;
    nplain.value_una = recipient.value_una;
    nplain.rcm       = recipient_rcm;
    if (recipient_memo != nullptr) {
        std::memcpy(nplain.memo.data(), recipient_memo->data(), nplain.memo.size());
    }
    shdrv::EncryptedNote enc_bytes;
    try {
        enc_bytes = shdrv::EncryptNoteForRecipient(dvf, recipient.pk_d, nplain,
                                                   esk_override);
    } catch (const std::exception& e) {
        out.status = OpStatus::InternalError;
        out.error = std::string("encrypt_note_failed: ") + e.what();
        return out;
    }

    sh::PlannedOutput planned{};
    planned.commitment     = recipient_commitment;
    planned.value_una      = recipient.value_una;
    planned.rcv            = recipient_rcv;
    planned.encrypted_note =
        std::vector<uint8_t>(enc_bytes.begin(), enc_bytes.end());
    planned.output_proof   = std::move(recipient_proof);
    planned.nonce          = RandomHash();

    out.status     = OpStatus::Ok;
    out.commitment = recipient_commitment;
    out.randomness = recipient_rcm;
    out.planned    = std::move(planned);
    return out;
}

AttachAddressedTransferResult BuildAddressedTransferBundleForTx(
    dinero::Transaction& tx,
    const std::vector<UnshieldNoteInput>& spends,
    const AddressedRecipient& recipient,
    uint64_t change_value_una,
    uint64_t fee_una,
    const std::array<uint8_t, 512>* recipient_memo,
    bool cv_bound,
    bool spend_auth,
    const AddressedRecipient* change_recipient) {
    AttachAddressedTransferResult out;

    if (spends.empty()) {
        out.status = OpStatus::InvalidParams;
        out.error = "at least one spend required";
        return out;
    }
    if (recipient.value_una == 0) {
        out.status = OpStatus::InvalidParams;
        out.error = "recipient value must be positive";
        return out;
    }
    if (!dinero::Transaction::IsShieldedVersion(tx.version)) {
        out.status = OpStatus::InvalidParams;
        out.error = "tx.version must be shielded (5 or 6)";
        return out;
    }
    if (!tx.vin.empty() || !tx.vout.empty()) {
        out.status = OpStatus::InvalidParams;
        out.error = "transfer tx must have empty vin/vout";
        return out;
    }

    uint64_t spend_sum = 0;
    for (const auto& s : spends) spend_sum += s.value_una;
    if (spend_sum != recipient.value_una + change_value_una + fee_una) {
        out.status = OpStatus::InvalidParams;
        out.error = "sum(spend) != recipient + change + fee";
        return out;
    }

    if (!sh::PedersenGeneratorsReady()) {
        (void)sh::PedersenGeneratorV();
        if (!sh::PedersenGeneratorsReady()) {
            out.status = OpStatus::InternalError;
            out.error = "pedersen_generators_not_ready";
            return out;
        }
    }

    sh::Hash zero{};

    // ── Spends: per-note Spartan spend proofs.
    std::vector<sh::PlannedSpend> planned_spends;
    planned_spends.reserve(spends.size());
    for (const auto& s : spends) {
        sh::Hash value_hash = ValueToHash(s.value_una);
        sh::Hash s_rcv = RandomHash();
        sh::SpendWitness sw{};
        sw.secret_key  = s.secret_key;
        sw.leaf_index  = s.leaf_index;
        sw.value       = value_hash;
        sw.randomness  = s.randomness;
        sw.d           = s.d;
        sw.rcv         = s_rcv;
        sw.merkle_path = s.merkle_path;
        sh::SpendPublicInputs spi{};
        spi.nullifier = sh::ComputeNullifier(s.secret_key, s.leaf_index);
        spi.anchor    = s.anchor;
        if (cv_bound && !ComputeBundleCv(s_rcv, s.value_una, spi.cv)) {
            out.status = OpStatus::InternalError;
            out.error = "cv_commit_failed";
            return out;
        }
        const bool s_spend_auth = (s.key_scheme == NoteKeyScheme::Auth);
        auto proof = sh::ProveSpend(sw, spi, nullptr,
                                    /*bind_public_inputs=*/true, cv_bound,
                                    s_spend_auth);
        if (proof.empty()) {
            out.status = OpStatus::ProofError;
            out.error = "spend proof generation failed";
            return out;
        }
        sh::PlannedSpend ps{};
        ps.nullifier   = spi.nullifier;
        ps.anchor      = spi.anchor;
        ps.value_una   = s.value_una;
        ps.rcv         = s_rcv;
        ps.spend_proof = std::move(proof);
        ps.nonce       = RandomHash();
        out.spend_nullifiers.push_back(spi.nullifier);
        planned_spends.push_back(std::move(ps));
    }

    // ── Recipient output (addressed): built by the shared helper so the
    //    commitment / encryption / proof convention has ONE definition
    //    (reused by the shield-to-recipient path).
    auto rout = BuildAddressedRecipientOutput(recipient, recipient_memo,
                                              cv_bound, spend_auth);
    if (rout.status != OpStatus::Ok) {
        out.status = rout.status;
        out.error  = rout.error;
        return out;
    }
    const sh::Hash recipient_commitment = rout.commitment;

    std::vector<sh::PlannedOutput> planned_outputs;
    planned_outputs.reserve(2);
    planned_outputs.push_back(std::move(rout.planned));

    // ── Change (legacy self-recipient style; receivable via existing
    //    pending-note bookkeeping — wallet wrapper passes the secret
    //    material to AddPendingNote). Migrates to addressed in 3e.
    sh::Hash change_secret_key{};
    sh::Hash change_public_key{};
    sh::Hash change_randomness{};
    sh::Hash change_commitment{};
    sh::Hash change_d{};
    NoteKeyScheme change_key_scheme = NoteKeyScheme::LegacySenderKey;
    if (change_value_una > 0) {
        if (spend_auth) {
            if (change_recipient == nullptr ||
                change_recipient->value_una != change_value_una) {
                out.status = OpStatus::InvalidParams;
                out.error = "spend_auth_requires_addressed_change";
                return out;
            }
            auto change_out = BuildAddressedRecipientOutput(
                *change_recipient, nullptr, cv_bound, true);
            if (change_out.status != OpStatus::Ok) {
                out.status = change_out.status;
                out.error = "change_" + change_out.error;
                return out;
            }
            change_commitment = change_out.commitment;
            change_randomness = change_out.randomness;
            change_public_key = change_recipient->pk_d_spend;
            std::memcpy(change_d.data(), change_recipient->d.data(),
                        change_recipient->d.size());
            change_key_scheme = NoteKeyScheme::Auth;
            planned_outputs.push_back(std::move(change_out.planned));
        } else {
            change_secret_key = RandomHash();
            change_public_key = sh::PoseidonHash2(change_secret_key, zero);
            change_randomness = RandomHash();
            sh::Hash change_value_hash = ValueToHash(change_value_una);
            change_commitment = sh::NoteCommitment(
                zero, change_public_key, change_value_hash, change_randomness);

            sh::Hash change_rcv = RandomHash();
            sh::OutputWitness ow{};
            ow.value      = change_value_hash;
            ow.public_key = change_public_key;
            ow.randomness = change_randomness;
            ow.d          = zero;
            ow.rcv        = change_rcv;
            sh::OutputPublicInputs opi{};
            opi.commitment = change_commitment;
            if (cv_bound && !ComputeBundleCv(change_rcv, change_value_una, opi.cv)) {
                OPENSSL_cleanse(change_secret_key.data(), change_secret_key.size());
                out.status = OpStatus::InternalError;
                out.error = "cv_commit_failed";
                return out;
            }
            auto proof = sh::ProveOutput(ow, opi, nullptr,
                                         /*bind_public_inputs=*/true, cv_bound);
            if (proof.empty()) {
                OPENSSL_cleanse(change_secret_key.data(), change_secret_key.size());
                out.status = OpStatus::ProofError;
                out.error = "change output proof generation failed";
                return out;
            }
            sh::PlannedOutput cpo{};
            cpo.commitment     = change_commitment;
            cpo.value_una      = change_value_una;
            cpo.rcv            = change_rcv;
            cpo.encrypted_note = std::vector<uint8_t>(96, 0);
            cpo.output_proof   = std::move(proof);
            cpo.nonce          = RandomHash();
            planned_outputs.push_back(std::move(cpo));
        }
    }

    const sh::Hash tx_sighash = sh::ComputeShieldedTxSighash(tx);
    sh::ShieldedBundle bundle{};
    const auto build_rc = sh::BuildShieldedBundle(planned_spends, planned_outputs,
                                                  tx_sighash, bundle);
    if (build_rc != sh::BundleBuildResult::Ok) {
        OPENSSL_cleanse(change_secret_key.data(), change_secret_key.size());
        out.status = OpStatus::InternalError;
        out.error = "build_shielded_bundle_failed:" +
                    std::to_string(static_cast<int>(build_rc));
        return out;
    }
    if (bundle.value_balance != -static_cast<int64_t>(fee_una)) {
        OPENSSL_cleanse(change_secret_key.data(), change_secret_key.size());
        out.status = OpStatus::InternalError;
        out.error = "bundle_value_balance_mismatch";
        return out;
    }

    auto bundle_bytes = sh::SerializeShieldedBundle(bundle);
    if (bundle_bytes.empty()) {
        OPENSSL_cleanse(change_secret_key.data(), change_secret_key.size());
        out.status = OpStatus::InternalError;
        out.error = "bundle_serialization_failed";
        return out;
    }
    tx.shielded_bundle_bytes = std::move(bundle_bytes);

    out.status               = OpStatus::Ok;
    out.recipient_commitment = recipient_commitment;
    out.had_change           = (change_value_una > 0);
    out.change_commitment    = change_commitment;
    out.change_value_una     = change_value_una;
    out.bundle_bytes         = tx.shielded_bundle_bytes.size();
    // Stash secret material for caller to persist via AddPendingNote.
    // We use the existing AttachShieldResult shape only via the wallet
    // wrapper; the pure helper just hands them back via a side-channel
    // through the local change_* variables it returns through the
    // wallet-bound wrapper. For the pure helper, expose just the
    // commitment hashes; secret material is regenerated by the wrapper
    // (which knows the wallet) — but we need to keep change_secret_key
    // alive so the wrapper can stash it. For now, expose via OPENSSL
    // boundary: the wrapper doesn't need it because we pass through.
    // Solution: extend AttachAddressedTransferResult to carry it.
    // (See header — change_* fields below.)
    out.change_secret_key = change_secret_key;
    out.change_public_key = change_public_key;
    out.change_randomness = change_randomness;
    out.change_d = change_d;
    out.change_key_scheme = change_key_scheme;
    OPENSSL_cleanse(change_secret_key.data(), change_secret_key.size());
    return out;
}

// ── Shield-to-recipient: transparent → external dins1 ─────────────────

AttachShieldResult BuildAddressedShieldBundleForTx(
    dinero::Transaction& tx,
    const AddressedRecipient& recipient,
    const std::array<uint8_t, 512>* recipient_memo,
    bool cv_bound,
    bool spend_auth) {
    AttachShieldResult out;

    if (recipient.value_una == 0) {
        out.status = OpStatus::InvalidParams;
        out.error = "value must be positive";
        return out;
    }
    if (!dinero::Transaction::IsShieldedVersion(tx.version)) {
        out.status = OpStatus::InvalidParams;
        out.error = "tx.version must be shielded (5 or 6)";
        return out;
    }

    if (!sh::PedersenGeneratorsReady()) {
        (void)sh::PedersenGeneratorV();
        if (!sh::PedersenGeneratorsReady()) {
            out.status = OpStatus::InternalError;
            out.error = "pedersen_generators_not_ready";
            return out;
        }
    }

    // ONE addressed recipient output (shared construction). No shielded
    // spends, no shielded change — transparent change is the RPC's job.
    auto rout = BuildAddressedRecipientOutput(recipient, recipient_memo,
                                              cv_bound, spend_auth);
    if (rout.status != OpStatus::Ok) {
        out.status = rout.status;
        out.error  = rout.error;
        return out;
    }

    // Sighash over the transparent envelope (vins/change/locktime/version/
    // explicit_fee); bundle bytes are not in the sighash.
    const sh::Hash tx_sighash = sh::ComputeShieldedTxSighash(tx);

    // Bundle: no spends, one output, value_balance = +value (transparent
    // coins entering the pool), exactly like BuildShieldBundleForTx.
    sh::ShieldedBundle bundle{};
    const auto build_rc = sh::BuildShieldedBundle({}, {rout.planned},
                                                  tx_sighash, bundle);
    if (build_rc != sh::BundleBuildResult::Ok) {
        out.status = OpStatus::InternalError;
        out.error = "build_shielded_bundle_failed:" +
                    std::to_string(static_cast<int>(build_rc));
        return out;
    }
    if (bundle.value_balance != static_cast<int64_t>(recipient.value_una)) {
        out.status = OpStatus::InternalError;
        out.error = "bundle_value_balance_mismatch";
        return out;
    }

    auto bundle_bytes = sh::SerializeShieldedBundle(bundle);
    if (bundle_bytes.empty()) {
        out.status = OpStatus::InternalError;
        out.error = "bundle_serialization_failed";
        return out;
    }
    tx.shielded_bundle_bytes = std::move(bundle_bytes);

    out.status       = OpStatus::Ok;
    out.commitment   = rout.commitment;  // recipient commitment (their note)
    out.randomness   = rout.randomness;
    out.bundle_bytes = tx.shielded_bundle_bytes.size();
    // No spend authority is returned for an external recipient. The wallet
    // self-shield wrapper supplies its independently derived recipient secret.
    return out;
}

} // namespace dinero::wallet::shielded_ops
