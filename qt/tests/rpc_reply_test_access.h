#pragma once
#include "rpcclient.h"
// Only exercises reply parsing/signals. No connection initialization or I/O.
struct RpcReplyTestAccess {
    static RpcClient* Make(QObject* parent=nullptr) { return new RpcClient(parent,RpcClient::ReplyOnlyTestTag{}); }
    static bool Deliver(RpcClient& client,const QString& tag,const QByteArray& body) { return client.deliverRpcResponse(tag,body); }
};
