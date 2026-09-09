#include "wallet/shielded_outgoing_view.h"

#include "crypto/evp_secp256k1.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <string_view>

namespace dinero::wallet::shielded {
namespace {

using Hash = consensus::shielded::Hash;
using ValueCommitment = consensus::shielded::ValueCommitment;

constexpr std::string_view kSaltDomain = "DIN/v8/shielded/outgoing/salt/v1";
constexpr std::string_view kKeyDomain = "DIN/v8/shielded/outgoing/key/v1";
constexpr std::string_view kAadDomain = "DIN/v8/shielded/outgoing/aad/v1";

std::array<uint8_t, 32> Sha256(const uint8_t *data, std::size_t size) {
  std::array<uint8_t, 32> out{};
  SHA256(data, size, out.data());
  return out;
}

std::optional<std::array<uint8_t, 32>>
Hkdf(const uint8_t *salt, std::size_t salt_size, const uint8_t *ikm,
     std::size_t ikm_size, std::string_view info) {
  std::array<uint8_t, 32> prk{};
  unsigned int length = 0;
  if (HMAC(EVP_sha256(), salt, static_cast<int>(salt_size), ikm, ikm_size,
           prk.data(), &length) == nullptr ||
      length != prk.size()) {
    OPENSSL_cleanse(prk.data(), prk.size());
    return std::nullopt;
  }
  std::vector<uint8_t> expand(info.begin(), info.end());
  expand.push_back(1);
  std::array<uint8_t, 32> out{};
  length = 0;
  const bool ok =
      HMAC(EVP_sha256(), prk.data(), static_cast<int>(prk.size()),
           expand.data(), expand.size(), out.data(), &length) != nullptr &&
      length == out.size();
  OPENSSL_cleanse(prk.data(), prk.size());
  if (!ok) {
    OPENSSL_cleanse(out.data(), out.size());
    return std::nullopt;
  }
  return out;
}

std::vector<uint8_t> PublicContext(const OutgoingPublicOutput &output,
                                   const Hash &epk) {
  std::vector<uint8_t> result;
  result.reserve(97);
  result.insert(result.end(), output.commitment.begin(),
                output.commitment.end());
  result.insert(result.end(), output.value_commitment.begin(),
                output.value_commitment.end());
  result.insert(result.end(), epk.begin(), epk.end());
  return result;
}

std::optional<std::array<uint8_t, 32>>
RecoveryKey(const Hash &ovk, const OutgoingPublicOutput &output,
            const uint8_t *recipient) {
  Hash epk{};
  std::memcpy(epk.data(), recipient, epk.size());
  const auto context = PublicContext(output, epk);
  const auto recipient_hash = Sha256(recipient, kEncryptedNoteBytes);
  std::vector<uint8_t> salt_preimage(kSaltDomain.begin(), kSaltDomain.end());
  salt_preimage.insert(salt_preimage.end(), context.begin(), context.end());
  salt_preimage.insert(salt_preimage.end(), recipient_hash.begin(),
                       recipient_hash.end());
  const auto salt = Sha256(salt_preimage.data(), salt_preimage.size());
  return Hkdf(salt.data(), salt.size(), ovk.data(), ovk.size(), kKeyDomain);
}

void ClearRecoveredNote(OutgoingRecoveredNote &note) noexcept {
  OPENSSL_cleanse(&note, sizeof(note));
}

std::vector<uint8_t> OutgoingAad(const OutgoingPublicOutput &output,
                                 const uint8_t *recipient) {
  Hash epk{};
  std::memcpy(epk.data(), recipient, epk.size());
  const auto context = PublicContext(output, epk);
  std::vector<uint8_t> aad(kAadDomain.begin(), kAadDomain.end());
  aad.push_back(kOutgoingEnvelopeVersion);
  aad.insert(aad.end(), context.begin(), context.end());
  aad.insert(aad.end(), recipient, recipient + kEncryptedNoteBytes);
  return aad;
}

bool AeadEncrypt(const std::array<uint8_t, 32> &key,
                 const std::vector<uint8_t> &aad, const uint8_t *plaintext,
                 std::size_t plaintext_size, uint8_t *ciphertext,
                 uint8_t *tag) {
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  if (!ctx)
    return false;
  std::array<uint8_t, 12> nonce{};
  int aad_written = 0;
  int written = 0;
  int final_written = 0;
  bool ok =
      EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr,
                         nullptr) == 1 &&
      EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, nonce.size(),
                          nullptr) == 1 &&
      EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.data()) ==
          1 &&
      EVP_EncryptUpdate(ctx, nullptr, &aad_written, aad.data(), aad.size()) ==
          1 &&
      EVP_EncryptUpdate(ctx, ciphertext, &written, plaintext,
                        static_cast<int>(plaintext_size)) == 1 &&
      EVP_EncryptFinal_ex(ctx, ciphertext + written, &final_written) == 1 &&
      EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16, tag) == 1;
  EVP_CIPHER_CTX_free(ctx);
  return ok &&
         static_cast<std::size_t>(written + final_written) == plaintext_size;
}

bool AeadDecrypt(const std::array<uint8_t, 32> &key,
                 const std::vector<uint8_t> &aad, const uint8_t *ciphertext,
                 std::size_t ciphertext_size, const uint8_t *tag,
                 uint8_t *plaintext) {
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  if (!ctx)
    return false;
  std::array<uint8_t, 12> nonce{};
  int aad_written = 0;
  int written = 0;
  int final_written = 0;
  bool ok = EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr,
                               nullptr) == 1 &&
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, nonce.size(),
                                nullptr) == 1 &&
            EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(),
                               nonce.data()) == 1 &&
            EVP_DecryptUpdate(ctx, nullptr, &aad_written, aad.data(),
                              aad.size()) == 1 &&
            EVP_DecryptUpdate(ctx, plaintext, &written, ciphertext,
                              static_cast<int>(ciphertext_size)) == 1 &&
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, 16,
                                const_cast<uint8_t *>(tag)) == 1 &&
            EVP_DecryptFinal_ex(ctx, plaintext + written, &final_written) == 1;
  EVP_CIPHER_CTX_free(ctx);
  return ok &&
         static_cast<std::size_t>(written + final_written) == ciphertext_size;
}

secp256k1_context *SecpCtx() {
  return dinero::crypto::GetSecp256k1ContextSignVerify();
}

bool ValidXOnly(const Hash &x) {
  secp256k1_xonly_pubkey key{};
  return secp256k1_xonly_pubkey_parse(SecpCtx(), &key, x.data()) == 1;
}

bool ValidScalar(const Hash &scalar) {
  return secp256k1_ec_seckey_verify(SecpCtx(), scalar.data()) == 1;
}

// Return false when scalar*point is odd-y.  Merely comparing x coordinates
// would accept n-scalar, which has the same x coordinate and opposite parity.
bool MultiplyEvenY(const Hash &scalar, const Hash &point_x, Hash &product_x) {
  std::array<uint8_t, 33> encoded{};
  encoded[0] = 0x02;
  std::memcpy(encoded.data() + 1, point_x.data(), point_x.size());
  secp256k1_pubkey point{};
  if (!ValidScalar(scalar) ||
      secp256k1_ec_pubkey_parse(SecpCtx(), &point, encoded.data(),
                                encoded.size()) != 1 ||
      secp256k1_ec_pubkey_tweak_mul(SecpCtx(), &point, scalar.data()) != 1) {
    return false;
  }
  secp256k1_xonly_pubkey xonly{};
  int parity = 0;
  return secp256k1_xonly_pubkey_from_pubkey(SecpCtx(), &xonly, &parity,
                                            &point) == 1 &&
         secp256k1_xonly_pubkey_serialize(SecpCtx(), product_x.data(),
                                          &xonly) == 1 &&
         parity == 0;
}

} // namespace

OutgoingConstructionInput::~OutgoingConstructionInput() {
  OPENSSL_cleanse(ovk.data(), ovk.size());
  OPENSSL_cleanse(esk_normalized.data(), esk_normalized.size());
}

OutgoingRecoveredNote::~OutgoingRecoveredNote() {
  OPENSSL_cleanse(this, sizeof(*this));
}

const char *
OutgoingRecoveryVerdictName(OutgoingRecoveryVerdict verdict) noexcept {
  switch (verdict) {
  case OutgoingRecoveryVerdict::Ok:
    return "Ok";
  case OutgoingRecoveryVerdict::LegacyNoOutgoingRecovery:
    return "LegacyNoOutgoingRecovery";
  case OutgoingRecoveryVerdict::InvalidLength:
    return "InvalidLength";
  case OutgoingRecoveryVerdict::UnsupportedEnvelopeVersion:
    return "UnsupportedEnvelopeVersion";
  case OutgoingRecoveryVerdict::SpendAuthorityRequired:
    return "SpendAuthorityRequired";
  case OutgoingRecoveryVerdict::OutgoingRecoveryInactive:
    return "OutgoingRecoveryInactive";
  case OutgoingRecoveryVerdict::NotForViewerOrTampered:
    return "NotForViewerOrTampered";
  case OutgoingRecoveryVerdict::UnsupportedPlaintextVersion:
    return "UnsupportedPlaintextVersion";
  case OutgoingRecoveryVerdict::InvalidEncryptionKey:
    return "InvalidEncryptionKey";
  case OutgoingRecoveryVerdict::InvalidSpendKey:
    return "InvalidSpendKey";
  case OutgoingRecoveryVerdict::InvalidNullifierKeyCommitment:
    return "InvalidNullifierKeyCommitment";
  case OutgoingRecoveryVerdict::InvalidEphemeralSecret:
    return "InvalidEphemeralSecret";
  case OutgoingRecoveryVerdict::RecipientAuthenticationFailed:
    return "RecipientAuthenticationFailed";
  case OutgoingRecoveryVerdict::EphemeralKeyMismatch:
    return "EphemeralKeyMismatch";
  case OutgoingRecoveryVerdict::CommitmentMismatch:
    return "CommitmentMismatch";
  case OutgoingRecoveryVerdict::InvalidActivationPolicy:
    return "InvalidActivationPolicy";
  }
  return "InvalidActivationPolicy";
}

bool IsOutgoingRuleActive(uint32_t height,
                          uint32_t activation_height) noexcept {
  return activation_height != kOutgoingRecoveryDormant &&
         height >= activation_height;
}

bool IsOutgoingActivationPolicyValid(
    uint32_t spend_auth_activation_height,
    uint32_t outgoing_activation_height) noexcept {
  return outgoing_activation_height == kOutgoingRecoveryDormant ||
         (spend_auth_activation_height != kOutgoingRecoveryDormant &&
          outgoing_activation_height >= spend_auth_activation_height);
}

OutgoingConstructionResult
BuildOutgoingEnvelope(const OutgoingConstructionInput &input) {
  OutgoingConstructionResult result;
  if (!ValidXOnly(input.pk_d_enc)) {
    result.verdict = OutgoingRecoveryVerdict::InvalidEncryptionKey;
    return result;
  }
  if (!ValidXOnly(input.pk_d_spend)) {
    result.verdict = OutgoingRecoveryVerdict::InvalidSpendKey;
    return result;
  }
  if (!ValidScalar(input.nfk_commitment)) {
    result.verdict = OutgoingRecoveryVerdict::InvalidNullifierKeyCommitment;
    return result;
  }
  if (!ValidScalar(input.esk_normalized)) {
    result.verdict = OutgoingRecoveryVerdict::InvalidEphemeralSecret;
    return result;
  }

  // Refuse to emit an envelope that cannot later recover the exact recipient
  // output.  Merely checking field shapes is insufficient: a valid but stale
  // esk, spend key, or nullifier-key commitment would encrypt successfully and
  // strand the sender's history even though the recipient note itself remains
  // valid on chain.
  auto recipient = TryDecryptNoteWithEphemeralSecret(
      input.esk_normalized, input.pk_d_enc,
      input.recipient_encrypted_note);
  if (!recipient) {
    result.verdict = OutgoingRecoveryVerdict::RecipientAuthenticationFailed;
    return result;
  }
  Hash expected_epk{};
  try {
    const Hash p_d = HashToPoint(recipient->d, kDstDiv);
    if (!MultiplyEvenY(input.esk_normalized, p_d, expected_epk) ||
        !std::equal(expected_epk.begin(), expected_epk.end(),
                    input.recipient_encrypted_note.begin())) {
      OPENSSL_cleanse(&*recipient, sizeof(*recipient));
      result.verdict = OutgoingRecoveryVerdict::EphemeralKeyMismatch;
      return result;
    }
  } catch (...) {
    OPENSSL_cleanse(&*recipient, sizeof(*recipient));
    result.verdict = OutgoingRecoveryVerdict::EphemeralKeyMismatch;
    return result;
  }
  Hash d_packed{};
  std::memcpy(d_packed.data(), recipient->d.data(), recipient->d.size());
  Hash value{};
  for (int i = 0; i < 8; ++i) {
    value[31 - i] =
        static_cast<uint8_t>((recipient->value_una >> (8 * i)) & 0xffU);
  }
  const Hash ownership_key = consensus::shielded::AuthRecipientCommitmentKey(
      input.pk_d_spend, input.nfk_commitment);
  const Hash expected_commitment = consensus::shielded::NoteCommitment(
      d_packed, ownership_key, value, recipient->rcm);
  OPENSSL_cleanse(&*recipient, sizeof(*recipient));
  if (expected_commitment != input.public_output.commitment) {
    result.verdict = OutgoingRecoveryVerdict::CommitmentMismatch;
    return result;
  }

  std::array<uint8_t, kOutgoingPlaintextBytes> plaintext{};
  plaintext[0] = kOutgoingPlaintextVersion;
  std::memcpy(plaintext.data() + 1, input.pk_d_enc.data(), 32);
  std::memcpy(plaintext.data() + 33, input.pk_d_spend.data(), 32);
  std::memcpy(plaintext.data() + 65, input.nfk_commitment.data(), 32);
  std::memcpy(plaintext.data() + 97, input.esk_normalized.data(), 32);

  auto key = RecoveryKey(input.ovk, input.public_output,
                         input.recipient_encrypted_note.data());
  if (!key) {
    OPENSSL_cleanse(plaintext.data(), plaintext.size());
    result.verdict = OutgoingRecoveryVerdict::NotForViewerOrTampered;
    return result;
  }
  const auto aad =
      OutgoingAad(input.public_output, input.recipient_encrypted_note.data());
  result.envelope.resize(kOutgoingEnvelopeBytes);
  result.envelope[0] = kOutgoingEnvelopeVersion;
  std::memcpy(result.envelope.data() + 1, input.recipient_encrypted_note.data(),
              kEncryptedNoteBytes);
  const bool ok =
      AeadEncrypt(*key, aad, plaintext.data(), plaintext.size(),
                  result.envelope.data() + 1 + kEncryptedNoteBytes,
                  result.envelope.data() + kOutgoingEnvelopeBytes - 16);
  OPENSSL_cleanse(key->data(), key->size());
  OPENSSL_cleanse(plaintext.data(), plaintext.size());
  if (!ok) {
    result.envelope.clear();
    result.verdict = OutgoingRecoveryVerdict::NotForViewerOrTampered;
    return result;
  }
  result.verdict = OutgoingRecoveryVerdict::Ok;
  return result;
}

OutgoingRecoveryResult
RecoverOutgoingNote(const Hash &ovk, const OutgoingPublicOutput &public_output,
                    const std::vector<uint8_t> &encoded, uint32_t output_height,
                    uint32_t spend_auth_activation_height,
                    uint32_t outgoing_activation_height) {
  OutgoingRecoveryResult result;
  if (!IsOutgoingActivationPolicyValid(spend_auth_activation_height,
                                       outgoing_activation_height)) {
    result.verdict = OutgoingRecoveryVerdict::InvalidActivationPolicy;
    return result;
  }
  if (encoded.size() == kEncryptedNoteBytes) {
    result.verdict = OutgoingRecoveryVerdict::LegacyNoOutgoingRecovery;
    return result;
  }
  if (encoded.size() != kOutgoingEnvelopeBytes) {
    result.verdict = OutgoingRecoveryVerdict::InvalidLength;
    return result;
  }
  if (encoded[0] != kOutgoingEnvelopeVersion) {
    result.verdict = OutgoingRecoveryVerdict::UnsupportedEnvelopeVersion;
    return result;
  }
  if (!IsOutgoingRuleActive(output_height, spend_auth_activation_height)) {
    result.verdict = OutgoingRecoveryVerdict::SpendAuthorityRequired;
    return result;
  }
  if (!IsOutgoingRuleActive(output_height, outgoing_activation_height)) {
    result.verdict = OutgoingRecoveryVerdict::OutgoingRecoveryInactive;
    return result;
  }

  const uint8_t *recipient = encoded.data() + 1;
  const uint8_t *outgoing = recipient + kEncryptedNoteBytes;
  auto key = RecoveryKey(ovk, public_output, recipient);
  if (!key) {
    result.verdict = OutgoingRecoveryVerdict::NotForViewerOrTampered;
    return result;
  }
  const auto aad = OutgoingAad(public_output, recipient);
  std::array<uint8_t, kOutgoingPlaintextBytes> plaintext{};
  const bool opened =
      AeadDecrypt(*key, aad, outgoing, kOutgoingPlaintextBytes,
                  outgoing + kOutgoingPlaintextBytes, plaintext.data());
  OPENSSL_cleanse(key->data(), key->size());
  if (!opened) {
    OPENSSL_cleanse(plaintext.data(), plaintext.size());
    result.verdict = OutgoingRecoveryVerdict::NotForViewerOrTampered;
    return result;
  }
  if (plaintext[0] != kOutgoingPlaintextVersion) {
    OPENSSL_cleanse(plaintext.data(), plaintext.size());
    result.verdict = OutgoingRecoveryVerdict::UnsupportedPlaintextVersion;
    return result;
  }
  std::memcpy(result.note.pk_d_enc.data(), plaintext.data() + 1, 32);
  std::memcpy(result.note.pk_d_spend.data(), plaintext.data() + 33, 32);
  std::memcpy(result.note.nfk_commitment.data(), plaintext.data() + 65, 32);
  std::memcpy(result.note.esk_normalized.data(), plaintext.data() + 97, 32);
  OPENSSL_cleanse(plaintext.data(), plaintext.size());
  if (!ValidXOnly(result.note.pk_d_enc)) {
    ClearRecoveredNote(result.note);
    result.verdict = OutgoingRecoveryVerdict::InvalidEncryptionKey;
    return result;
  }
  if (!ValidXOnly(result.note.pk_d_spend)) {
    ClearRecoveredNote(result.note);
    result.verdict = OutgoingRecoveryVerdict::InvalidSpendKey;
    return result;
  }
  if (!ValidScalar(result.note.nfk_commitment)) {
    ClearRecoveredNote(result.note);
    result.verdict =
        OutgoingRecoveryVerdict::InvalidNullifierKeyCommitment;
    return result;
  }
  if (!ValidScalar(result.note.esk_normalized)) {
    ClearRecoveredNote(result.note);
    result.verdict = OutgoingRecoveryVerdict::InvalidEphemeralSecret;
    return result;
  }

  EncryptedNote recipient_note{};
  std::memcpy(recipient_note.data(), recipient, recipient_note.size());
  auto note = TryDecryptNoteWithEphemeralSecret(
      result.note.esk_normalized, result.note.pk_d_enc, recipient_note);
  if (!note) {
    ClearRecoveredNote(result.note);
    result.verdict = OutgoingRecoveryVerdict::RecipientAuthenticationFailed;
    return result;
  }
  result.note.d = note->d;
  result.note.value_una = note->value_una;
  result.note.rcm = note->rcm;
  result.note.memo = note->memo;

  Hash p_d{};
  Hash expected_epk{};
  try {
    p_d = HashToPoint(note->d, kDstDiv);
  } catch (...) {
    OPENSSL_cleanse(&*note, sizeof(*note));
    ClearRecoveredNote(result.note);
    result.verdict = OutgoingRecoveryVerdict::EphemeralKeyMismatch;
    return result;
  }
  if (!MultiplyEvenY(result.note.esk_normalized, p_d, expected_epk) ||
      !std::equal(expected_epk.begin(), expected_epk.end(), recipient)) {
    OPENSSL_cleanse(&*note, sizeof(*note));
    ClearRecoveredNote(result.note);
    result.verdict = OutgoingRecoveryVerdict::EphemeralKeyMismatch;
    return result;
  }

  Hash d_packed{};
  std::memcpy(d_packed.data(), note->d.data(), note->d.size());
  Hash value{};
  for (int i = 0; i < 8; ++i) {
    value[31 - i] = static_cast<uint8_t>((note->value_una >> (8 * i)) & 0xff);
  }
  const Hash ownership_key = consensus::shielded::AuthRecipientCommitmentKey(
      result.note.pk_d_spend, result.note.nfk_commitment);
  const Hash expected_commitment = consensus::shielded::NoteCommitment(
      d_packed, ownership_key, value, note->rcm);
  if (expected_commitment != public_output.commitment) {
    OPENSSL_cleanse(&*note, sizeof(*note));
    ClearRecoveredNote(result.note);
    result.verdict = OutgoingRecoveryVerdict::CommitmentMismatch;
    return result;
  }
  result.note.recipient_address_payload = BuildAddressPayload(
      note->d, result.note.pk_d_enc, result.note.pk_d_spend,
      result.note.nfk_commitment);
  OPENSSL_cleanse(&*note, sizeof(*note));
  result.verdict = OutgoingRecoveryVerdict::Ok;
  return result;
}

} // namespace dinero::wallet::shielded
