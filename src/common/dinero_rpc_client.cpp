#include "common/dinero_rpc_client.h"
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCoreApplication>
#include <QDebug>
#include <QUrl>
#include <QAuthenticator>

DineroRpcClient::DineroRpcClient(QObject* parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
    , m_healthTimer(new QTimer(this))
    , m_useStaticAuth(false)
{
    // Set up network manager
    connect(m_nam, &QNetworkAccessManager::authenticationRequired,
            this, &DineroRpcClient::onAuthRequired);
    
    // Set up health refresh timer (every 30 seconds)
    m_healthTimer->setInterval(30000);
    connect(m_healthTimer, &QTimer::timeout, this, &DineroRpcClient::refreshHealth);
    
    // Auto-detect configuration
    m_dataDir = resolveDataDir();
    m_rpcUrl = resolveRpcUrl();
    m_cookieFile = resolveCookieFile();
    
    // Load initial auth
    reloadCookie();
}

DineroRpcClient::~DineroRpcClient() {
    m_healthTimer->stop();
}

void DineroRpcClient::setDataDir(const QString& datadir) {
    m_dataDir = datadir;
    m_cookieFile = resolveCookieFile();
    reloadCookie();
}

void DineroRpcClient::setRpcUrl(const QString& url) {
    m_rpcUrl = url;
}

void DineroRpcClient::setCookieFile(const QString& cookieFile) {
    m_cookieFile = cookieFile;
    reloadCookie();
}

void DineroRpcClient::setCredentials(const QString& user, const QString& password) {
    m_rpcUser = user;
    m_rpcPassword = password;
    m_useStaticAuth = true;
    m_authHeader = "Basic " + QByteArray(user.toUtf8() + ":" + password.toUtf8()).toBase64();
}

QString DineroRpcClient::resolveDataDir() const {
    // Check environment variable first
    QString envDataDir = qgetenv("DINERO_DATADIR");
    if (!envDataDir.isEmpty()) {
        return envDataDir;
    }
    
    // Use platform-specific default
    QString baseDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    
#ifdef Q_OS_MAC
    return baseDir + "/Dinero";
#elif defined(Q_OS_WIN)
    return baseDir + "/Dinero";
#else
    return baseDir + "/Dinero";
#endif
}

QString DineroRpcClient::resolveCookieFile() const {
    // Check environment variable first
    QString envCookieFile = qgetenv("DINERO_COOKIE_FILE");
    if (!envCookieFile.isEmpty()) {
        return envCookieFile;
    }
    
    // Default to datadir/.cookie
    return m_dataDir + "/.cookie";
}

QString DineroRpcClient::resolveRpcUrl() const {
    // Check environment variable first
    QString envRpcUrl = qgetenv("DINERO_RPC_URL");
    if (!envRpcUrl.isEmpty()) {
        return envRpcUrl;
    }
    
    // Default to localhost mainnet
    return "http://127.0.0.1:20998";
}

QByteArray DineroRpcClient::getAuthHeader() {
    // If using static auth, return it
    if (m_useStaticAuth) {
        return m_authHeader;
    }
    
    // Try to read cookie file
    QString user, password;
    if (readCookieFile(m_cookieFile, user, password)) {
        return "Basic " + QByteArray(user.toUtf8() + ":" + password.toUtf8()).toBase64();
    }
    
    return QByteArray(); // No auth
}

void DineroRpcClient::reloadCookie() {
    // Check for static credentials in environment
    QString envUser = qgetenv("DINERO_RPC_USER");
    QString envPassword = qgetenv("DINERO_RPC_PASSWORD");
    
    if (!envUser.isEmpty() && !envPassword.isEmpty()) {
        setCredentials(envUser, envPassword);
        return;
    }
    
    // Try to read from cookie file
    QString user, password;
    if (readCookieFile(m_cookieFile, user, password)) {
        m_useStaticAuth = false;
        m_authHeader = "Basic " + QByteArray(user.toUtf8() + ":" + password.toUtf8()).toBase64();
    } else {
        m_authHeader.clear();
    }
}

bool DineroRpcClient::readCookieFile(const QString& path, QString& user, QString& password) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    
    QByteArray data = file.readAll().trimmed();
    QString cookie = QString::fromUtf8(data);
    
    if (cookie.isEmpty()) {
        return false;
    }
    
    // Cookie format: "username:password"
    int colonPos = cookie.indexOf(':');
    if (colonPos == -1) {
        // Assume it's just the password, use default username
        user = "__cookie__";
        password = cookie;
    } else {
        user = cookie.left(colonPos);
        password = cookie.mid(colonPos + 1);
    }
    
    return true;
}

QNetworkReply* DineroRpcClient::makeRpcCall(const QString& method, const QJsonArray& params) {
    QNetworkRequest request(m_rpcUrl);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    
    QByteArray authHeader = getAuthHeader();
    if (!authHeader.isEmpty()) {
        request.setRawHeader("Authorization", authHeader);
    }
    
    QJsonObject jsonRpc;
    jsonRpc["jsonrpc"] = "2.0";
    jsonRpc["id"] = 1;
    jsonRpc["method"] = method;
    jsonRpc["params"] = params;
    
    QJsonDocument doc(jsonRpc);
    QByteArray data = doc.toJson(QJsonDocument::Compact);
    
    return m_nam->post(request, data);
}

QJsonObject DineroRpcClient::parseRpcResponse(QNetworkReply* reply) {
    QJsonObject result;
    
    if (reply->error() != QNetworkReply::NoError) {
        result["error"] = reply->errorString();
        return result;
    }
    
    QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    if (doc.isObject()) {
        QJsonObject response = doc.object();
        if (response.isMember("result")) {
            result = response["result"].toObject();
        } else if (response.isMember("error")) {
            result["error"] = response["error"];
        }
    }
    
    return result;
}

DineroRpcClient::HealthInfo DineroRpcClient::getHealth() {
    QNetworkReply* reply = makeRpcCall("gethealth");
    
    // Wait for response (blocking)
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    
    QJsonObject json = parseRpcResponse(reply);
    reply->deleteLater();
    
    return parseHealthInfo(json);
}

DineroRpcClient::MiningInfo DineroRpcClient::getMiningInfo() {
    QNetworkReply* reply = makeRpcCall("getmininginfo");
    
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    
    QJsonObject json = parseRpcResponse(reply);
    reply->deleteLater();
    
    return parseMiningInfo(json);
}

DineroRpcClient::BlockchainInfo DineroRpcClient::getBlockchainInfo() {
    QNetworkReply* reply = makeRpcCall("getblockchaininfo");
    
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    
    QJsonObject json = parseRpcResponse(reply);
    reply->deleteLater();
    
    return parseBlockchainInfo(json);
}

bool DineroRpcClient::startMining(int threads) {
    QNetworkReply* reply = makeRpcCall("startmining", QJsonArray() << threads);
    
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    
    QJsonObject json = parseRpcResponse(reply);
    reply->deleteLater();
    
    return !json.isMember("error");
}

bool DineroRpcClient::stopMining() {
    QNetworkReply* reply = makeRpcCall("stopmining");
    
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    
    QJsonObject json = parseRpcResponse(reply);
    reply->deleteLater();
    
    return !json.isMember("error");
}

QString DineroRpcClient::getNewAddress() {
    QNetworkReply* reply = makeRpcCall("getnewaddress");
    
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    
    QJsonObject json = parseRpcResponse(reply);
    reply->deleteLater();
    
    if (json.isMember("error")) {
        return QString();
    }
    
    return json["result"].toString();
}

double DineroRpcClient::getBalance() {
    QNetworkReply* reply = makeRpcCall("getbalance");
    
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    
    QJsonObject json = parseRpcResponse(reply);
    reply->deleteLater();
    
    if (json.isMember("error")) {
        return 0.0;
    }
    
    return json["result"].toDouble();
}

void DineroRpcClient::getHealthAsync(std::function<void(const HealthInfo&)> callback) {
    m_healthCallback = callback;
    
    QNetworkReply* reply = makeRpcCall("gethealth");
    connect(reply, &QNetworkReply::finished, this, &DineroRpcClient::onReplyFinished);
}

void DineroRpcClient::getMiningInfoAsync(std::function<void(const MiningInfo&)> callback) {
    m_miningCallback = callback;
    
    QNetworkReply* reply = makeRpcCall("getmininginfo");
    connect(reply, &QNetworkReply::finished, this, &DineroRpcClient::onReplyFinished);
}

void DineroRpcClient::getBlockchainInfoAsync(std::function<void(const BlockchainInfo&)> callback) {
    m_blockchainCallback = callback;
    
    QNetworkReply* reply = makeRpcCall("getblockchaininfo");
    connect(reply, &QNetworkReply::finished, this, &DineroRpcClient::onReplyFinished);
}

void DineroRpcClient::onReplyFinished() {
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;
    
    QJsonObject json = parseRpcResponse(reply);
    
    // Handle 401 by reloading cookie and retrying once
    if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 401) {
        reloadCookie();
        // Could implement retry logic here
    }
    
    // Call appropriate callback
    if (m_healthCallback) {
        m_healthCallback(parseHealthInfo(json));
        m_healthCallback = nullptr;
    } else if (m_miningCallback) {
        m_miningCallback(parseMiningInfo(json));
        m_miningCallback = nullptr;
    } else if (m_blockchainCallback) {
        m_blockchainCallback(parseBlockchainInfo(json));
        m_blockchainCallback = nullptr;
    }
    
    reply->deleteLater();
}

void DineroRpcClient::onAuthRequired(QNetworkReply* reply, QAuthenticator* auth) {
    Q_UNUSED(reply)
    
    // Try to get credentials from cookie or static auth
    QString user, password;
    if (m_useStaticAuth) {
        user = m_rpcUser;
        password = m_rpcPassword;
    } else if (readCookieFile(m_cookieFile, user, password)) {
        // Cookie loaded successfully
    } else {
        return; // No auth available
    }
    
    auth->setUser(user);
    auth->setPassword(password);
}

void DineroRpcClient::refreshHealth() {
    getHealthAsync([this](const HealthInfo& info) {
        emit healthUpdated(info);
    });
}

DineroRpcClient::HealthInfo DineroRpcClient::parseHealthInfo(const QJsonObject& json) {
    HealthInfo info;
    
    info.status = json["status"].toString().toStdString();
    info.chain = json["chain"].toString().toStdString();
    info.height = json["height"].toInt();
    info.hashrate = json["hashrate"].toDouble();
    info.difficulty_bits = json["difficulty_bits"].toString().toStdString();
    info.target = json["target"].toString().toStdString();
    info.mining_enabled = json["mining_enabled"].toBool();
    info.mempool_size = json["mempool_size"].toInt();
    info.last_block_age = json["last_block_age"].toInt();
    info.uptime = json["uptime"].toInt();
    info.connections = json["connections"].toInt();
    
    return info;
}

DineroRpcClient::MiningInfo DineroRpcClient::parseMiningInfo(const QJsonObject& json) {
    MiningInfo info;
    
    info.blocks = json["blocks"].toInt();
    info.difficulty = json["difficulty"].toDouble();
    info.networkhashps = json["networkhashps"].toDouble();
    info.mining_enabled = json["mining_enabled"].toBool();
    info.mining_address = json["mining_address"].toString().toStdString();
    info.difficulty_bits = json["difficulty_bits"].toString().toStdString();
    info.target = json["target"].toString().toStdString();
    info.hashrate_hps = json["hashrate_hps"].toDouble();
    
    return info;
}

DineroRpcClient::BlockchainInfo DineroRpcClient::parseBlockchainInfo(const QJsonObject& json) {
    BlockchainInfo info;
    
    info.chain = json["chain"].toString().toStdString();
    info.blocks = json["blocks"].toInt();
    info.bestblockhash = json["bestblockhash"].toString().toStdString();
    info.difficulty = json["difficulty"].toDouble();
    info.initialblockdownload = json["initialblockdownload"].toBool();
    info.verificationprogress = json["verificationprogress"].toDouble();
    
    return info;
}
