#pragma once
#include <QString>
#include <QUrl>

namespace RpcConnectionPolicy {
// A local transport lifetime token is not remote-server authentication.
// Both token and endpoint must still match before a reply can affect state.
inline bool MatchesOrigin(const QString& sentContext,const QUrl& sentEndpoint,
                          const QString& currentContext,const QUrl& currentEndpoint) {
    return !sentContext.isEmpty() && sentContext==currentContext && sentEndpoint==currentEndpoint;
}
inline bool AcceptsResponse(const QString& sentContext,const QUrl& sentEndpoint,
                           const QString& currentContext,const QUrl& currentEndpoint,
                           bool explicitRetry,const QUrl& responseEndpoint,int httpStatus) {
    if(!MatchesOrigin(sentContext,sentEndpoint,currentContext,currentEndpoint))return false;
    // Bound/effect requests are never forwarded by an HTTP redirect. A lost
    // response remains ambiguous and requires wallet-owned reconciliation.
    return !explicitRetry || (responseEndpoint==sentEndpoint && !(httpStatus>=300 && httpStatus<400));
}
}  // namespace RpcConnectionPolicy
