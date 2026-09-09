#include "wallet/shielded_hardware_wallet.h"

#include "wallet/shielded_outgoing_view.h"

#include <openssl/crypto.h>

#include <algorithm>

namespace dinero::wallet::shielded {

HardwareOutgoingContextResult PrepareHardwareOutgoingViewContext(
    hw::IHardwareWallet &device, uint32_t account, uint32_t current_tip_height,
    uint32_t spend_auth_activation_height,
    uint32_t outgoing_activation_height) {
  HardwareOutgoingContextResult result;
  if (!IsOutgoingActivationPolicyValid(spend_auth_activation_height,
                                       outgoing_activation_height)) {
    result.verdict = HardwareOutgoingContextVerdict::InvalidActivationPolicy;
    result.error = "outgoing recovery cannot activate before spend authority";
    return result;
  }
  if (!IsOutgoingRuleActive(current_tip_height, spend_auth_activation_height) ||
      !IsOutgoingRuleActive(current_tip_height, outgoing_activation_height)) {
    result.verdict = HardwareOutgoingContextVerdict::RuleDormant;
    return result;
  }
  if (!device.IsConnected()) {
    result.verdict = HardwareOutgoingContextVerdict::NotConnected;
    result.error = "hardware wallet is not connected";
    return result;
  }
  const auto info = device.GetDeviceInfo();
  if (!info.success) {
    result.verdict = HardwareOutgoingContextVerdict::DeviceError;
    result.error = info.error_message;
    return result;
  }
  if (!info.value.capabilities.supports_shielded_outgoing_view) {
    result.verdict = HardwareOutgoingContextVerdict::CapabilityMissing;
    result.error = "device firmware lacks shielded outgoing-view support";
    return result;
  }
  auto key = device.GetShieldedOutgoingViewingKey(account);
  if (!key.success) {
    result.verdict = HardwareOutgoingContextVerdict::DeviceError;
    result.error = key.error_message;
    return result;
  }
  if (std::all_of(key.value.begin(), key.value.end(),
                  [](uint8_t byte) { return byte == 0; })) {
    result.verdict = HardwareOutgoingContextVerdict::InvalidViewingKey;
    result.error = "device returned an invalid all-zero outgoing viewing key";
    return result;
  }

  result.context.ovk = key.value;
  result.context.current_tip_height = current_tip_height;
  result.context.spend_auth_activation_height = spend_auth_activation_height;
  result.context.outgoing_activation_height = outgoing_activation_height;
  OPENSSL_cleanse(key.value.data(), key.value.size());
  result.verdict = HardwareOutgoingContextVerdict::Ok;
  return result;
}

const char *
HardwareOutgoingContextVerdictName(HardwareOutgoingContextVerdict verdict) {
  switch (verdict) {
  case HardwareOutgoingContextVerdict::Ok:
    return "ok";
  case HardwareOutgoingContextVerdict::RuleDormant:
    return "rule_dormant";
  case HardwareOutgoingContextVerdict::InvalidActivationPolicy:
    return "invalid_activation_policy";
  case HardwareOutgoingContextVerdict::NotConnected:
    return "not_connected";
  case HardwareOutgoingContextVerdict::CapabilityMissing:
    return "capability_missing";
  case HardwareOutgoingContextVerdict::DeviceError:
    return "device_error";
  case HardwareOutgoingContextVerdict::InvalidViewingKey:
    return "invalid_viewing_key";
  }
  return "unknown";
}

} // namespace dinero::wallet::shielded
