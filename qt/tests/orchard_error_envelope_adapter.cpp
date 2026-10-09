#include "orchard_error_envelope_adapter.h"
#include "rpc_reply_test_access.h"
#include <QJsonDocument>
#include <memory>
namespace OrchardErrorEnvelopeTest {
std::string Decode(const std::string& wire) {
    std::unique_ptr<RpcClient> client(RpcReplyTestAccess::Make());
    int detailCount=0,legacyCount=0,resultCount=0;QJsonObject out;
    QObject::connect(client.get(),&RpcClient::rpcErrorDetailed,client.get(),[&](const QString& tag,int code,const QString& message,const QJsonValue& data){
        ++detailCount;out["tag"]=tag;out["code"]=code;out["message"]=message;
        if(!data.isUndefined())out["data"]=data;
    });
    QObject::connect(client.get(),&RpcClient::rpcError,client.get(),[&](const QString&,int,const QString&){++legacyCount;});
    QObject::connect(client.get(),&RpcClient::rpcResult,client.get(),[&](const QString&,const QJsonValue& result){++resultCount;out["result"]=result;});
    out["success"]=RpcReplyTestAccess::Deliver(*client,"actual-request-tag",QByteArray::fromStdString(wire));
    out["details"]=detailCount;out["legacy"]=legacyCount;out["results"]=resultCount;
    return QJsonDocument(out).toJson(QJsonDocument::Compact).toStdString();
}
}
