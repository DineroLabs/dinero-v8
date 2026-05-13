// Copyright (c) 2026 Dinero Labs.
//
// Liquidity Vault RPC method declarations.
//
// Six methods make up the daemon-side vault RPC surface (design doc §C.3):
//   vault.account.spendable    — single account spendable balance
//   vault.account.metrics      — full per-account snapshot
//   vault.observe              — explicit deposit registration (rare)
//   vault.withdraw             — enqueue a withdrawal
//   vault.withdrawal.status    — query withdrawal lifecycle
//   vault.metrics              — global vault metrics

#pragma once

#include "din_json.h"
#include "rpc/rpc_registry.h"

#include <memory>

namespace dinero::vault {
class VaultService;
}

namespace din {

dinero::vault::VaultService* GetVaultService();
void SetVaultService(dinero::vault::VaultService* service);

Json rpc_vault_account_spendable(const ExecutionContext& ctx, const Json& params);
Json rpc_vault_account_metrics(const ExecutionContext& ctx, const Json& params);
Json rpc_vault_observe(const ExecutionContext& ctx, const Json& params);
Json rpc_vault_withdraw(const ExecutionContext& ctx, const Json& params);
Json rpc_vault_withdrawal_status(const ExecutionContext& ctx, const Json& params);
Json rpc_vault_metrics(const ExecutionContext& ctx, const Json& params);

}  // namespace din

void RegisterVaultRPC();
