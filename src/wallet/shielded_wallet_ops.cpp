/**
 * Shielded wallet operations — shield + unshield handlers.
 * See include/wallet/shielded_wallet_ops.h.
 */

#include "wallet/shielded_wallet_ops.h"
#include "wallet/shielded_derivation.h"

#include "consensus/shielded/binding_sig.h"
#include "consensus/shielded/bundle_builder.h"
#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/pedersen_generators.h"
#include "consensus/shielded/shielded_circuit.h"
#include "consensus/shielded/shielded_serialization.h"

#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <cmath>
#include <cstring>

namespace dinero::wallet::shielded_ops {

namespace sh = consensus::shielded;

namespace {

sh::Hash RandomHash() {
    sh::Hash h{};
    RAND_bytes(h.data(), sh::HASH_BYTES);
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
                                          uint64_t value_una) {
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

    // 2. Spartan output proof for the new note.
    sh::OutputWitness ow{};
    ow.value      = value_hash;
    ow.public_key = public_key;
    ow.randomness = randomness;
    ow.d          = diversifier;

    sh::OutputPublicInputs opi{};
    opi.commitment = commitment;

    auto output_proof = sh::ProveOutput(ow, opi, nullptr);
    if (output_proof.empty()) {
        out.status = OpStatus::ProofError;
        out.error = "output proof generation failed";
        OPENSSL_cleanse(secret_key.data(), secret_key.size());
        return out;
    }

    // 3. Wire one PlannedOutput.
    sh::PlannedOutput planned{};
    planned.commitment     = commitment;
    planned.value_una      = value_una;
    planned.rcv            = RandomHash();
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
                                              uint64_t fee_una) {
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

    // 2. Generate the Spartan spend proof.
    sh::SpendWitness sw{};
    sw.secret_key  = note.secret_key;
    sw.leaf_index  = note.leaf_index;
    sw.value       = value_hash;
    sw.randomness  = note.randomness;
    sw.d           = note.d;
    sw.merkle_path = note.merkle_path;

    sh::SpendPublicInputs spi{};
    spi.nullifier = sh::ComputeNullifier(note.secret_key, note.leaf_index);
    spi.anchor    = note.anchor;

    auto spend_proof = sh::ProveSpend(sw, spi, nullptr);
    if (spend_proof.empty()) {
        out.status = OpStatus::ProofError;
        out.error = "spend proof generation failed";
        return out;
    }

    // 3. Wire the PlannedSpend.
    sh::PlannedSpend planned{};
    planned.nullifier   = spi.nullifier;
    planned.anchor      = spi.anchor;
    planned.value_una   = note.value_una;
    planned.rcv         = RandomHash();
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
                                              uint64_t fee_una) {
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

    sh::SpendWitness sw{};
    sw.secret_key  = note.secret_key;
    sw.leaf_index  = note.leaf_index;
    sw.value       = spend_value_hash;
    sw.randomness  = note.randomness;
    sw.d           = spend_d;
    sw.merkle_path = note.merkle_path;

    sh::SpendPublicInputs spi{};
    spi.nullifier = sh::ComputeNullifier(note.secret_key, note.leaf_index);
    spi.anchor    = note.anchor;

    auto spend_proof = sh::ProveSpend(sw, spi, nullptr);
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
    planned_spend.rcv         = RandomHash();
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

    sh::OutputWitness ow{};
    ow.value      = out_value_hash;
    ow.public_key = out_public_key;
    ow.randomness = out_randomness;
    ow.d          = out_d;

    sh::OutputPublicInputs opi{};
    opi.commitment = out_commitment;

    auto output_proof = sh::ProveOutput(ow, opi, nullptr);
    if (output_proof.empty()) {
        out.status = OpStatus::ProofError;
        out.error = "output proof generation failed";
        OPENSSL_cleanse(out_secret_key.data(), out_secret_key.size());
        return out;
    }

    sh::PlannedOutput planned_output{};
    planned_output.commitment     = out_commitment;
    planned_output.value_una      = out_value_una;
    planned_output.rcv            = RandomHash();
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
    uint64_t fee_una) {
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
        sh::SpendWitness sw{};
        sw.secret_key  = s.secret_key;
        sw.leaf_index  = s.leaf_index;
        sw.value       = value_hash;
        sw.randomness  = s.randomness;
        sw.d           = s.d;
        sw.merkle_path = s.merkle_path;

        sh::SpendPublicInputs spi{};
        spi.nullifier = sh::ComputeNullifier(s.secret_key, s.leaf_index);
        spi.anchor    = s.anchor;

        auto proof = sh::ProveSpend(sw, spi, nullptr);
        if (proof.empty()) {
            out.status = OpStatus::ProofError;
            out.error = "spend proof generation failed";
            return out;
        }
        sh::PlannedSpend ps{};
        ps.nullifier   = spi.nullifier;
        ps.anchor      = spi.anchor;
        ps.value_una   = s.value_una;
        ps.rcv         = RandomHash();
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

        sh::OutputWitness ow{};
        ow.value      = value_hash;
        ow.public_key = mat.public_key;
        ow.randomness = mat.randomness;
        ow.d          = output_d;

        sh::OutputPublicInputs opi{};
        opi.commitment = mat.commitment;

        auto proof = sh::ProveOutput(ow, opi, nullptr);
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
        po.rcv            = RandomHash();
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

AttachAddressedTransferResult BuildAddressedTransferBundleForTx(
    dinero::Transaction& tx,
    const std::vector<UnshieldNoteInput>& spends,
    const AddressedRecipient& recipient,
    uint64_t change_value_una,
    uint64_t fee_una,
    const std::array<uint8_t, 512>* recipient_memo) {
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
        sh::SpendWitness sw{};
        sw.secret_key  = s.secret_key;
        sw.leaf_index  = s.leaf_index;
        sw.value       = value_hash;
        sw.randomness  = s.randomness;
        sw.d           = s.d;
        sw.merkle_path = s.merkle_path;
        sh::SpendPublicInputs spi{};
        spi.nullifier = sh::ComputeNullifier(s.secret_key, s.leaf_index);
        spi.anchor    = s.anchor;
        auto proof = sh::ProveSpend(sw, spi, nullptr);
        if (proof.empty()) {
            out.status = OpStatus::ProofError;
            out.error = "spend proof generation failed";
            return out;
        }
        sh::PlannedSpend ps{};
        ps.nullifier   = spi.nullifier;
        ps.anchor      = spi.anchor;
        ps.value_una   = s.value_una;
        ps.rcv         = RandomHash();
        ps.spend_proof = std::move(proof);
        ps.nonce       = RandomHash();
        out.spend_nullifiers.push_back(spi.nullifier);
        planned_spends.push_back(std::move(ps));
    }

    // ── Recipient output (addressed):
    //   d = recipient.d (packed into 32-byte hash for the on-chain
    //                    commitment formula)
    //   pk_note = Poseidon(DeriveNoteSpendKey(rcm), 0)
    //   commitment = NoteCommitment(d_packed, pk_note, value, rcm)
    //   encrypted_note = EncryptNoteForRecipient(d, pk_d, plaintext)
    sh::Hash recipient_rcm = RandomHash();
    sh::Hash recipient_sk  = shdrv::DeriveNoteSpendKey(recipient_rcm);
    sh::Hash recipient_pk  = sh::PoseidonHash2(recipient_sk, zero);
    sh::Hash recipient_value_hash = ValueToHash(recipient.value_una);
    sh::Hash recipient_d_packed{};
    std::memcpy(recipient_d_packed.data(), recipient.d.data(), recipient.d.size());
    sh::Hash recipient_commitment = sh::NoteCommitment(
        recipient_d_packed, recipient_pk,
        recipient_value_hash, recipient_rcm);

    sh::OutputWitness recipient_ow{};
    recipient_ow.value      = recipient_value_hash;
    recipient_ow.public_key = recipient_pk;
    recipient_ow.randomness = recipient_rcm;
    recipient_ow.d          = recipient_d_packed;
    sh::OutputPublicInputs recipient_opi{};
    recipient_opi.commitment = recipient_commitment;
    auto recipient_proof = sh::ProveOutput(recipient_ow, recipient_opi, nullptr);
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
        enc_bytes = shdrv::EncryptNoteForRecipient(dvf, recipient.pk_d, nplain);
    } catch (const std::exception& e) {
        out.status = OpStatus::InternalError;
        out.error = std::string("encrypt_note_failed: ") + e.what();
        return out;
    }

    sh::PlannedOutput recipient_planned{};
    recipient_planned.commitment     = recipient_commitment;
    recipient_planned.value_una      = recipient.value_una;
    recipient_planned.rcv            = RandomHash();
    recipient_planned.encrypted_note =
        std::vector<uint8_t>(enc_bytes.begin(), enc_bytes.end());
    recipient_planned.output_proof   = std::move(recipient_proof);
    recipient_planned.nonce          = RandomHash();

    std::vector<sh::PlannedOutput> planned_outputs;
    planned_outputs.reserve(2);
    planned_outputs.push_back(std::move(recipient_planned));

    // ── Change (legacy self-recipient style; receivable via existing
    //    pending-note bookkeeping — wallet wrapper passes the secret
    //    material to AddPendingNote). Migrates to addressed in 3e.
    sh::Hash change_secret_key{};
    sh::Hash change_public_key{};
    sh::Hash change_randomness{};
    sh::Hash change_commitment{};
    if (change_value_una > 0) {
        change_secret_key = RandomHash();
        change_public_key = sh::PoseidonHash2(change_secret_key, zero);
        change_randomness = RandomHash();
        sh::Hash change_value_hash = ValueToHash(change_value_una);
        change_commitment = sh::NoteCommitment(
            zero, change_public_key, change_value_hash, change_randomness);

        sh::OutputWitness ow{};
        ow.value      = change_value_hash;
        ow.public_key = change_public_key;
        ow.randomness = change_randomness;
        ow.d          = zero;
        sh::OutputPublicInputs opi{};
        opi.commitment = change_commitment;
        auto proof = sh::ProveOutput(ow, opi, nullptr);
        if (proof.empty()) {
            OPENSSL_cleanse(change_secret_key.data(), change_secret_key.size());
            out.status = OpStatus::ProofError;
            out.error = "change output proof generation failed";
            return out;
        }
        sh::PlannedOutput cpo{};
        cpo.commitment     = change_commitment;
        cpo.value_una      = change_value_una;
        cpo.rcv            = RandomHash();
        cpo.encrypted_note = std::vector<uint8_t>(96, 0);  // legacy placeholder
        cpo.output_proof   = std::move(proof);
        cpo.nonce          = RandomHash();
        planned_outputs.push_back(std::move(cpo));
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
    OPENSSL_cleanse(change_secret_key.data(), change_secret_key.size());
    return out;
}

} // namespace dinero::wallet::shielded_ops
