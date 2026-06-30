#pragma once
/**
 * Shielded wallet operations — shield, unshield, private balance.
 *
 * These are pure handler functions (no JSON, no RPC server dependency).
 * The JSON-RPC adapter layer calls these with parsed params.
 */

#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/shielded_circuit.h"
#include "primitives/block.h"
#include "wallet/transaction.h"
#include "wallet/shielded_note_store.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dinero {
class WalletManager;
}

namespace dinero::wallet::shielded_ops {

enum class OpStatus : uint8_t {
    Ok              = 0,
    InvalidParams   = 1,
    InsufficientFunds = 2,
    StoreError      = 3,
    ProofError      = 4,
    InternalError   = 5,
};

// ── Shield: transparent → private ────────────────────────────────────

struct ShieldParams {
    uint64_t value_una;         ///< amount to shield
    uint32_t current_height;
};

struct ShieldResult {
    OpStatus status = OpStatus::InternalError;
    consensus::shielded::Hash commitment{};
    consensus::shielded::Hash nullifier_key{};  ///< secret_key for this note
    uint64_t leaf_index = 0;
    std::vector<uint8_t> output_proof;
    std::string error;
};

/**
 * Create a shielded note from transparent value.
 *
 * This does NOT create or sign the transparent transaction. It:
 *   1. Generates a fresh secret_key (random)
 *   2. Derives public_key = Poseidon(secret_key, 0)
 *   3. Generates random note randomness
 *   4. Computes commitment = NoteCommitment(value, pk, randomness)
 *   5. Generates output proof
 *   6. Stores the note in the ShieldedNoteStore
 *
 * The caller is responsible for building a v5 transaction that:
 *   - Spends transparent UTXOs totaling value_una + fee
 *   - Carries a ShieldedBundle with this commitment + proof
 */
ShieldResult Shield(ShieldParams params,
                    ShieldedNoteStore& store,
                    consensus::shielded::CommitmentTree& tree);

// ── Unshield: private → transparent ──────────────────────────────────

struct UnshieldParams {
    uint64_t leaf_index;        ///< which note to spend
    uint32_t current_height;
};

struct UnshieldResult {
    OpStatus status = OpStatus::InternalError;
    consensus::shielded::Hash nullifier{};
    consensus::shielded::Hash anchor{};
    uint64_t value_una = 0;
    std::vector<uint8_t> spend_proof;
    std::string error;
};

/**
 * Spend a shielded note, producing a nullifier + spend proof.
 *
 * The caller builds a v5 transaction that:
 *   - Carries a ShieldedBundle with this nullifier + proof
 *   - Has a transparent output for value_una - fee
 */
UnshieldResult Unshield(UnshieldParams params,
                        ShieldedNoteStore& store,
                        const consensus::shielded::CommitmentTree& tree);

// ── Wallet runtime-backed helpers ────────────────────────────────────

/**
 * Ensure the wallet-scoped shielded runtime is open and its tree rebuilt
 * from persisted shielded chain leaves.
 */
bool EnsureWalletRuntime(dinero::WalletManager& wallet, std::string* error = nullptr);

/**
 * Prepare a new local shielded note without guessing its eventual
 * on-chain leaf index. The note remains pending until block sync
 * confirms its commitment in the shielded output stream.
 */
ShieldResult PrepareShield(ShieldParams params, dinero::WalletManager& wallet);

/**
 * Spend a confirmed shielded note using the wallet-maintained shielded
 * commitment tree built from connected blocks.
 */
UnshieldResult UnshieldConfirmed(UnshieldParams params, dinero::WalletManager& wallet);

/**
 * Feed a newly connected block into the wallet-side shielded sync state.
 * Only shielded bundle transactions are decoded; no legacy confidential/ring lane
 * logic is involved.
 */
bool ProcessConfirmedBlock(dinero::WalletManager& wallet,
                           uint32_t height,
                           const std::vector<dinero::Transaction>& transactions,
                           std::string* error = nullptr);

/**
 * Replay shielded receive detection for an already-connected block.
 *
 * Unlike ProcessConfirmedBlock(), this is idempotent for wallet rescans:
 * it uses the persisted shielded chain-leaf table to recover the real
 * leaf_index for each output instead of appending duplicate leaves.
 * The wallet must be unlocked so incoming viewing keys can be derived.
 */
bool RescanConfirmedBlock(dinero::WalletManager& wallet,
                          uint32_t height,
                          const std::vector<dinero::Transaction>& transactions,
                          std::string* error = nullptr);

/**
 * Roll back the wallet-side shielded sync state for a disconnected tip block.
 */
bool ProcessDisconnectedBlock(dinero::WalletManager& wallet,
                              uint32_t height,
                              const dinero::Block& block,
                              std::string* error = nullptr);

std::vector<ShieldedNote> ListShieldedNotes(dinero::WalletManager& wallet, bool include_pending);
uint64_t GetShieldedBalance(dinero::WalletManager& wallet);
uint64_t GetShieldedTreeSize(dinero::WalletManager& wallet);

// ── Issue #273: size-aware fee floor ─────────────────────────────────

/// Safety margin (una) added on top of the exact mempool floor by
/// RequiredFeeForTx. The explicit-fee field is fixed-width (8 bytes) so
/// the fee VALUE cannot move the vsize, but proof bytes can drift a few
/// bytes between two otherwise-identical builds.
inline constexpr uint64_t kFeeSizingMarginUna = 16;

/**
 * Minimum fee (una) the mempool will accept for `tx` at `min_fee_rate`
 * (una/vbyte), plus kFeeSizingMarginUna. The mempool's acceptance
 * criterion is `fee / tx.GetVirtualSize() >= min_fee_rate`, so this is
 * `ceil(min_fee_rate * vsize) + margin`. Measure on the FINAL tx shape
 * (bundle attached, witnesses signed) — the v6 shielded bundle counts in
 * base serialization, so attaching it grows vsize by kilobytes.
 */
uint64_t RequiredFeeForTx(const dinero::Transaction& tx, double min_fee_rate);

// ── Phase 3 wave 3b: shield-side bundle attachment ───────────────────

struct AttachShieldResult {
    OpStatus status = OpStatus::InternalError;
    consensus::shielded::Hash commitment{};
    consensus::shielded::Hash nullifier_key{};   ///< secret_key for the new note
    consensus::shielded::Hash public_key{};
    consensus::shielded::Hash randomness{};
    uint64_t bundle_bytes = 0;
    std::string error;
};

/**
 * Pure helper — given an unsigned transparent envelope, generate a fresh
 * shielded note (sk/pk/randomness), build a one-output bundle that shields
 * `value_una`, and attach it to `tx.shielded_bundle_bytes`. Does NOT persist
 * the note anywhere; the caller is responsible for storing the returned
 * `nullifier_key` + `commitment` + `randomness` if it intends to spend later.
 *
 * Caller responsibilities BEFORE invoking:
 *   - tx.version is a shielded version (legacy v5 or bundle-committing v6)
 *   - tx.vin populated (transparent inputs to be spent)
 *   - tx.vout populated (change output, if any — recipient is shielded)
 *   - tx.lockTime set
 *   - tx.has_explicit_fee = true; tx.explicit_fee = the actual fee
 *   - Witness fields LEFT UNSET (sign transparent inputs AFTER this call;
 *     BIP143 sighash does not commit to shielded_bundle_bytes, so order is OK).
 */
// `cv_bound`: when true, emit cv-bound (0x03/0x04) proofs that bind the
// Pedersen value commitment to the in-circuit note value (audit Critical #1).
// The caller decides this from the tx's expected mining height vs
// Params().shielded_cv_binding_activation_height (see the Attach* wrappers).
// Default false = legacy (0x01/0x02), correct while activation is inert.
AttachShieldResult BuildShieldBundleForTx(dinero::Transaction& tx,
                                          uint64_t value_una,
                                          bool cv_bound = false);

/**
 * Wallet-manager-backed wrapper around `BuildShieldBundleForTx` that ALSO
 * persists the new note to the wallet's pending-note store. RPC handler
 * (`wallet.shield`) uses this; tests use the pure helper above.
 *
 * `persist=false` is the dry-run mode for size-aware fee measurement
 * (issue #273): build + attach only, do NOT store the pending note. The
 * resulting tx is for measurement and must be discarded.
 */
AttachShieldResult AttachShieldOutputBundle(dinero::Transaction& tx,
                                            uint64_t value_una,
                                            dinero::WalletManager& wallet,
                                            uint32_t current_height,
                                            bool persist = true);

// ── Phase 3 wave 3c: unshield-side bundle attachment ─────────────────

/// One shielded note being spent. Pure-data: no tree handle.
struct UnshieldNoteInput {
    consensus::shielded::Hash secret_key{};
    consensus::shielded::Hash randomness{};
    consensus::shielded::Hash d{};            ///< 32-byte packed diversifier bound into the note commitment
    consensus::shielded::Hash anchor{};       ///< wallet-tree root at spend time
    uint64_t                  leaf_index = 0;
    uint64_t                  value_una = 0;
    std::array<consensus::shielded::Hash,
               consensus::shielded::TREE_DEPTH> merkle_path{};
};

struct AttachUnshieldResult {
    OpStatus status = OpStatus::InternalError;
    consensus::shielded::Hash nullifier{};
    consensus::shielded::Hash anchor{};
    uint64_t bundle_bytes = 0;
    std::string error;
};

/**
 * Pure helper — given an unsigned transparent envelope (shielded version, single
 * transparent vout for the unshield recipient, lockTime + explicit_fee
 * already set), generate a Spartan spend proof for `note`, build a
 * bundle with one spend / zero outputs, and attach it to
 * `tx.shielded_bundle_bytes`.
 *
 * Bundle's `value_balance = -note.value_una` (Dinero convention:
 * negative = unshield).
 *
 * Caller responsibilities BEFORE invoking:
 *   - tx.version is a shielded version (legacy v5 or bundle-committing v6)
 *   - tx.vin == empty (unshield has no transparent inputs)
 *   - tx.vout == [{ value = note.value_una - fee_una, scriptPubKey = self_taproot }]
 *   - tx.lockTime + tx.explicit_fee = fee_una set
 *
 * The binding sig commits to that exact transparent envelope. Any
 * downstream mutation of vout (recipient swap, value tweak) breaks
 * verification.
 */
AttachUnshieldResult BuildUnshieldBundleForTx(dinero::Transaction& tx,
                                              const UnshieldNoteInput& note,
                                              uint64_t fee_una,
                                              bool cv_bound = false);

/**
 * Wallet-bound wrapper around `BuildUnshieldBundleForTx`. Looks up the
 * confirmed unspent note at `note_leaf_index`, builds the auth path from
 * the wallet-side commitment tree, calls the pure helper, and marks the
 * note pending-spent (`spent=1, spent_height=0`) so the wallet selector
 * does not pick it again before the tx mines.
 *
 * Returns `InvalidParams` if the note is missing, unconfirmed, already
 * spent (or pending-spent), or has insufficient value.
 *
 * `persist=false` is the dry-run mode for size-aware fee measurement
 * (issue #273): build + attach only, do NOT mark the note pending-spent.
 * The resulting tx is for measurement and must be discarded.
 */
AttachUnshieldResult AttachUnshieldInputBundle(dinero::Transaction& tx,
                                               uint64_t note_leaf_index,
                                               uint64_t fee_una,
                                               dinero::WalletManager& wallet,
                                               bool persist = true);

/**
 * Coin selector for unshield: returns the smallest unspent confirmed note
 * with `value_una >= min_value_una`, or std::nullopt if none exists.
 * Used by the RPC to translate `wallet.unshield(amount, fee)` into a
 * concrete `note_leaf_index`.
 */
std::optional<ShieldedNote> SelectUnshieldNote(
    dinero::WalletManager& wallet,
    uint64_t min_value_una);

// ── Phase 3 wave 3d: shielded→shielded self-transfer ─────────────────

struct AttachTransferResult {
    OpStatus status = OpStatus::InternalError;
    consensus::shielded::Hash spend_nullifier{};
    consensus::shielded::Hash spend_anchor{};
    consensus::shielded::Hash out_commitment{};
    consensus::shielded::Hash out_secret_key{};   ///< caller persists for later spend
    consensus::shielded::Hash out_public_key{};
    consensus::shielded::Hash out_randomness{};
    uint64_t                  out_value_una = 0;
    uint64_t                  bundle_bytes = 0;
    std::string error;
};

/**
 * Pure helper — given an unsigned shielded envelope with empty vin AND empty vout,
 * spend `note` and create exactly one fresh self-controlled output note
 * with value = note.value_una - fee_una. Bundle's
 * `value_balance = -fee_una` (Dinero convention: net out-of-pool flow,
 * which here is just the miner fee).
 *
 * Caller responsibilities BEFORE invoking:
 *   - tx.version is a shielded version (legacy v5 or bundle-committing v6)
 *   - tx.vin == empty
 *   - tx.vout == empty
 *   - tx.witness_version = 0 (forces SegWit marker so empty-vin/empty-vout
 *     decodes unambiguously)
 *   - tx.explicit_fee = fee_una set
 *
 * Returns the freshly-generated output note's secret material so the
 * wallet wrapper can persist it as a pending note.
 */
AttachTransferResult BuildTransferBundleForTx(dinero::Transaction& tx,
                                              const UnshieldNoteInput& note,
                                              uint64_t fee_una,
                                              bool cv_bound = false);

/**
 * Wallet-bound wrapper. Looks up the note at `note_leaf_index`, builds
 * the auth path, calls the pure helper, marks the old note pending-spent,
 * and registers the new self-output via `AddPendingNote` so the wallet
 * tree picks it up at confirmation time.
 *
 * `persist=false` is the dry-run mode for size-aware fee measurement
 * (issue #273): build + attach only — no pending-spent mark, no pending
 * note. The resulting tx is for measurement and must be discarded.
 */
AttachTransferResult AttachTransferInputBundle(dinero::Transaction& tx,
                                               uint64_t note_leaf_index,
                                               uint64_t fee_una,
                                               dinero::WalletManager& wallet,
                                               bool persist = true);

/**
 * Coin selector for transfer: smallest unspent confirmed note with
 * value_una >= min_value_una (= fee_una + a small dust floor). Identical
 * shape to SelectUnshieldNote; aliased for caller readability.
 */
std::optional<ShieldedNote> SelectTransferNote(
    dinero::WalletManager& wallet,
    uint64_t min_value_una);

// ── Phase 3 wave 3e: multi-spend + change-output self-transfer ───────

/// One freshly-created self-controlled output note in a multi-transfer.
struct TransferOutputMaterial {
    consensus::shielded::Hash commitment{};
    consensus::shielded::Hash secret_key{};
    consensus::shielded::Hash public_key{};
    consensus::shielded::Hash randomness{};
    uint64_t                  value_una = 0;
};

struct AttachMultiTransferResult {
    OpStatus status = OpStatus::InternalError;
    std::vector<consensus::shielded::Hash> spend_nullifiers;
    std::vector<consensus::shielded::Hash> spend_anchors;
    std::vector<TransferOutputMaterial>    outputs;
    uint64_t bundle_bytes = 0;
    std::string error;
};

/**
 * Pure helper — spend N input notes and create M output notes inside a
 * single bundle, with `value_balance = -fee_una`. Caller supplies the
 * spend-side note material as a vector and the desired output values as
 * a vector. Helper generates Spartan proofs for each side, builds the
 * bundle, and attaches it.
 *
 * Constraint: sum(spend.value_una) == sum(output_values) + fee_una.
 *
 * Caller responsibilities BEFORE invoking:
 *   - tx.version is a shielded version (legacy v5 or bundle-committing v6)
 *   - tx.vin == empty, tx.vout == empty, tx.witness_version = 0
 *   - tx.explicit_fee = fee_una set
 */
AttachMultiTransferResult BuildMultiTransferBundleForTx(
    dinero::Transaction& tx,
    const std::vector<UnshieldNoteInput>& spends,
    const std::vector<uint64_t>& output_values,
    uint64_t fee_una,
    bool cv_bound = false);

/**
 * Wallet-bound wrapper. Looks up each note at the supplied leaf indices,
 * builds auth paths (all anchored at the wallet-tree root), calls the
 * pure helper, marks every spent note pending-spent, and persists every
 * new self-output as a pending note.
 *
 * `persist=false` is the dry-run mode for size-aware fee measurement
 * (issue #273): build + attach only — no pending-spent marks, no pending
 * notes. The resulting tx is for measurement and must be discarded.
 */
AttachMultiTransferResult AttachMultiTransferInputBundle(
    dinero::Transaction& tx,
    const std::vector<uint64_t>& note_leaf_indices,
    const std::vector<uint64_t>& output_values,
    uint64_t fee_una,
    dinero::WalletManager& wallet,
    bool persist = true);

/**
 * Greedy multi-note coin selector. Picks the smallest unspent confirmed
 * notes (ascending value) until cumulative value >= target_value_una.
 * Returns std::nullopt if total available is insufficient.
 */
std::optional<std::vector<ShieldedNote>> SelectTransferNotesForValue(
    dinero::WalletManager& wallet,
    uint64_t target_value_una);

// ── Phase 5 Wave 3d: any-recipient transfer ──────────────────────────

/// Recipient parameters for an addressed shielded output. `d` (11 bytes)
/// and `pk_d` (32-byte x-only) come from `DecodeShieldedAddress`; the
/// commitment uses `Poseidon(d, pk_note)` for `addr_bind`, where
/// `pk_note = Poseidon(DeriveNoteSpendKey(rcm), 0)` so the receiver can
/// re-derive the spend secret from the encrypted-note plaintext.
struct AddressedRecipient {
    std::array<uint8_t, 11>     d{};
    consensus::shielded::Hash   pk_d{};
    uint64_t                    value_una = 0;
};

struct AttachAddressedTransferResult {
    OpStatus status = OpStatus::InternalError;
    std::vector<consensus::shielded::Hash> spend_nullifiers;
    consensus::shielded::Hash recipient_commitment{};
    bool                      had_change = false;
    consensus::shielded::Hash change_commitment{};
    consensus::shielded::Hash change_secret_key{};
    consensus::shielded::Hash change_public_key{};
    consensus::shielded::Hash change_randomness{};
    uint64_t                  change_value_una = 0;
    uint64_t                  bundle_bytes = 0;
    std::string error;
};

/**
 * Pure helper. Spends N input notes and creates one addressed output
 * for `recipient` plus (if `change_value_una > 0`) one self-change
 * output. Bundle's `value_balance = -fee_una`. Constraint:
 * `sum(spend.value_una) == recipient.value_una + change_value_una +
 * fee_una`.
 *
 * The recipient output uses the addressed-transfer convention:
 *   - commitment = NoteCommitment(d_packed, pk_note, value, rcm)
 *     where pk_note = Poseidon(DeriveNoteSpendKey(rcm), 0)
 *   - encrypted_note = EncryptNoteForRecipient(d, pk_d, plaintext)
 * The change output uses the legacy self-recipient convention so the
 * existing wallet-side bookkeeping (AddPendingNote) keeps working
 * without the receive-side scanner shipping. (Wave 3e migrates change
 * to the same addressed convention as the recipient.)
 */
/// Optional 512-byte memo for the addressed recipient note. Zero-pad
/// shorter memos; truncate longer ones at the call site. nullptr =
/// all-zero memo (current default).
AttachAddressedTransferResult BuildAddressedTransferBundleForTx(
    dinero::Transaction& tx,
    const std::vector<UnshieldNoteInput>& spends,
    const AddressedRecipient& recipient,
    uint64_t change_value_una,
    uint64_t fee_una,
    const std::array<uint8_t, 512>* recipient_memo = nullptr,
    bool cv_bound = false);

/**
 * Wallet-bound wrapper. Looks up `note_leaf_indices`, decodes
 * `recipient_address`, builds the bundle, marks spends pending-spent,
 * and persists the legacy-style change note (if any) via
 * `AddPendingNote`.
 *
 * `persist=false` is the dry-run mode for size-aware fee measurement
 * (issue #273): build + attach only — no pending-spent marks, no pending
 * change note. The resulting tx is for measurement and must be discarded.
 */
AttachAddressedTransferResult AttachAddressedTransferInputBundle(
    dinero::Transaction& tx,
    const std::vector<uint64_t>& note_leaf_indices,
    const std::string& recipient_address,
    uint64_t recipient_value_una,
    uint64_t fee_una,
    dinero::WalletManager& wallet,
    const std::string* recipient_memo_utf8 = nullptr,
    bool persist = true);

} // namespace dinero::wallet::shielded_ops
