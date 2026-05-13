// modern_all_in_one.cpp
// Minimal All‑in‑One GUI for Dinero with ephemeral ports + nodeinfo.json discovery.
// Qt 6.x single-file example (Widgets). Build with: Qt >= 6.2
//
// Features:
//  - Launch/stop dinerod with ephemeral ports (-rpcport=0 -wsport=0 -port=0) and -nodeinfo
//  - Watch nodeinfo.json for effective ports, read .cookie, build Authorization header
//  - Prefer WebSocket JSON-RPC, fallback to HTTP
//  - Status panel (best block hash), Console panel (free-form JSON-RPC)
//  - Basic log tail from dinerod stdout/stderr
//
// NOTE: Adjust DINEROD_PATH if your binary isn't alongside the app.

#include <QApplication>
#include <QMainWindow>
#include <QTabWidget>
#include <QWidget>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QCheckBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QProcess>
#include <QTimer>
#include <QFileSystemWatcher>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrl>
#include <QWebSocket>
#include <QDateTime>

// ---------- Small helpers ----------
static QString platformDefaultDataDir(const QString &network)
{
#if defined(Q_OS_MAC)
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) // e.g. ~/Library/Application Support/<App>
                           .replace("/Applications", ""); // guard weird paths
    // AppDataLocation often ends with app name; ensure parent is Application Support
    // We'll just force the conventional path:
    QString home = QDir::homePath();
    return home + "/Library/Application Support/Dinero/" + network;
#elif defined(Q_OS_WIN)
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/Dinero/" + network;
#else
    return QDir::homePath() + "/.dinero/" + network;
#endif
}

static QByteArray readTrimmedFile(const QString &path)
{
    QFile f(path); if (!f.open(QIODevice::ReadOnly)) return {};
    QByteArray b = f.readAll();
    return b.trimmed();
}

static QString authHeaderFromCookieFile(const QString &cookiePath)
{
    QByteArray line = readTrimmedFile(cookiePath);
    if (line.isEmpty()) return {};
    return "Basic " + line.toBase64(); // The ENTIRE .cookie line is base64'd per server contract
}

struct NodeInfo {
    QString httpUrl; // e.g. http://127.0.0.1:63377/
    QString wsUrl;   // e.g. ws://127.0.0.1:63378/ws
    QString cookiePath; // absolute path to .cookie
    QString network; // mainnet|testnet|regtest
    bool valid() const { return !wsUrl.isEmpty() || !httpUrl.isEmpty(); }
};

static NodeInfo parseNodeInfoJson(const QString &path)
{
    NodeInfo ni; QFile f(path); if (!f.open(QIODevice::ReadOnly)) return ni;
    auto doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) return ni; auto o = doc.object();
    if (o.contains("rpc") && o["rpc"].isObject()) ni.httpUrl = o["rpc"].toObject().value("url").toString();
    if (o.contains("ws")  && o["ws"].isObject())  ni.wsUrl   = o["ws"].toObject().value("url").toString();
    if (o.contains("cookie")) ni.cookiePath = o.value("cookie").toString();
    if (o.contains("network")) ni.network   = o.value("network").toString();
    return ni;
}

// ---------- ProcessManager: launch/stop dinerod ----------
class ProcessManager : public QObject {
    Q_OBJECT
public:
    explicit ProcessManager(QObject *parent=nullptr) : QObject(parent) {
        proc_ = new QProcess(this);
        connect(proc_, &QProcess::readyReadStandardOutput, this, &ProcessManager::onStdout);
        connect(proc_, &QProcess::readyReadStandardError, this, &ProcessManager::onStderr);
        connect(proc_, &QProcess::stateChanged, this, &ProcessManager::onState);
        connect(proc_, qOverload<int,QProcess::ExitStatus>(&QProcess::finished), this, &ProcessManager::onFinished);
    }

    void setBinaryPath(QString p) { binaryPath_ = std::move(p); }

    void start(const QString &datadir, const QString &network, const QString &nodeinfoPath,
               bool enableP2P=false, bool regtest=true)
    {
        if (proc_->state() != QProcess::NotRunning) return;
        QStringList args;
        if (regtest) args << "-regtest";
        if (network == "testnet") args << "-testnet"; // mutually exclusive with regtest
        args << ("-datadir=" + datadir);
        args << "-server=1"; // ensure HTTP RPC enabled
        args << "-rpcbind=127.0.0.1" << "-rpcallowip=127.0.0.1";
        args << "-rpcport=0" << "-wsport=0" << "-port=0"; // ephemeral
        args << ("-nodeinfo=" + nodeinfoPath);
        if (enableP2P) args << "-p2p"; // optional

        // Pipe stdio so we can show logs
        proc_->setProcessChannelMode(QProcess::MergedChannels);
        
        // Smart binary path detection
        QString binaryPath = binaryPath_;
        if (binaryPath.isEmpty()) {
            QString appDir = QCoreApplication::applicationDirPath();
            
            // Try common locations relative to app
            QStringList candidates = {
                appDir + "/dinerod",
                appDir + "/../dinerod",
                appDir + "/../../dinerod", 
                appDir + "/../../../dinerod",
                appDir + "/../../../../dinerod",
                appDir + "/../../../../../bin/dinerod",
                // For macOS app bundles, go up from Contents/MacOS
                appDir + "/../../dinerod",
                appDir + "/../../../bin/dinerod",
                appDir + "/../../../../bin/dinerod",
                // Absolute paths for development
                "/Users/haydarevich/Documents/Dinero/build-debug/bin/dinerod",
                "/Users/haydarevich/Documents/Dinero/bin/dinerod"
            };
            
            for (const QString& candidate : candidates) {
                if (QFile::exists(candidate)) {
                    binaryPath = candidate;
                    emit logLine("[Process] Found dinerod at: " + binaryPath);
                    break;
                }
            }
            
            if (binaryPath.isEmpty()) {
                emit logLine("[Process] ERROR: Cannot find dinerod binary. Tried:");
                for (const QString& candidate : candidates) {
                    emit logLine("  " + candidate);
                }
                return;
            }
        }
        
        proc_->setProgram(binaryPath);
        proc_->setArguments(args);
        emit logLine("[Process] Starting: " + proc_->program() + " " + args.join(' '));
        proc_->start();
    }

    void stop() {
        if (proc_->state() == QProcess::NotRunning) return;
        emit logLine("[Process] Stopping dinerod...");
        proc_->terminate();
        if (!proc_->waitForFinished(2500)) proc_->kill();
    }

signals:
    void logLine(const QString &line);
    void runningChanged(bool running);

private slots:
    void onStdout() { emit logLine(QString::fromUtf8(proc_->readAllStandardOutput())); }
    void onStderr() { emit logLine(QString::fromUtf8(proc_->readAllStandardError())); }
    void onState(QProcess::ProcessState s){ emit runningChanged(s!=QProcess::NotRunning); }
    void onFinished(int code, QProcess::ExitStatus){ emit logLine(QString("[Process] exited %1").arg(code)); }

private:
    QProcess *proc_{};
    QString binaryPath_;
};

// ---------- RPC backends ----------
class RpcWsClient : public QObject {
    Q_OBJECT
public:
    explicit RpcWsClient(QObject *parent=nullptr) : QObject(parent) {
        connect(&ws_, &QWebSocket::connected, this, [this]{ emit connectedChanged(true); });
        connect(&ws_, &QWebSocket::disconnected, this, [this]{ emit connectedChanged(false); });
        connect(&ws_, &QWebSocket::textMessageReceived, this, &RpcWsClient::onMessage);
    }
    void open(const QUrl &url, const QByteArray &authHeader) {
        QNetworkRequest req(url);
        req.setRawHeader("Authorization", authHeader);
        ws_.open(req);
    }
    void close(){ ws_.close(); }

    using Ok = std::function<void(QJsonObject)>; using Err = std::function<void(QString)>;
    void call(const QString &method, const QJsonArray &params, Ok ok, Err err) {
        int id = ++lastId_;
        QJsonObject obj{{"jsonrpc","2.0"},{"id",id},{"method",method},{"params",params}};
        pending_[id] = {std::move(ok), std::move(err)};
        ws_.sendTextMessage(QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
    }
    bool isConnected() const { return ws_.state()==QAbstractSocket::ConnectedState; }
signals:
    void connectedChanged(bool);
private slots:
    void onMessage(const QString &m){
        auto doc = QJsonDocument::fromJson(m.toUtf8()); if (!doc.isObject()) return;
        auto o = doc.object(); int id = o.value("id").toInt();
        if (pending_.contains(id)) {
            auto pair = pending_.take(id);
            if (o.contains("error")) pair.err(QJsonDocument(o.value("error").toObject()).toJson());
            else pair.ok(o);
        }
    }
private:
    struct Pair{Ok ok; Err err;};
    QWebSocket ws_;
    int lastId_ = 0;
    QMap<int, Pair> pending_;
};

class RpcHttpClient : public QObject {
    Q_OBJECT
public:
    explicit RpcHttpClient(QObject *parent=nullptr) : QObject(parent) {}
    using Ok = std::function<void(QJsonObject)>; using Err = std::function<void(QString)>;
    void call(const QUrl &url, const QByteArray &authHeader, const QString &method, const QJsonArray &params, Ok ok, Err err) {
        QNetworkRequest r(url);
        r.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        r.setRawHeader("Authorization", authHeader);
        QJsonObject obj{{"jsonrpc","2.0"},{"id",1},{"method",method},{"params",params}};
        auto *reply = nam_.post(r, QJsonDocument(obj).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this, [=]{
            QByteArray body = reply->readAll(); reply->deleteLater();
            auto doc = QJsonDocument::fromJson(body);
            if (!doc.isObject()) { err(QString("HTTP bad JSON: %1").arg(QString::fromUtf8(body))); return; }
            auto o = doc.object();
            if (o.contains("error")) err(QJsonDocument(o.value("error").toObject()).toJson());
            else ok(o);
        });
    }
private:
    QNetworkAccessManager nam_;
};

// Facade that prefers WS, falls back to HTTP
class RpcFacade : public QObject {
    Q_OBJECT
public:
    explicit RpcFacade(QObject *parent=nullptr) : QObject(parent) {
        connect(&ws_, &RpcWsClient::connectedChanged, this, &RpcFacade::onWsConnectedChanged);
    }

    void configure(const QString &wsUrl, const QString &httpUrl, const QString &authHeader){
        wsUrl_ = wsUrl; httpUrl_ = httpUrl; authHeader_ = authHeader;
        if (!wsUrl_.isEmpty()) ws_.open(QUrl(wsUrl_), authHeader_.toUtf8());
    }

    void call(const QString &method, const QJsonArray &params, std::function<void(QJsonObject)> ok, std::function<void(QString)> err){
        if (ws_.isConnected()) {
            ws_.call(method, params, ok, err);
        } else if (!httpUrl_.isEmpty()) {
            http_.call(QUrl(httpUrl_), authHeader_.toUtf8(), method, params, ok, err);
        } else {
            err("No RPC transport available");
        }
    }

signals:
    void transportChanged(QString);

private slots:
    void onWsConnectedChanged(bool up){ emit transportChanged(up? "WebSocket" : "HTTP" ); }

private:
    RpcWsClient ws_;
    RpcHttpClient http_;
    QString wsUrl_, httpUrl_, authHeader_;
};

// ---------- Main Window ----------
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow() {
        setWindowTitle("Dinero All‑in‑One (modern)");
        auto *central = new QWidget; setCentralWidget(central);
        auto *tabs = new QTabWidget; // Status, Console, Logs

        // Top controls
        datadirEdit_ = new QLineEdit(defaultDataDir_());
        auto *browseBtn = new QPushButton("Browse…");
        auto *startBtn = new QPushButton("Start");
        auto *stopBtn  = new QPushButton("Stop"); stopBtn->setEnabled(false);
        statusLbl_ = new QLabel("stopped");
        transportLbl_ = new QLabel("transport: –");

        auto *top = new QHBoxLayout; top->addWidget(new QLabel("Data dir:")); top->addWidget(datadirEdit_); top->addWidget(browseBtn); top->addStretch(); top->addWidget(statusLbl_); top->addWidget(transportLbl_); top->addWidget(startBtn); top->addWidget(stopBtn);

        // Status tab
        auto *statusTab = new QWidget; auto *statusLay = new QFormLayout(statusTab);
        bestHashLbl_ = new QLabel("–");
        walletNameLbl_ = new QLabel("–");
        balanceLbl_ = new QLabel("–");
        statusLay->addRow("Best block hash:", bestHashLbl_);
        statusLay->addRow("Wallet:", walletNameLbl_);
        statusLay->addRow("Balance:", balanceLbl_);

        // Console tab
        auto *consoleTab = new QWidget; auto *conLay = new QVBoxLayout(consoleTab);
        methodEdit_ = new QLineEdit("getbestblockhash");
        paramsEdit_ = new QPlainTextEdit("[]"); paramsEdit_->setFixedHeight(70);
        auto *sendBtn = new QPushButton("Send");
        replyEdit_ = new QPlainTextEdit; replyEdit_->setReadOnly(true);
        auto *row = new QHBoxLayout; row->addWidget(new QLabel("method")); row->addWidget(methodEdit_); row->addWidget(sendBtn);
        conLay->addLayout(row); conLay->addWidget(new QLabel("params (JSON array)")); conLay->addWidget(paramsEdit_); conLay->addWidget(new QLabel("reply")); conLay->addWidget(replyEdit_);

        // Logs tab
        logEdit_ = new QPlainTextEdit; logEdit_->setReadOnly(true);

        tabs->addTab(statusTab, "Status");
        tabs->addTab(consoleTab, "Console");
        tabs->addTab(logEdit_,   "Logs");

        auto *mainLay = new QVBoxLayout(central);
        mainLay->addLayout(top);
        mainLay->addWidget(tabs);

        // Wiring
        connect(browseBtn, &QPushButton::clicked, this, &MainWindow::pickDatadir);
        connect(startBtn,  &QPushButton::clicked, this, &MainWindow::startNode);
        connect(stopBtn,   &QPushButton::clicked, this, &MainWindow::stopNode);
        connect(&pm_, &ProcessManager::logLine, this, &MainWindow::appendLog);
        connect(&pm_, &ProcessManager::runningChanged, this, [=](bool r){ statusLbl_->setText(r? "running" : "stopped"); startBtn->setEnabled(!r); stopBtn->setEnabled(r);} );
        connect(&rpc_, &RpcFacade::transportChanged, this, [=](const QString &t){ transportLbl_->setText("transport: "+t); });
        connect(sendBtn, &QPushButton::clicked, this, &MainWindow::sendConsoleRpc);

        // NodeInfo watcher
        watcher_ = new QFileSystemWatcher(this);
        connect(watcher_, &QFileSystemWatcher::fileChanged, this, &MainWindow::onNodeInfoChanged);

        // periodic status poll (uses whichever transport is active)
        pollTimer_ = new QTimer(this); pollTimer_->setInterval(1500);
        connect(pollTimer_, &QTimer::timeout, this, &MainWindow::pollStatus);
    }

private slots:
    void pickDatadir(){
        QString d = QFileDialog::getExistingDirectory(this, "Choose data directory", datadirEdit_->text());
        if (!d.isEmpty()) datadirEdit_->setText(d);
    }

    void startNode(){
        const QString datadir = datadirEdit_->text();
        QDir().mkpath(datadir);
        nodeinfoPath_ = QDir(datadir).filePath("nodeinfo.json");
        if (!watcher_->files().isEmpty()) watcher_->removePaths(watcher_->files());
        watcher_->addPath(nodeinfoPath_);
        pm_.start(datadir, /*network*/"regtest", nodeinfoPath_, /*p2p*/false, /*regtest*/true);
        appendLog("[UI] Waiting for nodeinfo.json at: " + nodeinfoPath_);
        // Also try a delayed read in case change notifications race
        QTimer::singleShot(800, this, &MainWindow::readNodeInfoIfAvailable);
        pollTimer_->start();
    }

    void stopNode(){
        pollTimer_->stop();
        pm_.stop();
    }

    void onNodeInfoChanged(const QString &path){ Q_UNUSED(path); readNodeInfoIfAvailable(); }

    void readNodeInfoIfAvailable(){
        if (!QFileInfo::exists(nodeinfoPath_)) return;
        NodeInfo ni = parseNodeInfoJson(nodeinfoPath_);
        if (!ni.valid()) return;
        if (ni.cookiePath.isEmpty()) ni.cookiePath = QDir(QFileInfo(nodeinfoPath_).absolutePath()).filePath(".cookie");
        currentAuth_ = authHeaderFromCookieFile(ni.cookiePath);
        appendLog("[NodeInfo] http="+ni.httpUrl+" ws="+ni.wsUrl+" cookie="+ni.cookiePath);
        rpc_.configure(ni.wsUrl, ni.httpUrl, currentAuth_);
        // kick an immediate status poll
        pollStatus();
    }

    void pollStatus(){
        if (currentAuth_.isEmpty()) return; // no auth yet
        rpc_.call("getbestblockhash", QJsonArray{},
                  [this](QJsonObject o){ bestHashLbl_->setText(o.value("result").toString()); },
                  [this](QString e){ appendLog("[RPC] getbestblockhash error: "+e); });
        // wallet.info (ignore if not available yet)
        rpc_.call("wallet.info", QJsonArray{},
                  [this](QJsonObject o){
                      auto r = o.value("result").toObject();
                      walletNameLbl_->setText(r.value("name").toString());
                      balanceLbl_->setText(QString::number(r.value("balance").toDouble()));
                  },
                  [this](QString e){ Q_UNUSED(e); /*silent*/ });
    }

    void sendConsoleRpc(){
        QString m = methodEdit_->text();
        QJsonParseError pe{}; auto arr = QJsonDocument::fromJson(paramsEdit_->toPlainText().toUtf8(), &pe).array();
        if (pe.error != QJsonParseError::NoError) { replyEdit_->setPlainText("Params JSON error: "+pe.errorString()); return; }
        rpc_.call(m, arr,
                  [this](QJsonObject o){ replyEdit_->setPlainText(QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented))); },
                  [this](QString e){ replyEdit_->setPlainText("Error: "+e); });
    }

    void appendLog(const QString &line){
        const QString ts = QDateTime::currentDateTime().toString("hh:mm:ss");
        logEdit_->appendPlainText("["+ts+"] "+line);
    }

private:
    QString defaultDataDir_() const {
        return platformDefaultDataDir("regtest");
    }

    // UI widgets
    QLineEdit *datadirEdit_{}; QLabel *statusLbl_{}; QLabel *transportLbl_{};
    QLabel *bestHashLbl_{}; QLabel *walletNameLbl_{}; QLabel *balanceLbl_{};
    QLineEdit *methodEdit_{}; QPlainTextEdit *paramsEdit_{}; QPlainTextEdit *replyEdit_{}; QPlainTextEdit *logEdit_{};

    // Infra
    ProcessManager pm_;
    RpcFacade rpc_;
    QFileSystemWatcher *watcher_{};
    QTimer *pollTimer_{};
    QString nodeinfoPath_;
    QString currentAuth_;
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    MainWindow w; w.resize(980, 680); w.show();
    return app.exec();
}

#include "modern_all_in_one.moc"
