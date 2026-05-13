#include "daemon/daemon_context.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/services/wallet_service.h"
#include <QString>

// Health check helpers sourced from live daemon context.

int currentTipHeight() {
    auto* ctx = DaemonContext::instance();
    if (ctx && ctx->chainstate) {
        return static_cast<int>(ctx->chainstate->getBlockHeight());
    }
    return 0;
}

bool walletLoaded() {
    auto* ctx = DaemonContext::instance();
    return ctx && ctx->wallet && ctx->wallet->hasActiveWallet();
}

QString activeWalletName() {
    auto* ctx = DaemonContext::instance();
    if (ctx && ctx->wallet && ctx->wallet->hasActiveWallet()) {
        return QString::fromStdString(ctx->wallet->getCurrentWalletName());
    }
    return QString();
}
