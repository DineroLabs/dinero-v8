/**
 * Wallet-scoped runtime helpers for the v5 shielded pool.
 *
 * Keeps the shielded note DB and wallet-side commitment tree separate from
 * the low-level proving library so dinero_shielded stays testable without
 * WalletManager.
 */

#include "wallet/shielded_wallet_ops.h"
#include "wallet/shielded_derivation.h"

#include "consensus/chainparams.h"
#include "consensus/shielded/binding_sig.h"
#include "consensus/shielded/bundle_builder.h"
#include "consensus/shielded/pedersen_generators.h"
#include "consensus/shielded/shielded_serialization.h"
#include "wallet/wallet_manager.h"

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace dinero::wallet::shielded_ops {

namespace sh = consensus::shielded;

namespace {

// Audit Critical #1: decide whether wallet-built shielded proofs must be
// cv-bound (0x03/0x04). A tx attached now is expected to be mined in the NEXT
// block (tip + 1); it must be cv-bound iff that height is at/above the
// activation height. While activation defaults to UINT32_MAX this is always
// false (tip+1 can never reach UINT32_MAX), so the runtime behavior is
// unchanged until a real activation height is configured.
//
// BOUNDARY CAVEAT: this predicts the mining height as tip+1. A tx built just
// below the boundary but actually mined at/above the activation height (delay,
// reorg, or simply not landing in the very next block) carries a legacy proof
// and will be rejected at consensus (version-byte mismatch) — and vice-versa
// for a cv-bound tx that lands in a pre-activation block. The mempool/submit
// path re-validates at the real next-block height (ValidateShieldedBundle), so
// such a tx is rejected (not silently mis-accepted) and must be rebuilt.
bool CvBoundForMiningAtTip(uint32_t tip_height) {
    const uint64_t target_height = static_cast<uint64_t>(tip_height) + 1;
    return target_height >=
           static_cast<uint64_t>(dinero::Params().shielded_cv_binding_activation_height);
}

// Expected shielded-address HRP for the active chain (spec §7.1 network
// match). A recipient address whose HRP differs must be rejected before
// any tx work.
const char* ExpectedShieldedHrp() {
    namespace shdrv = ::dinero::wallet::shielded;
    switch (dinero::GetActiveChain()) {
        case dinero::Chain::MAINNET: return shdrv::kHrpMainnet;
        case dinero::Chain::TESTNET: return shdrv::kHrpTestnet;
        case dinero::Chain::REGTEST: return shdrv::kHrpRegtest;
    }
    return shdrv::kHrpMainnet;
}

struct ShieldedRuntime {
    std::string       store_path;
    ShieldedNoteStore store;
    sh::CommitmentTree tree;
};

std::mutex                       g_runtime_mutex;
std::unique_ptr<ShieldedRuntime> g_runtime;

// Fails CLOSED — see the twin in shielded_wallet_ops.cpp. An ignored
// RAND_bytes failure leaves the buffer all-zero, and this feeds cv blinding
// factors and range-proof nonces; zero rcv makes cv = value*V, brute-forceable,
// on a bundle that still verifies.
sh::Hash RandomHash() {
    sh::Hash h{};
    if (RAND_bytes(h.data(), sh::HASH_BYTES) != 1) {
        throw std::runtime_error("shielded: RAND_bytes failed (refusing to "
                                 "build a bundle with zero blinding)");
    }
    return h;
}

// See shielded_wallet_ops.cpp for the Phase 2 wave 4 byte-order fix.
sh::Hash ValueToHash(uint64_t v) {
    sh::Hash h{};
    for (int i = 0; i < 8; ++i) {
        h[31 - i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
    }
    return h;
}

bool HasOwnedNoteCommitment(const ShieldedNoteStore& store, const sh::Hash& commitment) {
    const auto notes = store.ListAll();
    return std::any_of(notes.begin(), notes.end(), [&](const ShieldedNote& note) {
        return note.commitment == commitment;
    });
}

std::optional<uint64_t> FindLeafIndex(const std::vector<sh::Hash>& leaves,
                                      const sh::Hash& commitment) {
    for (uint64_t i = 0; i < leaves.size(); ++i) {
        if (leaves[i] == commitment) return i;
    }
    return std::nullopt;
}

std::string DeriveShieldedStorePath(const dinero::WalletManager& wallet) {
    const std::string p2mr_path = wallet.GetV7P2MRStorePath();
    if (p2mr_path.empty()) {
        return {};
    }

    std::filesystem::path path(p2mr_path);
    std::string filename = path.filename().string();
    constexpr const char* kP2MRPrefix = "v7_p2mr_";
    if (filename.rfind(kP2MRPrefix, 0) == 0) {
        filename.replace(0, std::char_traits<char>::length(kP2MRPrefix), "shielded_notes_");
        path.replace_filename(filename);
        return path.string();
    }

    path.replace_filename("shielded_notes.db");
    return path.string();
}

const char* DecodeErrorToString(sh::BundleDecodeError error) {
    switch (error) {
        case sh::BundleDecodeError::Ok: return "ok";
        case sh::BundleDecodeError::Truncated: return "truncated";
        case sh::BundleDecodeError::VarintOverflow: return "varint_overflow";
        case sh::BundleDecodeError::TrailingBytes: return "trailing_bytes";
        case sh::BundleDecodeError::NotCanonical: return "not_canonical";
        case sh::BundleDecodeError::OrderViolation: return "order_violation";
    }
    return "unknown";
}

bool EnsureRuntimeLocked(dinero::WalletManager& wallet, std::string* error) {
    const std::string store_path = DeriveShieldedStorePath(wallet);
    if (store_path.empty()) {
        if (error) *error = "no_active_wallet";
        return false;
    }

    if (g_runtime && g_runtime->store_path == store_path) {
        return true;
    }

    auto fresh = std::make_unique<ShieldedRuntime>();
    auto open = fresh->store.Open(store_path);
    if (open != ShieldedNoteStore::OpenResult::Ok) {
        if (error) *error = "shielded_store_open_failed";
        return false;
    }

    const auto leaves = fresh->store.LoadChainLeaves();
    for (const auto& leaf : leaves) {
        fresh->tree.Append(leaf);
    }

    // Phase 3 wave 3c: roll back any pending-spent rows whose unshield tx
    // never mined (daemon crash, mempool eviction, or restart without
    // mempool persistence). The mempool's nullifier-conflict check is the
    // safety net: if the original tx is still pending, a re-spend attempt
    // is rejected with `Shielded nullifier conflict with mempool transaction`.
    fresh->store.UnmarkAllPendingSpent();

    fresh->store_path = store_path;
    g_runtime = std::move(fresh);
    return true;
}

} // namespace

bool EnsureWalletRuntime(dinero::WalletManager& wallet, std::string* error) {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    return EnsureRuntimeLocked(wallet, error);
}

ShieldResult PrepareShield(ShieldParams params, dinero::WalletManager& wallet) {
    ShieldResult out;

    if (params.value_una == 0) {
        out.status = OpStatus::InvalidParams;
        out.error = "value must be positive";
        return out;
    }

    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    std::string init_error;
    if (!EnsureRuntimeLocked(wallet, &init_error)) {
        out.status = OpStatus::StoreError;
        out.error = init_error.empty() ? "shielded_store_init_failed" : init_error;
        return out;
    }

    sh::Hash secret_key = RandomHash();
    sh::Hash zero{};
    sh::Hash public_key = sh::PoseidonHash2(secret_key, zero);
    sh::Hash randomness = RandomHash();
    sh::Hash value_hash = ValueToHash(params.value_una);
    // Phase 2 wave 5: diversifier defaults to zeros until the wallet
    // surfaces shielded addresses (Phase 5).
    sh::Hash diversifier{};
    sh::Hash commitment = sh::NoteCommitment(diversifier, public_key, value_hash, randomness);

    sh::OutputWitness ow;
    ow.value = value_hash;
    ow.public_key = public_key;
    ow.randomness = randomness;
    ow.d = diversifier;

    sh::OutputPublicInputs opi;
    opi.commitment = commitment;

    auto proof = sh::ProveOutput(ow, opi, nullptr);
    if (proof.empty()) {
        out.status = OpStatus::ProofError;
        out.error = "output proof generation failed";
        OPENSSL_cleanse(secret_key.data(), secret_key.size());
        return out;
    }

    if (!g_runtime->store.AddPendingNote(params.value_una,
                                         secret_key,
                                         public_key,
                                         randomness,
                                         commitment,
                                         params.current_height)) {
        out.status = OpStatus::StoreError;
        out.error = "failed to persist pending shielded note";
        OPENSSL_cleanse(secret_key.data(), secret_key.size());
        return out;
    }

    out.status = OpStatus::Ok;
    out.commitment = commitment;
    out.nullifier_key = secret_key;
    out.output_proof = std::move(proof);

    OPENSSL_cleanse(secret_key.data(), secret_key.size());
    return out;
}

UnshieldResult UnshieldConfirmed(UnshieldParams params, dinero::WalletManager& wallet) {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    std::string init_error;
    if (!EnsureRuntimeLocked(wallet, &init_error)) {
        UnshieldResult out;
        out.status = OpStatus::StoreError;
        out.error = init_error.empty() ? "shielded_store_init_failed" : init_error;
        return out;
    }
    return Unshield(params, g_runtime->store, g_runtime->tree);
}

bool ProcessConfirmedBlock(dinero::WalletManager& wallet,
                           uint32_t height,
                           const std::vector<dinero::Transaction>& transactions,
                           std::string* error) {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    if (!EnsureRuntimeLocked(wallet, error)) {
        return false;
    }

    // Phase 5 Wave 3e + multi-account: trial-decrypt every shielded
    // output against the wallet's receive/view cache. This deliberately
    // does not require spend unlock; wallet.lock and spend-time timeout
    // only drop spend secrets.
    namespace shdrv = ::dinero::wallet::shielded;
    std::vector<sh::Hash> wallet_ivks = wallet.GetShieldedIncomingViewingKeys();

    for (const auto& tx : transactions) {
        if (!tx.IsShielded()) {
            continue;
        }

        sh::ShieldedBundle bundle;
        const auto decode = sh::DeserializeShieldedBundle(tx.shielded_bundle_bytes, &bundle);
        if (decode != sh::BundleDecodeError::Ok) {
            if (error) {
                *error = std::string("shielded_bundle_decode_failed: ") + DecodeErrorToString(decode);
            }
            return false;
        }

        for (const auto& spend : bundle.spends) {
            g_runtime->store.MarkSpentByNullifier(spend.nullifier, height);
        }

        for (const auto& output : bundle.outputs) {
            // Wave 3e + multi-account: addressed-output detection.
            // Trial-decrypt the encrypted_note against every account's
            // ivk; on first AEAD success, sanity-check the recomputed
            // commitment and register the note as pending so the
            // ConfirmNote step below promotes it. Stops at the first
            // matching ivk (a note can only belong to one account).
            if (!wallet_ivks.empty() &&
                output.encrypted_note.size() == shdrv::kEncryptedNoteBytes) {
                shdrv::EncryptedNote enc{};
                std::memcpy(enc.data(), output.encrypted_note.data(), enc.size());
                for (const auto& ivk : wallet_ivks) {
                    auto plaintext = shdrv::TryDecryptNoteForViewer(ivk, enc);
                    if (!plaintext) continue;
                    sh::Hash sk_note = shdrv::DeriveNoteSpendKey(plaintext->rcm);
                    sh::Hash pk_note = sh::PoseidonHash2(sk_note, sh::Hash{});
                    sh::Hash d_packed{};
                    std::memcpy(d_packed.data(), plaintext->d.data(),
                                plaintext->d.size());
                    sh::Hash value_h = ValueToHash(plaintext->value_una);
                    sh::Hash expected = sh::NoteCommitment(d_packed, pk_note,
                                                           value_h, plaintext->rcm);
                    if (expected == output.commitment) {
                        (void)g_runtime->store.AddPendingNote(
                            plaintext->value_una, sk_note, pk_note,
                            plaintext->rcm, output.commitment, height,
                            NoteKeyScheme::LegacySenderKey);
                    } else {
                        // Spend-authority note: committed to pk_d = s·G rather
                        // than to the sender-derived Poseidon(rcm-key, 0), so
                        // the legacy recomputation above cannot match. `s` is
                        // NOT in the plaintext — the recipient derives it from
                        // their OWN ivk plus the note's diversifier, which is
                        // exactly why the sender cannot spend it.
                        //
                        // Distinguished by which commitment formula reproduces
                        // the on-chain value, not by height: the note carries
                        // its own convention, and a note is only detectable at
                        // all if one of the two formulas matches.
                        auto dk = shdrv::DeriveDiversifiedSpendKey(ivk,
                                                                   plaintext->d);
                        sh::Hash expected_auth = sh::NoteCommitment(
                            d_packed, dk.pk_d, value_h, plaintext->rcm);
                        if (expected_auth == output.commitment) {
                            (void)g_runtime->store.AddPendingNote(
                                plaintext->value_una, dk.s, dk.pk_d,
                                plaintext->rcm, output.commitment, height,
                                NoteKeyScheme::Auth);
                        }
                        OPENSSL_cleanse(dk.s.data(), dk.s.size());
                    }
                    OPENSSL_cleanse(sk_note.data(), sk_note.size());
                    break;
                }
            }

            const uint64_t leaf_index = g_runtime->store.GetChainLeafCount();
            if (!g_runtime->store.AppendChainLeaf(output.commitment, leaf_index, height)) {
                if (error) *error = "shielded_leaf_append_failed";
                return false;
            }

            const uint64_t new_count = g_runtime->store.GetChainLeafCount();
            if (new_count == leaf_index + 1) {
                g_runtime->tree.Append(output.commitment);
                g_runtime->store.ConfirmNote(output.commitment, leaf_index, height);
            } else if (new_count != leaf_index) {
                if (error) *error = "shielded_leaf_count_drift";
                return false;
            }
        }
    }

    return true;
}

bool RescanConfirmedBlock(dinero::WalletManager& wallet,
                          uint32_t height,
                          const std::vector<dinero::Transaction>& transactions,
                          std::string* error) {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    if (!EnsureRuntimeLocked(wallet, error)) {
        return false;
    }

    bool has_shielded_activity = false;
    bool has_shielded_outputs = false;
    for (const auto& tx : transactions) {
        if (!tx.IsShielded()) {
            continue;
        }
        sh::ShieldedBundle bundle;
        const auto decode = sh::DeserializeShieldedBundle(tx.shielded_bundle_bytes, &bundle);
        if (decode != sh::BundleDecodeError::Ok) {
            if (error) {
                *error = std::string("shielded_bundle_decode_failed: ") + DecodeErrorToString(decode);
            }
            return false;
        }
        has_shielded_activity = true;
        if (!bundle.outputs.empty()) {
            has_shielded_outputs = true;
            break;
        }
    }
    if (!has_shielded_activity) {
        return true;
    }

    namespace shdrv = ::dinero::wallet::shielded;
    std::vector<sh::Hash> wallet_ivks;
    if (has_shielded_outputs) {
        wallet_ivks = wallet.GetShieldedIncomingViewingKeys();
    }

    if (has_shielded_outputs && wallet_ivks.empty()) {
        if (error) *error = "shielded_rescan_no_viewing_keys_unlock_once";
        return false;
    }

    auto leaves = g_runtime->store.LoadChainLeaves();

    for (const auto& tx : transactions) {
        if (!tx.IsShielded()) {
            continue;
        }

        sh::ShieldedBundle bundle;
        const auto decode = sh::DeserializeShieldedBundle(tx.shielded_bundle_bytes, &bundle);
        if (decode != sh::BundleDecodeError::Ok) {
            if (error) {
                *error = std::string("shielded_bundle_decode_failed: ") + DecodeErrorToString(decode);
            }
            return false;
        }

        for (const auto& spend : bundle.spends) {
            g_runtime->store.MarkSpentByNullifier(spend.nullifier, height);
        }

        for (const auto& output : bundle.outputs) {
            auto leaf_index_opt = FindLeafIndex(leaves, output.commitment);
            uint64_t leaf_index = 0;
            if (leaf_index_opt) {
                leaf_index = *leaf_index_opt;
            } else {
                // Historical restore path: if wallet shielded leaves are not
                // populated yet, append in chain order. Rescans over an
                // already-synced wallet take the branch above and remain
                // idempotent.
                leaf_index = g_runtime->store.GetChainLeafCount();
                if (!g_runtime->store.AppendChainLeaf(output.commitment, leaf_index, height)) {
                    if (error) *error = "shielded_leaf_append_failed";
                    return false;
                }
                g_runtime->tree.Append(output.commitment);
                leaves.push_back(output.commitment);
            }

            if (output.encrypted_note.size() != shdrv::kEncryptedNoteBytes) {
                continue;
            }

            shdrv::EncryptedNote enc{};
            std::memcpy(enc.data(), output.encrypted_note.data(), enc.size());
            for (const auto& ivk : wallet_ivks) {
                auto plaintext = shdrv::TryDecryptNoteForViewer(ivk, enc);
                if (!plaintext) continue;

                sh::Hash sk_note = shdrv::DeriveNoteSpendKey(plaintext->rcm);
                sh::Hash pk_note = sh::PoseidonHash2(sk_note, sh::Hash{});
                sh::Hash d_packed{};
                std::memcpy(d_packed.data(), plaintext->d.data(), plaintext->d.size());
                sh::Hash value_h = ValueToHash(plaintext->value_una);
                sh::Hash expected = sh::NoteCommitment(d_packed, pk_note,
                                                       value_h, plaintext->rcm);
                if (expected == output.commitment &&
                    !HasOwnedNoteCommitment(g_runtime->store, output.commitment)) {
                    if (!g_runtime->store.AddNote(plaintext->value_una,
                                                  sk_note, pk_note,
                                                  plaintext->rcm,
                                                  output.commitment,
                                                  leaf_index,
                                                  height)) {
                        OPENSSL_cleanse(sk_note.data(), sk_note.size());
                        if (error) *error = "shielded_rescan_note_insert_failed";
                        return false;
                    }
                }
                OPENSSL_cleanse(sk_note.data(), sk_note.size());
                break;
            }
        }
    }

    return true;
}

bool ProcessDisconnectedBlock(dinero::WalletManager& wallet,
                              uint32_t height,
                              const dinero::Block& block,
                              std::string* error) {
    (void)height;
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    if (!EnsureRuntimeLocked(wallet, error)) {
        return false;
    }

    uint64_t removed_outputs = 0;
    for (const auto& tx : block.vtx) {
        if (!tx.IsShielded()) {
            continue;
        }

        sh::ShieldedBundle bundle;
        const auto decode = sh::DeserializeShieldedBundle(tx.shielded_bundle_bytes, &bundle);
        if (decode != sh::BundleDecodeError::Ok) {
            if (error) {
                *error = std::string("shielded_bundle_decode_failed: ") + DecodeErrorToString(decode);
            }
            return false;
        }

        for (const auto& spend : bundle.spends) {
            g_runtime->store.UnmarkSpentByNullifier(spend.nullifier);
        }
        for (const auto& output : bundle.outputs) {
            g_runtime->store.UnconfirmNote(output.commitment);
            ++removed_outputs;
        }
    }

    const uint64_t current_size = g_runtime->store.GetChainLeafCount();
    if (removed_outputs > current_size) {
        if (error) *error = "shielded_tree_underflow";
        return false;
    }

    const uint64_t new_size = current_size - removed_outputs;
    if (!g_runtime->store.TruncateChainLeaves(new_size)) {
        if (error) *error = "shielded_leaf_truncate_failed";
        return false;
    }
    if (!g_runtime->tree.Truncate(new_size)) {
        if (error) *error = "shielded_tree_truncate_failed";
        return false;
    }

    return true;
}

std::vector<ShieldedNote> ListShieldedNotes(dinero::WalletManager& wallet, bool include_pending) {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    if (!EnsureRuntimeLocked(wallet, nullptr)) {
        return {};
    }
    return include_pending ? g_runtime->store.ListAll() : g_runtime->store.ListUnspent();
}

uint64_t GetShieldedBalance(dinero::WalletManager& wallet) {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    if (!EnsureRuntimeLocked(wallet, nullptr)) {
        return 0;
    }
    return g_runtime->store.GetBalance();
}

uint64_t GetShieldedTreeSize(dinero::WalletManager& wallet) {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    if (!EnsureRuntimeLocked(wallet, nullptr)) {
        return 0;
    }
    return g_runtime->tree.Size();
}

bool RollbackPendingTransaction(
    dinero::WalletManager& wallet,
    const std::vector<sh::Hash>& spend_nullifiers,
    const std::vector<sh::Hash>& pending_commitments,
    std::string* error) {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    if (!EnsureRuntimeLocked(wallet, error)) {
        return false;
    }
    if (!g_runtime->store.RollbackPendingTransaction(spend_nullifiers,
                                                     pending_commitments)) {
        if (error) *error = "shielded_pending_rollback_failed";
        return false;
    }
    return true;
}

std::optional<ShieldedNote> SelectUnshieldNote(dinero::WalletManager& wallet,
                                               uint64_t min_value_una) {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    if (!EnsureRuntimeLocked(wallet, nullptr)) {
        return std::nullopt;
    }
    auto unspent = g_runtime->store.ListUnspent();
    // Smallest note satisfying min_value_una. Pre-Wave-3d we don't combine
    // notes, so picking the smallest reduces "wasted" value (the user pays
    // fee out of the note, not change).
    std::optional<ShieldedNote> best;
    for (const auto& n : unspent) {
        if (!n.confirmed || n.spent) continue;
        if (n.value_una < min_value_una) continue;
        if (!best || n.value_una < best->value_una) {
            best = n;
        }
    }
    return best;
}

AttachUnshieldResult AttachUnshieldInputBundle(dinero::Transaction& tx,
                                               uint64_t note_leaf_index,
                                               uint64_t fee_una,
                                               dinero::WalletManager& wallet,
                                               bool persist) {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    std::string init_error;
    if (!EnsureRuntimeLocked(wallet, &init_error)) {
        AttachUnshieldResult err{};
        err.status = OpStatus::StoreError;
        err.error  = init_error.empty() ? "shielded_store_init_failed" : init_error;
        return err;
    }

    auto note_opt = g_runtime->store.GetByLeafIndex(note_leaf_index);
    if (!note_opt) {
        AttachUnshieldResult err{};
        err.status = OpStatus::InvalidParams;
        err.error  = "no note at leaf_index " + std::to_string(note_leaf_index);
        return err;
    }
    const auto& note = *note_opt;
    if (!note.confirmed) {
        AttachUnshieldResult err{};
        err.status = OpStatus::InvalidParams;
        err.error  = "note not confirmed yet";
        return err;
    }
    if (note.spent) {
        AttachUnshieldResult err{};
        err.status = OpStatus::InvalidParams;
        err.error  = "note already spent (or pending-spent)";
        return err;
    }
    if (fee_una >= note.value_una) {
        AttachUnshieldResult err{};
        err.status = OpStatus::InvalidParams;
        err.error  = "fee must be strictly less than note value";
        return err;
    }

    auto auth_path = g_runtime->tree.GetAuthPath(note.leaf_index);
    if (!auth_path) {
        AttachUnshieldResult err{};
        err.status = OpStatus::ProofError;
        err.error  = "failed to build merkle auth path";
        return err;
    }

    UnshieldNoteInput input;
    input.secret_key  = note.secret_key;
    input.randomness  = note.randomness;
    input.anchor      = g_runtime->tree.Root();
    input.leaf_index  = note.leaf_index;
    input.value_una   = note.value_una;
    input.merkle_path = auth_path->siblings;
    // Carry the note's key convention into the spend so the proof variant
    // matches the commitment. Omitting it would default to legacy and make
    // every auth note silently unspendable.
    input.key_scheme  = note.key_scheme;

    const bool cv_bound = CvBoundForMiningAtTip(wallet.getBlockchainHeight());
    auto built = BuildUnshieldBundleForTx(tx, input, fee_una, cv_bound);
    if (built.status != OpStatus::Ok) {
        return built;
    }

    // Mark pending-spent (spent=1, spent_height=0). Mempool eviction hook +
    // wallet startup sweep will roll this back if the tx never mines.
    // Skipped in dry-run mode (persist=false): issue #273 fee measurement
    // discards this tx, so no wallet state may change.
    if (persist &&
        !g_runtime->store.MarkSpentByNullifier(built.nullifier,
                                               /*spent_height=*/0)) {
        AttachUnshieldResult err{};
        err.status = OpStatus::StoreError;
        err.error  = "failed to mark note pending-spent";
        return err;
    }

    return built;
}

AttachShieldResult AttachShieldOutputBundle(dinero::Transaction& tx,
                                            uint64_t value_una,
                                            dinero::WalletManager& wallet,
                                            uint32_t current_height,
                                            bool persist) {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    std::string init_error;
    if (!EnsureRuntimeLocked(wallet, &init_error)) {
        AttachShieldResult err{};
        err.status = OpStatus::StoreError;
        err.error  = init_error.empty() ? "shielded_store_init_failed" : init_error;
        return err;
    }

    // Pure builder does all the cryptographic work + bundle attachment.
    const bool cv_bound = CvBoundForMiningAtTip(wallet.getBlockchainHeight());
    auto built = BuildShieldBundleForTx(tx, value_una, cv_bound);
    if (built.status != OpStatus::Ok) {
        return built;
    }

    // Dry-run mode (issue #273 fee measurement): build/attach only, the
    // tx is discarded — do not persist the pending note.
    if (!persist) {
        OPENSSL_cleanse(built.nullifier_key.data(), built.nullifier_key.size());
        return built;
    }

    // Persist the pending note. ProcessConfirmedBlock promotes it to
    // confirmed (and assigns its real leaf_index) once the block lands.
    if (!g_runtime->store.AddPendingNote(value_una,
                                         built.nullifier_key,
                                         built.public_key,
                                         built.randomness,
                                         built.commitment,
                                         current_height)) {
        AttachShieldResult err{};
        err.status = OpStatus::StoreError;
        err.error  = "failed to persist pending shielded note";
        OPENSSL_cleanse(built.nullifier_key.data(), built.nullifier_key.size());
        return err;
    }

    OPENSSL_cleanse(built.nullifier_key.data(), built.nullifier_key.size());
    return built;
}

// ── Shield-to-recipient: transparent → external dins1 ─────────────────

AttachShieldResult AttachAddressedShieldOutputBundle(
    dinero::Transaction& tx,
    const std::string& recipient_address,
    uint64_t value_una,
    dinero::WalletManager& wallet,
    const std::array<uint8_t, 512>* recipient_memo,
    bool persist) {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    std::string init_error;
    if (!EnsureRuntimeLocked(wallet, &init_error)) {
        AttachShieldResult err{};
        err.status = OpStatus::StoreError;
        err.error  = init_error.empty() ? "shielded_store_init_failed" : init_error;
        return err;
    }

    if (value_una == 0) {
        AttachShieldResult err{};
        err.status = OpStatus::InvalidParams;
        err.error  = "value must be positive";
        return err;
    }

    // Decode + network-match BEFORE any tx work (spec §7.1).
    namespace shdrv = ::dinero::wallet::shielded;
    shdrv::DecodedShieldedAddress decoded;
    try {
        decoded = shdrv::DecodeShieldedAddress(recipient_address);
    } catch (const std::exception& e) {
        AttachShieldResult err{};
        err.status = OpStatus::InvalidParams;
        err.error  = std::string("invalid_shielded_address: ") + e.what();
        return err;
    }
    const std::string expected_hrp = ExpectedShieldedHrp();
    if (decoded.hrp != expected_hrp) {
        AttachShieldResult err{};
        err.status = OpStatus::InvalidParams;
        err.error  = "invalid_shielded_address: wrong network hrp '" +
                     decoded.hrp + "' (expected '" + expected_hrp + "')";
        return err;
    }

    AddressedRecipient recipient;
    recipient.d         = decoded.d;
    recipient.pk_d      = decoded.pk_d;
    recipient.pk_d_spend = decoded.pk_d_spend;
    recipient.value_una = value_una;

    const bool cv_bound = CvBoundForMiningAtTip(wallet.getBlockchainHeight());
    auto built = BuildAddressedShieldBundleForTx(tx, recipient, recipient_memo,
                                                 cv_bound);
    // No self note to persist — the note belongs to the recipient. `persist`
    // exists only for API symmetry with AttachShieldOutputBundle (dry-run
    // fee measurement); there is no wallet state to mutate here.
    (void)persist;
    return built;
}

// ── Phase 3 wave 3d: shielded→shielded self-transfer ─────────────────

std::optional<ShieldedNote> SelectTransferNote(dinero::WalletManager& wallet,
                                               uint64_t min_value_una) {
    return SelectUnshieldNote(wallet, min_value_una);
}

AttachTransferResult AttachTransferInputBundle(dinero::Transaction& tx,
                                               uint64_t note_leaf_index,
                                               uint64_t fee_una,
                                               dinero::WalletManager& wallet,
                                               bool persist) {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    std::string init_error;
    if (!EnsureRuntimeLocked(wallet, &init_error)) {
        AttachTransferResult err{};
        err.status = OpStatus::StoreError;
        err.error  = init_error.empty() ? "shielded_store_init_failed" : init_error;
        return err;
    }

    auto note_opt = g_runtime->store.GetByLeafIndex(note_leaf_index);
    if (!note_opt) {
        AttachTransferResult err{};
        err.status = OpStatus::InvalidParams;
        err.error  = "no note at leaf_index " + std::to_string(note_leaf_index);
        return err;
    }
    const auto& note = *note_opt;
    if (!note.confirmed) {
        AttachTransferResult err{};
        err.status = OpStatus::InvalidParams;
        err.error  = "note not confirmed yet";
        return err;
    }
    if (note.spent) {
        AttachTransferResult err{};
        err.status = OpStatus::InvalidParams;
        err.error  = "note already spent (or pending-spent)";
        return err;
    }
    if (fee_una >= note.value_una) {
        AttachTransferResult err{};
        err.status = OpStatus::InvalidParams;
        err.error  = "fee must be strictly less than note value";
        return err;
    }

    auto auth_path = g_runtime->tree.GetAuthPath(note.leaf_index);
    if (!auth_path) {
        AttachTransferResult err{};
        err.status = OpStatus::ProofError;
        err.error  = "failed to build merkle auth path";
        return err;
    }

    UnshieldNoteInput input;
    input.secret_key  = note.secret_key;
    input.randomness  = note.randomness;
    input.anchor      = g_runtime->tree.Root();
    input.leaf_index  = note.leaf_index;
    input.value_una   = note.value_una;
    input.merkle_path = auth_path->siblings;
    // Carry the note's key convention into the spend so the proof variant
    // matches the commitment. Omitting it would default to legacy and make
    // every auth note silently unspendable.
    input.key_scheme  = note.key_scheme;

    const bool cv_bound = CvBoundForMiningAtTip(wallet.getBlockchainHeight());
    auto built = BuildTransferBundleForTx(tx, input, fee_una, cv_bound);
    if (built.status != OpStatus::Ok) {
        return built;
    }

    // Dry-run mode (issue #273 fee measurement): build/attach only, the
    // tx is discarded — no pending-spent mark, no pending note.
    if (!persist) {
        OPENSSL_cleanse(built.out_secret_key.data(), built.out_secret_key.size());
        return built;
    }

    // Mark the spent note pending-spent so the selector won't pick it again.
    if (!g_runtime->store.MarkSpentByNullifier(built.spend_nullifier,
                                               /*spent_height=*/0)) {
        AttachTransferResult err{};
        err.status = OpStatus::StoreError;
        err.error  = "failed to mark spend note pending-spent";
        OPENSSL_cleanse(built.out_secret_key.data(), built.out_secret_key.size());
        return err;
    }

    // Persist the freshly-created self-output as a pending note. Confirmed +
    // assigned a real leaf_index by ProcessConfirmedBlock once the tx mines.
    const uint32_t created_height = 0;  // pending; promoted at confirm time
    if (!g_runtime->store.AddPendingNote(built.out_value_una,
                                         built.out_secret_key,
                                         built.out_public_key,
                                         built.out_randomness,
                                         built.out_commitment,
                                         created_height)) {
        (void)g_runtime->store.RollbackPendingTransaction(
            {built.spend_nullifier}, {built.out_commitment});
        AttachTransferResult err{};
        err.status = OpStatus::StoreError;
        err.error  = "failed to persist pending transfer-output note";
        OPENSSL_cleanse(built.out_secret_key.data(), built.out_secret_key.size());
        return err;
    }

    OPENSSL_cleanse(built.out_secret_key.data(), built.out_secret_key.size());
    return built;
}

// ── Phase 3 wave 3e: multi-spend + change-output self-transfer ───────

std::optional<std::vector<ShieldedNote>> SelectTransferNotesForValue(
    dinero::WalletManager& wallet,
    uint64_t target_value_una) {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    if (!EnsureRuntimeLocked(wallet, nullptr)) {
        return std::nullopt;
    }
    auto unspent = g_runtime->store.ListUnspent();
    // Sort confirmed unspent ascending by value, greedy-fill until target met.
    std::vector<ShieldedNote> candidates;
    for (const auto& n : unspent) {
        if (!n.confirmed || n.spent) continue;
        candidates.push_back(n);
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const ShieldedNote& a, const ShieldedNote& b) {
                  return a.value_una < b.value_una;
              });
    std::vector<ShieldedNote> picked;
    uint64_t cum = 0;
    for (const auto& n : candidates) {
        picked.push_back(n);
        cum += n.value_una;
        if (cum >= target_value_una) {
            return picked;
        }
    }
    return std::nullopt;  // insufficient
}

AttachMultiTransferResult AttachMultiTransferInputBundle(
    dinero::Transaction& tx,
    const std::vector<uint64_t>& note_leaf_indices,
    const std::vector<uint64_t>& output_values,
    uint64_t fee_una,
    dinero::WalletManager& wallet,
    bool persist) {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    std::string init_error;
    if (!EnsureRuntimeLocked(wallet, &init_error)) {
        AttachMultiTransferResult err{};
        err.status = OpStatus::StoreError;
        err.error  = init_error.empty() ? "shielded_store_init_failed" : init_error;
        return err;
    }

    if (note_leaf_indices.empty()) {
        AttachMultiTransferResult err{};
        err.status = OpStatus::InvalidParams;
        err.error  = "at least one note leaf_index required";
        return err;
    }

    // Resolve every leaf index against the store + tree, build the
    // UnshieldNoteInput vector. Anchor is the wallet-tree root at
    // selection time (single root for the whole bundle).
    const auto root = g_runtime->tree.Root();
    std::vector<UnshieldNoteInput> spends;
    spends.reserve(note_leaf_indices.size());
    for (uint64_t leaf_index : note_leaf_indices) {
        auto note_opt = g_runtime->store.GetByLeafIndex(leaf_index);
        if (!note_opt) {
            AttachMultiTransferResult err{};
            err.status = OpStatus::InvalidParams;
            err.error  = "no note at leaf_index " + std::to_string(leaf_index);
            return err;
        }
        const auto& note = *note_opt;
        if (!note.confirmed) {
            AttachMultiTransferResult err{};
            err.status = OpStatus::InvalidParams;
            err.error  = "note " + std::to_string(leaf_index) + " not confirmed";
            return err;
        }
        if (note.spent) {
            AttachMultiTransferResult err{};
            err.status = OpStatus::InvalidParams;
            err.error  = "note " + std::to_string(leaf_index) + " already spent (or pending-spent)";
            return err;
        }
        auto auth_path = g_runtime->tree.GetAuthPath(note.leaf_index);
        if (!auth_path) {
            AttachMultiTransferResult err{};
            err.status = OpStatus::ProofError;
            err.error  = "failed to build merkle auth path for leaf " +
                         std::to_string(leaf_index);
            return err;
        }
        UnshieldNoteInput input;
        input.secret_key  = note.secret_key;
        input.randomness  = note.randomness;
        input.anchor      = root;
        input.leaf_index  = note.leaf_index;
        input.value_una   = note.value_una;
        input.merkle_path = auth_path->siblings;
        input.key_scheme  = note.key_scheme;
        spends.push_back(std::move(input));
    }

    const bool cv_bound = CvBoundForMiningAtTip(wallet.getBlockchainHeight());
    auto built = BuildMultiTransferBundleForTx(tx, spends, output_values, fee_una, cv_bound);
    if (built.status != OpStatus::Ok) {
        return built;
    }

    // Dry-run mode (issue #273 fee measurement): build/attach only, the
    // tx is discarded — no pending-spent marks, no pending notes.
    if (!persist) {
        for (auto& m : built.outputs) {
            OPENSSL_cleanse(m.secret_key.data(), m.secret_key.size());
        }
        return built;
    }

    // Mark every spent note pending-spent.
    for (const auto& nullifier : built.spend_nullifiers) {
        if (!g_runtime->store.MarkSpentByNullifier(nullifier, /*spent_height=*/0)) {
            std::vector<sh::Hash> commitments;
            commitments.reserve(built.outputs.size());
            for (const auto& output : built.outputs) {
                commitments.push_back(output.commitment);
            }
            (void)g_runtime->store.RollbackPendingTransaction(
                built.spend_nullifiers, commitments);
            AttachMultiTransferResult err{};
            err.status = OpStatus::StoreError;
            err.error  = "failed to mark spend note pending-spent";
            for (auto& m : built.outputs) {
                OPENSSL_cleanse(m.secret_key.data(), m.secret_key.size());
            }
            return err;
        }
    }

    // Persist every fresh self-output as a pending note.
    const uint32_t created_height = 0;
    for (const auto& mat : built.outputs) {
        if (!g_runtime->store.AddPendingNote(mat.value_una,
                                             mat.secret_key,
                                             mat.public_key,
                                             mat.randomness,
                                             mat.commitment,
                                             created_height)) {
            std::vector<sh::Hash> commitments;
            commitments.reserve(built.outputs.size());
            for (const auto& output : built.outputs) {
                commitments.push_back(output.commitment);
            }
            (void)g_runtime->store.RollbackPendingTransaction(
                built.spend_nullifiers, commitments);
            AttachMultiTransferResult err{};
            err.status = OpStatus::StoreError;
            err.error  = "failed to persist pending transfer-output note";
            for (auto& m : built.outputs) {
                OPENSSL_cleanse(m.secret_key.data(), m.secret_key.size());
            }
            return err;
        }
    }

    for (auto& m : built.outputs) {
        OPENSSL_cleanse(m.secret_key.data(), m.secret_key.size());
    }
    return built;
}

// ── Phase 5 Wave 3d: addressed transfer wrapper ──────────────────────

AttachAddressedTransferResult AttachAddressedTransferInputBundle(
    dinero::Transaction& tx,
    const std::vector<uint64_t>& note_leaf_indices,
    const std::string& recipient_address,
    uint64_t recipient_value_una,
    uint64_t fee_una,
    dinero::WalletManager& wallet,
    const std::string* recipient_memo_utf8,
    bool persist) {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    std::string init_error;
    if (!EnsureRuntimeLocked(wallet, &init_error)) {
        AttachAddressedTransferResult err{};
        err.status = OpStatus::StoreError;
        err.error  = init_error.empty() ? "shielded_store_init_failed" : init_error;
        return err;
    }

    if (note_leaf_indices.empty() || recipient_value_una == 0 || fee_una == 0) {
        AttachAddressedTransferResult err{};
        err.status = OpStatus::InvalidParams;
        err.error  = "addressed transfer requires >=1 spend, positive recipient & fee";
        return err;
    }

    // Decode recipient address.
    namespace shdrv = ::dinero::wallet::shielded;
    shdrv::DecodedShieldedAddress recipient_decoded;
    try {
        recipient_decoded = shdrv::DecodeShieldedAddress(recipient_address);
    } catch (const std::exception& e) {
        AttachAddressedTransferResult err{};
        err.status = OpStatus::InvalidParams;
        err.error  = std::string("decode_recipient_address: ") + e.what();
        return err;
    }

    // Resolve every leaf-index → UnshieldNoteInput against current tree.
    const auto root = g_runtime->tree.Root();
    std::vector<UnshieldNoteInput> spends;
    spends.reserve(note_leaf_indices.size());
    uint64_t spend_sum = 0;
    for (uint64_t leaf_index : note_leaf_indices) {
        auto note_opt = g_runtime->store.GetByLeafIndex(leaf_index);
        if (!note_opt) {
            AttachAddressedTransferResult err{};
            err.status = OpStatus::InvalidParams;
            err.error  = "no note at leaf_index " + std::to_string(leaf_index);
            return err;
        }
        const auto& note = *note_opt;
        if (!note.confirmed) {
            AttachAddressedTransferResult err{};
            err.status = OpStatus::InvalidParams;
            err.error  = "note " + std::to_string(leaf_index) + " not confirmed";
            return err;
        }
        if (note.spent) {
            AttachAddressedTransferResult err{};
            err.status = OpStatus::InvalidParams;
            err.error  = "note " + std::to_string(leaf_index) + " already spent";
            return err;
        }
        auto auth_path = g_runtime->tree.GetAuthPath(note.leaf_index);
        if (!auth_path) {
            AttachAddressedTransferResult err{};
            err.status = OpStatus::ProofError;
            err.error  = "merkle auth path failed for leaf " + std::to_string(leaf_index);
            return err;
        }
        UnshieldNoteInput input;
        input.secret_key  = note.secret_key;
        input.randomness  = note.randomness;
        input.anchor      = root;
        input.leaf_index  = note.leaf_index;
        input.value_una   = note.value_una;
        input.merkle_path = auth_path->siblings;
        input.key_scheme  = note.key_scheme;
        spend_sum += note.value_una;
        spends.push_back(std::move(input));
    }
    if (spend_sum < recipient_value_una + fee_una) {
        AttachAddressedTransferResult err{};
        err.status = OpStatus::InvalidParams;
        err.error  = "insufficient: sum(spend) < amount + fee";
        return err;
    }
    const uint64_t change_value = spend_sum - recipient_value_una - fee_una;

    AddressedRecipient recipient;
    recipient.d         = recipient_decoded.d;
    recipient.pk_d      = recipient_decoded.pk_d;
    recipient.pk_d_spend = recipient_decoded.pk_d_spend;
    recipient.value_una = recipient_value_una;

    std::array<uint8_t, 512> memo_buf{};
    bool have_memo = false;
    if (recipient_memo_utf8 != nullptr && !recipient_memo_utf8->empty()) {
        const std::size_t copy_len =
            std::min<std::size_t>(recipient_memo_utf8->size(), memo_buf.size());
        std::memcpy(memo_buf.data(), recipient_memo_utf8->data(), copy_len);
        have_memo = true;
    }
    const bool cv_bound = CvBoundForMiningAtTip(wallet.getBlockchainHeight());
    auto built = BuildAddressedTransferBundleForTx(
        tx, spends, recipient, change_value, fee_una,
        have_memo ? &memo_buf : nullptr, cv_bound);
    if (built.status != OpStatus::Ok) {
        return built;
    }

    // Dry-run mode (issue #273 fee measurement): build/attach only, the
    // tx is discarded — no pending-spent marks, no pending change note.
    if (!persist) {
        OPENSSL_cleanse(built.change_secret_key.data(),
                        built.change_secret_key.size());
        return built;
    }

    // Mark spent notes pending-spent.
    for (const auto& nullifier : built.spend_nullifiers) {
        if (!g_runtime->store.MarkSpentByNullifier(nullifier, /*spent_height=*/0)) {
            const std::vector<sh::Hash> commitments = built.had_change
                ? std::vector<sh::Hash>{built.change_commitment}
                : std::vector<sh::Hash>{};
            (void)g_runtime->store.RollbackPendingTransaction(
                built.spend_nullifiers, commitments);
            AttachAddressedTransferResult err{};
            err.status = OpStatus::StoreError;
            err.error  = "failed to mark spend note pending-spent";
            OPENSSL_cleanse(built.change_secret_key.data(),
                            built.change_secret_key.size());
            return err;
        }
    }

    // Persist legacy-style change note (if any) so wallet keeps seeing it.
    if (built.had_change) {
        const uint32_t created_height = 0;  // pending; promoted at confirm
        if (!g_runtime->store.AddPendingNote(built.change_value_una,
                                             built.change_secret_key,
                                             built.change_public_key,
                                             built.change_randomness,
                                             built.change_commitment,
                                             created_height)) {
            (void)g_runtime->store.RollbackPendingTransaction(
                built.spend_nullifiers, {built.change_commitment});
            AttachAddressedTransferResult err{};
            err.status = OpStatus::StoreError;
            err.error  = "failed to persist pending change note";
            OPENSSL_cleanse(built.change_secret_key.data(),
                            built.change_secret_key.size());
            return err;
        }
    }

    OPENSSL_cleanse(built.change_secret_key.data(), built.change_secret_key.size());
    return built;
}

} // namespace dinero::wallet::shielded_ops
