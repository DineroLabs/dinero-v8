#pragma once
#include "rpc/rpc_registry.h"
din::Json rpc_context_wallet_orchard_getnewaddress(const ExecutionContext&,const din::Json&);
din::Json rpc_context_wallet_orchard_createaccount(const ExecutionContext&,const din::Json&);
void RegisterOrchardAccountRpc();
din::Json rpc_context_wallet_orchard_listoperations(const ExecutionContext&,const din::Json&);
