#include <gtest/gtest.h>

#include "wallet/shielded_hardware_wallet.h"
#include "wallet/shielded_outgoing_view.h"
#include "wallet/shielded_wallet_ops.h"

#include <json/json.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace out = dinero::wallet::shielded;
namespace ops = dinero::wallet::shielded_ops;
namespace hw = dinero::hw;
namespace sh = dinero::consensus::shielded;

class FakeShieldedHardwareWallet final : public hw::IHardwareWallet {
public:
  bool connected = false;
  bool capability = false;
  bool info_success = true;
  bool key_success = true;
  int key_requests = 0;
  uint32_t last_account = UINT32_MAX;
  std::array<uint8_t, 32> key{};

  hw::HWResult<std::vector<hw::DeviceInfo>> EnumerateDevices() override {
    return hw::HWResult<std::vector<hw::DeviceInfo>>::Ok({});
  }
  hw::HWResult<bool> Connect(const std::string &) override {
    connected = true;
    return hw::HWResult<bool>::Ok(true);
  }
  hw::HWResult<bool> Disconnect() override {
    connected = false;
    return hw::HWResult<bool>::Ok(true);
  }
  hw::HWResult<hw::DeviceInfo> GetDeviceInfo() override {
    if (!info_success) {
      return hw::HWResult<hw::DeviceInfo>::Err("device inspection failed");
    }
    hw::DeviceInfo info;
    info.capabilities.supports_shielded_outgoing_view = capability;
    return hw::HWResult<hw::DeviceInfo>::Ok(info);
  }
  bool IsConnected() const override { return connected; }
  hw::HWResult<dinero::PSBT> SignPSBT(const dinero::PSBT &,
                                      const std::vector<std::string> &,
                                      hw::ProgressCallback) override {
    return hw::HWResult<dinero::PSBT>::Err("unused");
  }
  hw::HWResult<bool> DisplayAddress(const std::string &,
                                    const std::string &) override {
    return hw::HWResult<bool>::Err("unused");
  }
  hw::HWResult<std::string> GetPublicKey(const std::string &) override {
    return hw::HWResult<std::string>::Err("unused");
  }
  hw::HWResult<uint32_t> GetMasterFingerprint() override {
    return hw::HWResult<uint32_t>::Err("unused");
  }
  hw::HWResult<std::array<uint8_t, 32>>
  GetShieldedOutgoingViewingKey(uint32_t account) override {
    ++key_requests;
    last_account = account;
    if (!key_success) {
      return hw::HWResult<std::array<uint8_t, 32>>::Err("device refused");
    }
    return hw::HWResult<std::array<uint8_t, 32>>::Ok(key);
  }
};

#ifndef SHIELDED_OUTGOING_VECTOR_FILE
#error "SHIELDED_OUTGOING_VECTOR_FILE must name the independent JSON fixture"
#endif

std::vector<uint8_t> FromHex(const std::string &text) {
  if ((text.size() & 1U) != 0)
    throw std::runtime_error("odd hex length");
  auto nibble = [](char c) -> uint8_t {
    if (c >= '0' && c <= '9')
      return static_cast<uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f')
      return static_cast<uint8_t>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F')
      return static_cast<uint8_t>(c - 'A' + 10);
    throw std::runtime_error("invalid hex");
  };
  std::vector<uint8_t> bytes(text.size() / 2);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<uint8_t>((nibble(text[2 * i]) << 4) |
                                    nibble(text[2 * i + 1]));
  }
  return bytes;
}

template <std::size_t N>
std::array<uint8_t, N> ArrayFromHex(const std::string &text) {
  const auto bytes = FromHex(text);
  if (bytes.size() != N)
    throw std::runtime_error("wrong fixed hex length");
  std::array<uint8_t, N> result{};
  std::copy(bytes.begin(), bytes.end(), result.begin());
  return result;
}

const Json::Value &Vectors() {
  static const Json::Value root = [] {
    std::ifstream input(SHIELDED_OUTGOING_VECTOR_FILE);
    if (!input)
      throw std::runtime_error("cannot open outgoing-view vectors");
    Json::CharReaderBuilder builder;
    Json::Value parsed;
    std::string errors;
    if (!Json::parseFromStream(builder, input, &parsed, &errors)) {
      throw std::runtime_error("cannot parse outgoing-view vectors: " + errors);
    }
    return parsed;
  }();
  return root;
}

const Json::Value &LegacyVectors() {
  static const Json::Value root = [] {
    std::ifstream input(SHIELDED_OUTGOING_LEGACY_VECTOR_FILE);
    if (!input)
      throw std::runtime_error("cannot open legacy outgoing-view vectors");
    Json::CharReaderBuilder builder;
    Json::Value parsed;
    std::string errors;
    if (!Json::parseFromStream(builder, input, &parsed, &errors)) {
      throw std::runtime_error("cannot parse legacy outgoing-view vectors: " +
                               errors);
    }
    return parsed;
  }();
  return root;
}

out::OutgoingPublicOutput PublicOutput() {
  out::OutgoingPublicOutput result;
  result.commitment = ArrayFromHex<32>(
      Vectors()["source"]["authenticated_note_commitment"].asString());
  result.value_commitment =
      ArrayFromHex<33>(Vectors()["source"]["value_commitment"].asString());
  return result;
}

out::OutgoingConstructionInput ConstructionInput() {
  out::OutgoingConstructionInput input;
  input.ovk = ArrayFromHex<32>(Vectors()["source"]["ovk"].asString());
  input.public_output = PublicOutput();
  input.recipient_encrypted_note = ArrayFromHex<out::kEncryptedNoteBytes>(
      Vectors()["source"]["recipient_encrypted_note_v1"].asString());
  input.pk_d_enc = ArrayFromHex<32>(Vectors()["source"]["pk_d_enc"].asString());
  input.pk_d_spend =
      ArrayFromHex<32>(Vectors()["source"]["pk_d_spend"].asString());
  input.nfk_commitment =
      ArrayFromHex<32>(Vectors()["source"]["nfk_commitment"].asString());
  input.esk_normalized =
      ArrayFromHex<32>(Vectors()["source"]["esk_normalized"].asString());
  return input;
}

out::OutgoingRecoveryResult
Recover(const std::vector<uint8_t> &envelope,
        out::OutgoingPublicOutput public_output = PublicOutput()) {
  return out::RecoverOutgoingNote(
      ConstructionInput().ovk, public_output, envelope,
      /*output_height=*/100, /*spend_auth=*/100, /*outgoing=*/100);
}

void ExpectVerdict(const std::string &fixture,
                   out::OutgoingRecoveryVerdict expected) {
  const auto bytes =
      FromHex(Vectors()["mutation_fixtures"][fixture].asString());
  EXPECT_EQ(Recover(bytes).verdict, expected) << fixture;
  EXPECT_EQ(out::OutgoingRecoveryVerdictName(expected),
            Vectors()["failure_classes"][fixture].asString())
      << fixture;
}

void ExpectNoRecoveredSecrets(const out::OutgoingRecoveryResult &result) {
  const auto all_zero = [](const auto &bytes) {
    return std::all_of(bytes.begin(), bytes.end(),
                       [](uint8_t byte) { return byte == 0; });
  };
  EXPECT_EQ(result.note.value_una, 0u);
  EXPECT_TRUE(all_zero(result.note.rcm));
  EXPECT_TRUE(all_zero(result.note.memo));
  EXPECT_TRUE(all_zero(result.note.esk_normalized));
  EXPECT_TRUE(all_zero(result.note.recipient_address_payload));
}

TEST(ShieldedOutgoingView, CanonicalConstructionMatchesIndependentOracle) {
  const auto built = out::BuildOutgoingEnvelope(ConstructionInput());
  ASSERT_EQ(built.verdict, out::OutgoingRecoveryVerdict::Ok);
  EXPECT_EQ(built.envelope,
            FromHex(Vectors()["outgoing"]["envelope_v3"].asString()));
}

TEST(ShieldedOutgoingView, CanonicalRecoveryMatchesIndependentOracle) {
  const auto recovered =
      Recover(FromHex(Vectors()["outgoing"]["envelope_v3"].asString()));
  ASSERT_EQ(recovered.verdict, out::OutgoingRecoveryVerdict::Ok);
  const auto &expected = Vectors()["recovery"];
  EXPECT_EQ(recovered.note.d, ArrayFromHex<11>(expected["d"].asString()));
  EXPECT_EQ(recovered.note.value_una, expected["value"].asUInt64());
  EXPECT_EQ(recovered.note.rcm, ArrayFromHex<32>(expected["rcm"].asString()));
  EXPECT_EQ(recovered.note.memo,
            ArrayFromHex<512>(expected["memo"].asString()));
  EXPECT_EQ(recovered.note.pk_d_enc,
            ArrayFromHex<32>(expected["pk_d_enc"].asString()));
  EXPECT_EQ(recovered.note.pk_d_spend,
            ArrayFromHex<32>(expected["pk_d_spend"].asString()));
  EXPECT_EQ(recovered.note.nfk_commitment,
            ArrayFromHex<32>(expected["nfk_commitment"].asString()));
  EXPECT_EQ(recovered.note.esk_normalized,
            ArrayFromHex<32>(expected["esk_normalized"].asString()));
  EXPECT_EQ(recovered.note.recipient_address_payload,
            ArrayFromHex<107>(expected["recipient_address_payload"].asString()));
}

TEST(ShieldedOutgoingView, ActivationPolicyIsIndependentAndSentinelAware) {
  EXPECT_FALSE(out::IsOutgoingRuleActive(UINT32_MAX, UINT32_MAX));
  EXPECT_FALSE(out::IsOutgoingRuleActive(99, 100));
  EXPECT_TRUE(out::IsOutgoingRuleActive(100, 100));
  EXPECT_TRUE(out::IsOutgoingActivationPolicyValid(UINT32_MAX, UINT32_MAX));
  EXPECT_TRUE(out::IsOutgoingActivationPolicyValid(100, UINT32_MAX));
  EXPECT_TRUE(out::IsOutgoingActivationPolicyValid(100, 100));
  EXPECT_FALSE(out::IsOutgoingActivationPolicyValid(UINT32_MAX, 100));
  EXPECT_FALSE(out::IsOutgoingActivationPolicyValid(101, 100));

  const auto envelope =
      FromHex(Vectors()["outgoing"]["envelope_v3"].asString());
  EXPECT_EQ(out::RecoverOutgoingNote(ConstructionInput().ovk, PublicOutput(),
                                     envelope, 100, 101, 101)
                .verdict,
            out::OutgoingRecoveryVerdict::SpendAuthorityRequired);
  EXPECT_EQ(out::RecoverOutgoingNote(ConstructionInput().ovk, PublicOutput(),
                                     envelope, 100, 100, 101)
                .verdict,
            out::OutgoingRecoveryVerdict::OutgoingRecoveryInactive);
  EXPECT_EQ(out::RecoverOutgoingNote(ConstructionInput().ovk, PublicOutput(),
                                     envelope, UINT32_MAX, UINT32_MAX,
                                     UINT32_MAX)
                .verdict,
            out::OutgoingRecoveryVerdict::SpendAuthorityRequired);
}

TEST(ShieldedOutgoingView, CanonicalParserPreservesEveryFailureClass) {
  using V = out::OutgoingRecoveryVerdict;
  const auto legacy =
      FromHex(Vectors()["source"]["recipient_encrypted_note_v1"].asString());
  EXPECT_EQ(Recover(legacy).verdict, V::LegacyNoOutgoingRecovery);
  EXPECT_EQ(out::OutgoingRecoveryVerdictName(V::LegacyNoOutgoingRecovery),
            Vectors()["failure_classes"]["legacy_v1"].asString());

  ExpectVerdict("truncated_v3", V::InvalidLength);
  ExpectVerdict("trailing_v3", V::InvalidLength);
  ExpectVerdict("unknown_envelope_version", V::UnsupportedEnvelopeVersion);
  ExpectVerdict("tampered_outgoing_tag", V::NotForViewerOrTampered);
  ExpectVerdict("unknown_plaintext_version", V::UnsupportedPlaintextVersion);
  ExpectVerdict("invalid_encryption_key", V::InvalidEncryptionKey);
  ExpectVerdict("invalid_spend_key", V::InvalidSpendKey);
  ExpectVerdict("invalid_nullifier_key_commitment",
                V::InvalidNullifierKeyCommitment);
  ExpectVerdict("noncanonical_nullifier_key_commitment",
                V::InvalidNullifierKeyCommitment);
  ExpectVerdict("zero_ephemeral_secret", V::InvalidEphemeralSecret);
  ExpectVerdict("curve_order_ephemeral_secret", V::InvalidEphemeralSecret);
  ExpectVerdict("ephemeral_key_mismatch", V::EphemeralKeyMismatch);
  ExpectVerdict("noncanonical_ephemeral_secret", V::EphemeralKeyMismatch);
  ExpectVerdict("recipient_authentication_failure",
                V::RecipientAuthenticationFailed);
  ExpectVerdict("recipient_ciphertext_transplant", V::NotForViewerOrTampered);

  auto wrong_ovk = ConstructionInput().ovk;
  wrong_ovk[0] ^= 1;
  const auto envelope =
      FromHex(Vectors()["outgoing"]["envelope_v3"].asString());
  EXPECT_EQ(out::RecoverOutgoingNote(wrong_ovk, PublicOutput(), envelope, 100,
                                     100, 100)
                .verdict,
            V::NotForViewerOrTampered);

  auto wrong_commitment = PublicOutput();
  wrong_commitment.commitment[0] ^= 1;
  EXPECT_EQ(Recover(envelope, wrong_commitment).verdict,
            V::NotForViewerOrTampered);
  auto wrong_cv = PublicOutput();
  wrong_cv.value_commitment[1] ^= 1;
  EXPECT_EQ(Recover(envelope, wrong_cv).verdict, V::NotForViewerOrTampered);

  auto mismatch_public = PublicOutput();
  mismatch_public.commitment = ArrayFromHex<32>(
      Vectors()["mutation_fixtures"]["commitment_mismatch_public"].asString());
  const auto mismatch =
      FromHex(Vectors()["mutation_fixtures"]["commitment_mismatch_envelope"]
                  .asString());
  const auto mismatch_result = Recover(mismatch, mismatch_public);
  EXPECT_EQ(mismatch_result.verdict, V::CommitmentMismatch);
  ExpectNoRecoveredSecrets(mismatch_result);

  const auto recipient_failure = Recover(
      FromHex(Vectors()["mutation_fixtures"]["recipient_authentication_failure"]
                  .asString()));
  EXPECT_EQ(recipient_failure.verdict, V::RecipientAuthenticationFailed);
  ExpectNoRecoveredSecrets(recipient_failure);
}

TEST(ShieldedOutgoingView, DormantUnsafeV2EnvelopeIsNeverReinterpreted) {
  const auto legacy_v2 =
      FromHex(LegacyVectors()["outgoing"]["envelope_v2"].asString());
  ASSERT_EQ(legacy_v2.size(), 725u);
  EXPECT_EQ(Recover(legacy_v2).verdict,
            out::OutgoingRecoveryVerdict::InvalidLength)
      << "the unpublished ivk-derived v2 format must remain permanently dead";
}

TEST(ShieldedOutgoingView, ConstructionRejectsInvalidAuthorityMaterial) {
  using V = out::OutgoingRecoveryVerdict;
  auto input = ConstructionInput();
  input.pk_d_enc.fill(0xff);
  EXPECT_EQ(out::BuildOutgoingEnvelope(input).verdict, V::InvalidEncryptionKey);
  input = ConstructionInput();
  input.pk_d_spend.fill(0xff);
  EXPECT_EQ(out::BuildOutgoingEnvelope(input).verdict, V::InvalidSpendKey);
  input = ConstructionInput();
  input.nfk_commitment.fill(0);
  EXPECT_EQ(out::BuildOutgoingEnvelope(input).verdict,
            V::InvalidNullifierKeyCommitment);
  input = ConstructionInput();
  input.nfk_commitment = ArrayFromHex<32>(
      "fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141");
  EXPECT_EQ(out::BuildOutgoingEnvelope(input).verdict,
            V::InvalidNullifierKeyCommitment);
  input = ConstructionInput();
  input.esk_normalized.fill(0);
  EXPECT_EQ(out::BuildOutgoingEnvelope(input).verdict,
            V::InvalidEphemeralSecret);
}

TEST(ShieldedOutgoingView, ConstructionRefusesUnrecoverableValidInputs) {
  using V = out::OutgoingRecoveryVerdict;

  auto input = ConstructionInput();
  input.esk_normalized.fill(0);
  input.esk_normalized[31] = 1;  // canonical, deliberately stale
  EXPECT_EQ(out::BuildOutgoingEnvelope(input).verdict,
            V::RecipientAuthenticationFailed)
      << "a valid-looking but stale esk must not create unrecoverable history";

  input = ConstructionInput();
  input.pk_d_spend = input.pk_d_enc;  // valid point, wrong authority
  EXPECT_EQ(out::BuildOutgoingEnvelope(input).verdict, V::CommitmentMismatch);

  input = ConstructionInput();
  input.nfk_commitment.fill(0);
  input.nfk_commitment[31] = 1;  // canonical, deliberately wrong
  EXPECT_EQ(out::BuildOutgoingEnvelope(input).verdict, V::CommitmentMismatch);
}

TEST(ShieldedOutgoingView, RecoveredRcmDoesNotRestoreRecipientSpendAuthority) {
  const auto recovered =
      Recover(FromHex(Vectors()["outgoing"]["envelope_v3"].asString()));
  ASSERT_EQ(recovered.verdict, out::OutgoingRecoveryVerdict::Ok);

  const auto legacy_secret = out::DeriveNoteSpendKey(recovered.note.rcm);
  const auto recipient_secret = ArrayFromHex<32>(
      Vectors()["security_boundary"]["recipient_spend_scalar"].asString());
  EXPECT_NE(legacy_secret, recipient_secret);
  EXPECT_EQ(legacy_secret,
            ArrayFromHex<32>(
                Vectors()["security_boundary"]["legacy_rcm_derived_spend_key"]
                    .asString()));

  sh::Hash d_packed{};
  std::copy(recovered.note.d.begin(), recovered.note.d.end(), d_packed.begin());
  sh::Hash value{};
  for (int i = 0; i < 8; ++i) {
    value[31 - i] =
        static_cast<uint8_t>((recovered.note.value_una >> (8 * i)) & 0xffU);
  }
  const auto legacy_public = sh::PoseidonHash2(legacy_secret, sh::Hash{});
  const auto legacy_commitment =
      sh::NoteCommitment(d_packed, legacy_public, value, recovered.note.rcm);
  EXPECT_EQ(
      legacy_commitment,
      ArrayFromHex<32>(
          Vectors()["security_boundary"]["legacy_note_commitment"].asString()));
  EXPECT_NE(legacy_commitment, PublicOutput().commitment)
      << "recovering rcm must not recreate the recipient-bound commitment";
}

TEST(ShieldedOutgoingView,
     HardwareWalletBoundaryIsCapabilityGatedAndFailClosed) {
  using V = out::HardwareOutgoingContextVerdict;
  FakeShieldedHardwareWallet device;

  // Dormancy is decided before touching the device. Existing hardware keeps
  // working while this feature is inactive and receives no surprise prompt.
  auto result =
      out::PrepareHardwareOutgoingViewContext(device, 2, 99, 100, 100);
  EXPECT_EQ(result.verdict, V::RuleDormant);
  EXPECT_EQ(device.key_requests, 0);

  result = out::PrepareHardwareOutgoingViewContext(device, 2, 100, 100, 99);
  EXPECT_EQ(result.verdict, V::InvalidActivationPolicy);
  EXPECT_EQ(device.key_requests, 0);

  result = out::PrepareHardwareOutgoingViewContext(device, 2, 100, 100, 100);
  EXPECT_EQ(result.verdict, V::NotConnected);
  EXPECT_EQ(device.key_requests, 0);

  device.connected = true;
  device.info_success = false;
  result = out::PrepareHardwareOutgoingViewContext(device, 2, 100, 100, 100);
  EXPECT_EQ(result.verdict, V::DeviceError);
  EXPECT_EQ(device.key_requests, 0);

  device.info_success = true;
  result = out::PrepareHardwareOutgoingViewContext(device, 2, 100, 100, 100);
  EXPECT_EQ(result.verdict, V::CapabilityMissing);
  EXPECT_EQ(device.key_requests, 0);

  device.capability = true;
  device.key_success = false;
  result = out::PrepareHardwareOutgoingViewContext(device, 2, 100, 100, 100);
  EXPECT_EQ(result.verdict, V::DeviceError);
  EXPECT_EQ(device.key_requests, 1);

  device.key_success = true;
  device.key.fill(0);
  result = out::PrepareHardwareOutgoingViewContext(device, 2, 100, 100, 100);
  EXPECT_EQ(result.verdict, V::InvalidViewingKey);

  device.key.fill(0x42);
  result = out::PrepareHardwareOutgoingViewContext(device, 2, 100, 100, 100);
  ASSERT_EQ(result.verdict, V::Ok) << result.error;
  EXPECT_EQ(result.context.ovk, device.key);
  EXPECT_EQ(result.context.current_tip_height, 100u);
  EXPECT_EQ(result.context.spend_auth_activation_height, 100u);
  EXPECT_EQ(result.context.outgoing_activation_height, 100u);
  EXPECT_EQ(device.last_account, 2u);
}

TEST(ShieldedOutgoingView, AddressedOutputEmitsV3OnlyAtCurrentTipActivation) {
  ops::AddressedRecipient recipient;
  recipient.d = ArrayFromHex<11>(Vectors()["source"]["d"].asString());
  recipient.pk_d = ArrayFromHex<32>(Vectors()["source"]["pk_d_enc"].asString());
  recipient.pk_d_spend =
      ArrayFromHex<32>(Vectors()["source"]["pk_d_spend"].asString());
  recipient.nfk_commitment =
      ArrayFromHex<32>(Vectors()["source"]["nfk_commitment"].asString());
  recipient.value_una = Vectors()["source"]["value"].asUInt64();
  const auto rcm = ArrayFromHex<32>(Vectors()["source"]["rcm"].asString());
  const auto esk =
      ArrayFromHex<32>(Vectors()["source"]["esk_normalized"].asString());

  ops::OutgoingViewEmissionContext context;
  context.ovk = ArrayFromHex<32>(Vectors()["source"]["ovk"].asString());
  context.current_tip_height = 100;
  context.spend_auth_activation_height = 100;
  context.outgoing_activation_height = 100;

  auto active = ops::BuildAddressedRecipientOutput(
      recipient, nullptr, /*cv_bound=*/true, /*spend_auth=*/true, &rcm, &esk,
      &context);
  ASSERT_EQ(active.status, ops::OpStatus::Ok) << active.error;
  ASSERT_EQ(active.planned.encrypted_note.size(), out::kOutgoingEnvelopeBytes);
  out::OutgoingPublicOutput public_output;
  public_output.commitment = active.commitment;
  public_output.value_commitment = active.value_commitment;
  auto recovered = out::RecoverOutgoingNote(
      context.ovk, public_output, active.planned.encrypted_note,
      /*output_height=*/100, /*spend_auth=*/100, /*outgoing=*/100);
  ASSERT_EQ(recovered.verdict, out::OutgoingRecoveryVerdict::Ok);
  EXPECT_EQ(recovered.note.value_una, recipient.value_una);
  EXPECT_EQ(recovered.note.rcm, rcm);
  EXPECT_EQ(recovered.note.pk_d_enc, recipient.pk_d);
  EXPECT_EQ(recovered.note.pk_d_spend, recipient.pk_d_spend);
  EXPECT_EQ(recovered.note.nfk_commitment, recipient.nfk_commitment);

  context.current_tip_height = 99;
  auto before = ops::BuildAddressedRecipientOutput(
      recipient, nullptr, /*cv_bound=*/true, /*spend_auth=*/true, &rcm, &esk,
      &context);
  ASSERT_EQ(before.status, ops::OpStatus::Ok) << before.error;
  EXPECT_EQ(before.planned.encrypted_note.size(), out::kEncryptedNoteBytes);

  context.current_tip_height = UINT32_MAX;
  context.spend_auth_activation_height = UINT32_MAX;
  context.outgoing_activation_height = UINT32_MAX;
  auto dormant = ops::BuildAddressedRecipientOutput(
      recipient, nullptr, /*cv_bound=*/true, /*spend_auth=*/true, &rcm, &esk,
      &context);
  ASSERT_EQ(dormant.status, ops::OpStatus::Ok) << dormant.error;
  EXPECT_EQ(dormant.planned.encrypted_note.size(), out::kEncryptedNoteBytes);
}

} // namespace
