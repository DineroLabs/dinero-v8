#include "orchard_http_transport_adapter.h"
#include "rpcclient.h"
#include "orchardwidget.h"
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QAbstractButton>
#include <functional>
#include <QPushButton>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QSet>
#include <QTableWidget>
#include <QTimer>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <unistd.h>
namespace OrchardHttpTransportTest {
namespace {
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
std::string Env(const char* n) { const auto* p=std::getenv(n); return p?p:""; }
std::string Wire(const QJsonObject& value) { return QJsonDocument(value).toJson(QJsonDocument::Compact).toStdString(); }
void EnsureApplication() {
    Require(qobject_cast<QApplication*>(QCoreApplication::instance()) != nullptr,
            "explicit test application lifetime is missing");
}
}
std::string ValidateEnvironment() {
    Require(Env("DINERO_ALLOW_ORCHARD_HTTP_TEST")=="explicit-isolated-authorization",
            "Orchard HTTP test execution is not authorized");
    Require(Env("DINERO_RPC_URL").empty(),"custom RPC endpoint is forbidden");
    Require(Env("QT_QPA_PLATFORM")=="offscreen","requires offscreen Qt");
    const auto home=std::filesystem::canonical(Env("DINERO_ORCHARD_HTTP_HOME"));
    Require(home==std::filesystem::canonical(Env("HOME")),"HOME must be isolated before process startup");
    Require(home.parent_path()==std::filesystem::canonical("/tmp") &&
            home.filename().string().starts_with("orchard-http-isolated-"),"unexpected isolated home location");
    const auto st=std::filesystem::status(home);
    Require((st.permissions() & (std::filesystem::perms::group_all|std::filesystem::perms::others_all))==std::filesystem::perms::none,
            "isolated home must have mode 0700");
    std::ifstream marker(home/"owner.marker");std::string token;std::getline(marker,token);
    Require(token=="orchard-http-transport-v2-isolated-only","isolated owner marker missing");
    Require(Env("XDG_CONFIG_HOME")== (home/"config").string() &&
            Env("XDG_CACHE_HOME")== (home/"cache").string() &&
            Env("XDG_RUNTIME_DIR")== (home/"runtime").string(),"XDG directories must be isolated");
    // No discovery, application construction or sockets have happened here.
    return home.string();
}
int RunWithApplication(int (*runTests)()) {
    ValidateEnvironment();
    Require(QCoreApplication::instance()==nullptr,"unexpected pre-existing Qt application");
    int argc=1;
    char name[]="orchard-http-transport-isolated";
    char* argv[]={name,nullptr};
    // Destroy the application before C++ global/Qt teardown, after all test
    // fixtures, clients and HTTP handlers have been destroyed.
    QApplication app(argc,argv);
    QNetworkProxyFactory::setUseSystemConfiguration(false);
    QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
    return runTests();
}
struct Client::Impl {
    std::unique_ptr<RpcClient> rpc;
    QSet<QString> pending;
    QJsonArray calls;
    // Reverse destruction order keeps callbacks and RPC alive until widget dies.
    std::unique_ptr<OrchardWidget> widget;
    void Wait(const std::function<bool()>& condition) {
        QEventLoop loop;QTimer inspect,deadline;deadline.setSingleShot(true);bool ready=false;
        QObject::connect(&inspect,&QTimer::timeout,&loop,[&]{
            if(pending.empty() && condition()){ready=true;loop.quit();}});
        QObject::connect(&deadline,&QTimer::timeout,&loop,&QEventLoop::quit);
        inspect.start(10);deadline.start(15000);loop.exec();
        Require(ready,"actual payment widget did not reach bounded quiescent state");
    }
    std::string Snapshot() const {
        Require(bool(widget),"payment widget missing");
        const auto state=widget->paymentState();
        auto* label=widget->findChild<QLabel*>("orchardPaymentState");
        auto* balance=widget->findChild<QLabel*>("orchardBalance");
        auto* retry=widget->findChild<QPushButton*>("orchardRetry");
        Require(label && balance && retry,"payment widget labels missing");
        return Wire({{"state",state?OrchardFlow::Label(*state):QString()},
                     {"label",label->text()},{"balance",balance->text()},
                     {"calls",calls},{"pending",pending.size()},{"retry_available",!retry->isHidden() && retry->isEnabled()}});
    }
};
Client::Client(const std::string& home,const std::string& datadir,uint16_t port) : impl_(std::make_unique<Impl>()) {
    Require(ValidateEnvironment()==home,"isolation changed");
    EnsureApplication();
    Require(QFileInfo(QDir::homePath()).canonicalFilePath().toStdString()==home,"Qt home is not isolated");
    Require(port>=32768 && port!=20998,"unexpected test port");
    impl_->rpc=std::make_unique<RpcClient>(); // actual normal constructor/discovery/auth/transport
    const QUrl endpoint(QString("http://127.0.0.1:%1/").arg(port));
    Require(impl_->rpc->serverCount()==1 && impl_->rpc->currentServer()==endpoint.toString(),"discovery escaped test endpoint");
    Require(QFileInfo(impl_->rpc->datadir()).canonicalFilePath()==QFileInfo(QString::fromStdString(datadir)).canonicalFilePath(),"discovery escaped test datadir");
    Require(impl_->rpc->loadCookie(),"test authentication cookie unavailable");
}
Client::~Client()=default;
std::string Client::Call(const std::string& method,const std::string& params,const std::string& tag) {
    const auto parsed=QJsonDocument::fromJson(QByteArray::fromStdString(params));
    Require(parsed.isObject() && !tag.empty(),"invalid test call");
    const auto route=QString::fromStdString(tag);QEventLoop loop;QTimer timer;timer.setSingleShot(true);
    QJsonObject result;int replies=0;
    QObject::connect(impl_->rpc.get(),&RpcClient::rpcResult,&loop,[&](const QString& t,const QJsonValue& v){
        if(t!=route)return;++replies;result={{"ok",true},{"tag",t},{"value",v}};loop.quit();});
    QObject::connect(impl_->rpc.get(),&RpcClient::rpcErrorDetailed,&loop,[&](const QString& t,int c,const QString& m,const QJsonValue& d){
        if(t!=route)return;++replies;result={{"ok",false},{"tag",t},{"code",c},{"message",m},{"data",d}};loop.quit();});
    QObject::connect(&timer,&QTimer::timeout,&loop,&QEventLoop::quit);
    timer.start(15000);impl_->rpc->callNamedAs(QString::fromStdString(method),parsed.object(),route);
    if(!replies)loop.exec();
    Require(replies==1,"real RPC did not produce exactly one bounded reply");
    return Wire(result);
}
std::string Client::ReadWidget(const std::string& wallet) {
    OrchardWidget widget(impl_->rpc.get());QSet<QString> pending;QEventLoop loop;
    QObject::connect(&widget,&OrchardWidget::requestSent,&loop,[&](const QString&,const QJsonObject&,const QString& tag){pending.insert(tag);});
    QObject::connect(impl_->rpc.get(),&RpcClient::rpcResult,&loop,[&](const QString& tag,const QJsonValue&){pending.remove(tag);});
    QObject::connect(impl_->rpc.get(),&RpcClient::rpcErrorDetailed,&loop,[&](const QString& tag,int,const QString&,const QJsonValue&){pending.remove(tag);});
    auto* accounts=widget.findChild<QComboBox*>("orchardAccountSelector");
    auto* balance=widget.findChild<QLabel*>("orchardBalance");
    auto* history=widget.findChild<QTableWidget*>("orchardHistoryTable");
    auto* receive=widget.findChild<QPushButton*>("orchardNewAddress");
    auto* activation=widget.findChild<QLabel*>("orchardIntro");
    Require(accounts && balance && history && receive && activation,"actual widget controls missing");
    QTimer inspect,deadline;deadline.setSingleShot(true);bool ready=false;
    QObject::connect(&inspect,&QTimer::timeout,&loop,[&]{
        if(pending.empty() && accounts->count()==2 && history->rowCount()==1 && balance->text()!="Unknown" && receive->isEnabled()){
            ready=true;loop.quit();}});
    QObject::connect(&deadline,&QTimer::timeout,&loop,&QEventLoop::quit);
    widget.setWalletScope(QString::fromStdString(wallet));widget.setWalletUnlocked(true);
    inspect.start(10);deadline.start(15000);loop.exec();
    Require(ready,"actual widget did not finish authenticated catalog/balance/history reads");
    QJsonArray ids;for(int i=0;i<accounts->count();++i)ids.append(accounts->itemData(i).toLongLong());
    return Wire({{"accounts",ids},{"balance",balance->text()},{"history_rows",history->rowCount()},
                 {"history_type",history->item(0,0)?history->item(0,0)->text():QString()},
                 {"activation",activation->text()},{"receive_enabled",receive->isEnabled()},
                 {"history_amount",history->item(0,1)?history->item(0,1)->text():QString()},
                 {"payment_present",widget.paymentState().has_value()},{"pending",pending.size()}});
}

std::string Client::BeginWidgetPayment(const std::string& wallet,const std::string& mode,
        const std::string& address,uint64_t amount,uint64_t fee) {
    Require(!impl_->widget,"payment widget already exists");
    impl_->widget=std::make_unique<OrchardWidget>(impl_->rpc.get());
    auto* widget=impl_->widget.get();
    QObject::connect(widget,&OrchardWidget::requestSent,widget,[this](const QString& method,const QJsonObject& params,const QString& tag){
        impl_->pending.insert(tag);impl_->calls.append(QJsonObject{{"method",method},{"params",params},{"tag",tag}});});
    QObject::connect(impl_->rpc.get(),&RpcClient::rpcResult,widget,[this](const QString& tag,const QJsonValue&){impl_->pending.remove(tag);});
    QObject::connect(impl_->rpc.get(),&RpcClient::rpcErrorDetailed,widget,[this](const QString& tag,int,const QString&,const QJsonValue&){impl_->pending.remove(tag);});
    auto* selector=widget->findChild<QComboBox*>("orchardMode");
    auto* recipient=widget->findChild<QLineEdit*>("orchardRecipient");
    auto* amountInput=widget->findChild<QLineEdit*>("orchardAmount");
    auto* feeInput=widget->findChild<QLineEdit*>("orchardFee");
    auto* review=widget->findChild<QPushButton*>("orchardReview");
    Require(selector && recipient && amountInput && feeInput && review,"payment controls missing");
    const auto index=selector->findData(QString::fromStdString(mode));Require(index>=0,"unknown payment mode");
    selector->setCurrentIndex(index);recipient->setText(QString::fromStdString(address));
    amountInput->setText(OrchardFlow::FormatDin(amount));feeInput->setText(OrchardFlow::FormatDin(fee));
    widget->setWalletScope(QString::fromStdString(wallet));widget->setWalletUnlocked(true);
    impl_->Wait([&]{return review->isEnabled();});
    QTimer accept,deadline;deadline.setSingleShot(true);bool accepted=false;
    QObject::connect(&accept,&QTimer::timeout,widget,[&]{
        auto* box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if(!box || box->parentWidget()!=widget)return;
        for(auto* button:box->buttons())if(box->buttonRole(button)==QMessageBox::AcceptRole){
            accepted=true;accept.stop();button->click();return;}
    });
    QObject::connect(&deadline,&QTimer::timeout,widget,[&]{
        if(auto* box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
            if(box->parentWidget()==widget)box->reject();
    });
    accept.start(10);deadline.start(15000);review->click();accept.stop();deadline.stop();
    Require(accepted,"actual payment review was not accepted");
    impl_->Wait([&]{return widget->paymentState().has_value() &&
        *widget->paymentState()!=OrchardFlow::State::Draft;});
    Require(*widget->paymentState()!=OrchardFlow::State::Confirmed,"unmined payment shown confirmed");
    return impl_->Snapshot();
}
std::string Client::WaitWidgetPayment(const std::string& expected) {
    Require(bool(impl_->widget),"payment widget missing");
    Require(expected=="submitted" || expected=="confirmed" || expected=="unconfirmed","unknown payment expectation");
    impl_->Wait([&]{
        const auto state=impl_->widget->paymentState();if(!state)return false;
        if(expected=="submitted")return *state==OrchardFlow::State::Submitted;
        if(expected=="confirmed")return *state==OrchardFlow::State::Confirmed;
        return *state!=OrchardFlow::State::Confirmed;
    });
    return impl_->Snapshot();
}
std::string Client::RetryWidgetPayment() {
    Require(bool(impl_->widget),"payment widget missing");
    auto* retry=impl_->widget->findChild<QPushButton*>("orchardRetry");
    Require(retry && !retry->isHidden() && retry->isEnabled(),"explicit retry unavailable");
    retry->click();
    return WaitWidgetPayment("submitted");
}
}
