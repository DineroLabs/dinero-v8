#pragma once
#include "rpc/rpc_registry.h"
din::Json rpc_context_wallet_orchard_getnewaddress(const ExecutionContext&,const din::Json&);
din::Json rpc_context_wallet_orchard_createaccount(const ExecutionContext&,const din::Json&);
void RegisterOrchardAccountRpc();
din::Json rpc_context_wallet_orchard_listoperations(const ExecutionContext&,const din::Json&);
din::Json rpc_context_wallet_orchard_listaccounts(const ExecutionContext&,const din::Json&);
din::Json rpc_context_wallet_orchard_queuespend(const ExecutionContext&,const din::Json&);
din::Json rpc_context_wallet_orchard_finishspend(const ExecutionContext&,const din::Json&);
din::Json rpc_context_wallet_orchard_queueshield(const ExecutionContext&,const din::Json&);
din::Json rpc_context_wallet_orchard_finishshield(const ExecutionContext&,const din::Json&);
din::Json rpc_context_wallet_orchard_getbalance(const ExecutionContext&,const din::Json&);

// Registry bootstrap: explicit intended name, no wallet switching or enrollment.
din::Json rpc_context_wallet_orchard_getwalletbinding(const ExecutionContext&,const din::Json&);

// Read-only chain rule observation; does not select or authorize a wallet.
din::Json rpc_context_orchard_getactivationstatus(const ExecutionContext&,const din::Json&);

// Explicitly paginated selected-checkpoint receipts, including spent notes/change.
din::Json rpc_context_wallet_orchard_listreceived(const ExecutionContext&,const din::Json&);
