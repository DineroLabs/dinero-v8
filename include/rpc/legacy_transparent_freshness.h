#pragma once

#include "din_json.h"
#include <memory>

struct ExecutionContext;
namespace dinero { class WalletService; }

// Refuse marked transparent-only wallet reads while their authenticated
// ordinary receipt is behind the selected chain. Unmarked wallets keep their
// existing RPC behavior. On refusal, result has no numeric balance fields.
bool RefuseIfLegacyTransparentTipNotCurrent(
    const ExecutionContext& ctx,
    const std::shared_ptr<dinero::WalletService>& service,
    din::Json& result);

// Marked transparent-only wallets have no Orchard catalog and cannot safely
// sign, allocate change/receive scripts, or import new script material.
bool RefuseIfLegacyTransparentSpendUnavailable(
    const ExecutionContext& ctx,
    const std::shared_ptr<dinero::WalletService>& service,
    din::Json& result);
bool RefuseIfLegacyTransparentAddressMutation(
    const ExecutionContext& ctx,
    const std::shared_ptr<dinero::WalletService>& service,
    din::Json& result);
