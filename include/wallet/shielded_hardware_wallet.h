#pragma once

#include "wallet/hardware_wallet_interface.h"
#include "wallet/shielded_wallet_ops.h"

#include <cstdint>
#include <string>

namespace dinero::wallet::shielded {

enum class HardwareOutgoingContextVerdict : uint8_t {
  Ok = 0,
  RuleDormant,
  InvalidActivationPolicy,
  NotConnected,
  CapabilityMissing,
  DeviceError,
  InvalidViewingKey,
};

struct HardwareOutgoingContextResult {
  HardwareOutgoingContextVerdict verdict =
      HardwareOutgoingContextVerdict::DeviceError;
  shielded_ops::OutgoingViewEmissionContext context{};
  std::string error;
};

/**
 * Obtain the minimal host material needed for sender recovery from a hardware
 * wallet. The device is not contacted while the rule is dormant. When active,
 * unsupported/disconnected/refusing devices fail closed; the host never falls
 * back to a software seed or asks for shielded spend authority.
 */
HardwareOutgoingContextResult PrepareHardwareOutgoingViewContext(
    hw::IHardwareWallet &device, uint32_t account, uint32_t current_tip_height,
    uint32_t spend_auth_activation_height, uint32_t outgoing_activation_height);

const char *
HardwareOutgoingContextVerdictName(HardwareOutgoingContextVerdict verdict);

} // namespace dinero::wallet::shielded
