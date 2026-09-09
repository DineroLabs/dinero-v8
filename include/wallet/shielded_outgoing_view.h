#pragma once

// Wallet-only outgoing shielded-note recovery.  This format is deliberately
// separate from consensus: encrypted_note remains opaque to block validation.

#include "consensus/shielded/shielded_tx.h"
#include "wallet/shielded_derivation.h"

#include <array>
#include <cstdint>
#include <vector>

namespace dinero::wallet::shielded {

inline constexpr uint32_t kOutgoingRecoveryDormant = UINT32_MAX;
inline constexpr uint8_t kOutgoingEnvelopeVersion = 0x03;
inline constexpr uint8_t kOutgoingPlaintextVersion = 0x02;
inline constexpr std::size_t kOutgoingPlaintextBytes = 129;
inline constexpr std::size_t kOutgoingCiphertextBytes = 145;
inline constexpr std::size_t kOutgoingEnvelopeBytes = 757;

enum class OutgoingRecoveryVerdict : uint8_t {
  Ok = 0,
  LegacyNoOutgoingRecovery,
  InvalidLength,
  UnsupportedEnvelopeVersion,
  SpendAuthorityRequired,
  OutgoingRecoveryInactive,
  NotForViewerOrTampered,
  UnsupportedPlaintextVersion,
  InvalidEncryptionKey,
  InvalidSpendKey,
  InvalidNullifierKeyCommitment,
  InvalidEphemeralSecret,
  RecipientAuthenticationFailed,
  EphemeralKeyMismatch,
  CommitmentMismatch,
  InvalidActivationPolicy,
};

const char *
OutgoingRecoveryVerdictName(OutgoingRecoveryVerdict verdict) noexcept;

/** Explicit sentinel-aware activation.  In particular MAX/MAX is dormant. */
bool IsOutgoingRuleActive(uint32_t height, uint32_t activation_height) noexcept;

/** Outgoing recovery may never precede recipient-bound spend authority. */
bool IsOutgoingActivationPolicyValid(
    uint32_t spend_auth_activation_height,
    uint32_t outgoing_activation_height) noexcept;

struct OutgoingPublicOutput {
  consensus::shielded::Hash commitment{};
  consensus::shielded::ValueCommitment value_commitment{};
};

struct OutgoingConstructionInput {
  consensus::shielded::Hash ovk{};
  OutgoingPublicOutput public_output{};
  EncryptedNote recipient_encrypted_note{};
  consensus::shielded::Hash pk_d_enc{};
  consensus::shielded::Hash pk_d_spend{};
  consensus::shielded::Hash nfk_commitment{};
  consensus::shielded::Hash esk_normalized{};

  ~OutgoingConstructionInput();
};

struct OutgoingConstructionResult {
  OutgoingRecoveryVerdict verdict = OutgoingRecoveryVerdict::InvalidLength;
  std::vector<uint8_t> envelope;
};

/** Build the exact 757-byte v3 envelope.  Callers must already have selected
 *  an active policy; this function validates the cryptographic inputs. */
OutgoingConstructionResult
BuildOutgoingEnvelope(const OutgoingConstructionInput &input);

struct OutgoingRecoveredNote {
  Diversifier d{};
  uint64_t value_una = 0;
  consensus::shielded::Hash rcm{};
  std::array<uint8_t, 512> memo{};
  consensus::shielded::Hash pk_d_enc{};
  consensus::shielded::Hash pk_d_spend{};
  consensus::shielded::Hash nfk_commitment{};
  consensus::shielded::Hash esk_normalized{};
  AddressPayload recipient_address_payload{};

  ~OutgoingRecoveredNote();
};

struct OutgoingRecoveryResult {
  OutgoingRecoveryVerdict verdict = OutgoingRecoveryVerdict::InvalidLength;
  OutgoingRecoveredNote note{};
};

/** Recover sender metadata and authenticate it back to both the embedded
 *  recipient ciphertext and the on-chain note commitment. */
OutgoingRecoveryResult
RecoverOutgoingNote(const consensus::shielded::Hash &ovk,
                    const OutgoingPublicOutput &public_output,
                    const std::vector<uint8_t> &encoded, uint32_t output_height,
                    uint32_t spend_auth_activation_height,
                    uint32_t outgoing_activation_height);

} // namespace dinero::wallet::shielded
