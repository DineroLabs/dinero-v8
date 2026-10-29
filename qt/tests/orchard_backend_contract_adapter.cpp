#include "orchard_backend_contract_adapter.h"
#include "../src/orchardcontract.h"
#include <QJsonDocument>
#include <QJsonParseError>
#include <stdexcept>
namespace OrchardBackendContractTest {
namespace {
std::string Wire(const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString(); }
Call Make(const QString& method,const QJsonObject& params) {return {method.toStdString(),Wire(params)};}
QJsonValue Number(quint64 n) {return QJsonValue(qint64(n));}
}
Call Account(const std::string& method,uint64_t account) {
    return Make(QString::fromStdString(method),OrchardContract::AccountParams(account));
}
Call Received(uint64_t account,uint64_t offset,uint64_t revision) {
    return Make(OrchardContract::kListReceived,OrchardContract::ReceivedParams(account,offset,revision));
}
Call Payment(bool shield,bool withdraw,uint64_t account,uint64_t revision,
             const std::string& id,const std::string& address,uint64_t amount,uint64_t fee) {
    OrchardContract::Request r;r.kind=shield?OrchardContract::Request::Kind::Shield:OrchardContract::Request::Kind::Spend;
    r.account=account;r.expectedRevision=revision;r.requestId=QString::fromStdString(id);r.feeUna=fee;
    OrchardContract::Recipient recipient{QString::fromStdString(address),amount,withdraw?QByteArray{}:QByteArray::fromHex("0100")};
    (withdraw?r.outputs:r.payments).push_back(recipient);
    return Make(OrchardContract::QueueMethod(r),OrchardContract::QueueParams(r));
}
Call Finish(bool shield,uint64_t account,const std::string& id) {
    OrchardContract::Request r;r.kind=shield?OrchardContract::Request::Kind::Shield:OrchardContract::Request::Kind::Spend;
    r.account=account;r.requestId=QString::fromStdString(id);
    return Make(OrchardContract::FinishMethod(r),OrchardContract::FinishParams(r));
}
std::string Parse(const std::string& kind,const std::string& wire,const std::string& context) {
    QJsonParseError error;const auto doc=QJsonDocument::fromJson(QByteArray::fromStdString(wire),&error);
    if(error.error!=QJsonParseError::NoError || !doc.isObject())throw std::runtime_error("invalid node JSON in Qt contract adapter");
    const QJsonValue v(doc.object());QJsonObject out{{"accepted",false}};
    if(kind=="binding") {const auto p=OrchardContract::ParseBinding(v,QString::fromStdString(context));if(p)out={{"accepted",true},{"binding",*p}};}
    else if(kind=="activation") {const auto p=OrchardContract::ParseActivation(v,QString::fromStdString(context));if(p)out={{"accepted",true},{"state",p->state},{"active",p->activeAtTip},{"next_active",p->activeForNext}};}
    else if(kind=="accounts") {const auto p=OrchardContract::ParseAccounts(v);if(p){QJsonArray ids;for(const auto& a:p->accounts)ids.append(Number(a.account));out={{"accepted",true},{"ids",ids},{"sequence",Number(p->sourceSequence)}};}}
    else if(kind=="balance") {const auto p=OrchardContract::ParseBalance(v);if(p)out={{"accepted",true},{"account",Number(p->account)},{"confirmed",Number(p->confirmed)},{"reserved",Number(p->reserved)},{"unreserved",Number(p->unreserved)},{"caught_up",p->caughtUp}};}
    else if(kind=="received") {const auto p=OrchardContract::ParseReceived(v);if(p){QJsonArray rows;for(const auto& n:p->notes)rows.append(QJsonObject{{"txid",n.txid},{"scope",n.scope},{"amount",Number(n.amount)},{"memo",n.memoHex}});out={{"accepted",true},{"revision",Number(p->revision)},{"account",Number(p->account)},{"total",Number(p->total)},{"offset",Number(p->offset)},{"caught_up",p->caughtUp},{"rows",rows}};}}
    else if(kind=="operations") {const auto p=OrchardContract::ParseOperations(v);if(p){QJsonArray rows;for(const auto& n:p->operations)rows.append(QJsonObject{{"id",n.operationId},{"state",n.durableState},{"txid",n.txid},{"outcome",n.outcome},{"completion",n.completionMethod}});out={{"accepted",true},{"account",Number(p->account)},{"syncing",p->syncing()},{"rows",rows}};}}
    else if(kind=="queue") {const auto p=OrchardContract::ParseQueue(v);if(p)out={{"accepted",true},{"id",p->operationId},{"account",Number(p->account)},{"revision",Number(p->accountRevision)},{"state",p->durableState},{"existing",p->existingRequest},{"queued",p->proofQueued}};}
    else if(kind=="finish") {const auto p=OrchardContract::ParseFinish(v);if(p)out={{"accepted",true},{"id",p->operationId},{"account",Number(p->account)},{"txid",p->txid},{"admitted",p->admitted},{"already",p->alreadyInMempool},{"code",p->submissionCode}};}
    else throw std::runtime_error("unknown Qt contract parser");
    return Wire(out);
}
}
