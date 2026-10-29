// Cache dependencies first, as in a TU that includes Windows headers later.
#include "daemon/p2p_message.h"
#include "p2p/connection_manager.h"
#include "p2p/rate_limiter.h"
#include "p2p/peer_scoring.h"

#define ERROR 0
#include "consensus/cpu_budget_monitor.h"
#ifdef ERROR
#error CPU budget header did not clear the Windows ERROR macro
#endif

#define ERROR 0
#include "p2p/network_limits_monitor.h"
#ifdef ERROR
#error Network monitor header did not clear the Windows ERROR macro
#endif

static_assert(static_cast<int>(dinero::consensus::CPUBudgetStatus::ERROR) == 4);
static_assert(static_cast<int>(dinero::p2p::NetworkHealthStatus::ERROR) == 4);
int main() { return 0; }
