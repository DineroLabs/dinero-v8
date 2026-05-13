#include "rpcclient.h"
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QFile>
#include <QDir>
#include <QTimer>

static QString defaultDatadir() {
#ifdef Q_OS_WIN
  const QString appData = QDir::fromNativeSeparators(qEnvironmentVariable("APPDATA"));
  return appData.isEmpty()
      ? QDir::home().filePath("Dinero")
      : QDir(appData).filePath("Dinero");
#elif defined(Q_OS_MAC)
  return QDir::home().filePath("Library/Application Support/Dinero");
#else
  return QDir::home().filePath(".dinero");
#endif
}

RpcClient::RpcClient(QObject* parent)
    : QObject(parent)
    , healthCheckTimer_(new QTimer(this))
    , nam_(new QNetworkAccessManager(this)) {

  datadir_ = defaultDatadir();

  // Try to auto-discover RPC endpoint from serverinfo.json
  bool discovered = loadServerInfo();

  if (!discovered) {
    // Fallback to hardcoded default
    qWarning() << "⚠️  Could not auto-discover RPC endpoint, using default port 20998";
    servers_ = {
      QUrl("http://127.0.0.1:20998/")          // Localhost RPC (standard port)
    };
  }

  // Advanced users can add custom servers via environment variable (highest priority)
  QString customRpc = qEnvironmentVariable("DINERO_RPC_URL");
  if (!customRpc.isEmpty()) {
    servers_.prepend(QUrl(customRpc));
    qDebug() << "Using custom RPC server:" << customRpc;
  }

  url_ = servers_[currentServerIndex_];

  // Load authentication cookie (CRITICAL for RPC auth)
  bool cookieLoaded = loadCookie();
  if (cookieLoaded) {
    qDebug() << "✅ Cookie loaded successfully from:" << datadir_;
  } else {
    qWarning() << "⚠️ Failed to load cookie - RPC calls will fail with authentication errors!";
  }

  // Health check every 30 seconds
  connect(healthCheckTimer_, &QTimer::timeout, this, &RpcClient::startHealthCheck);
  healthCheckTimer_->start(30000);
}

void RpcClient::setEndpoint(const QUrl& url) { 
    servers_.clear();
    servers_.append(url);
    currentServerIndex_ = 0;
    url_ = url; 
}

void RpcClient::setEndpoints(const QList<QUrl>& urls) {
    if (urls.isEmpty()) return;
    servers_ = urls;
    currentServerIndex_ = 0;
    url_ = servers_[0];
    qDebug() << "Configured" << servers_.size() << "RPC servers";
}

void RpcClient::addEndpoint(const QUrl& url) {
    if (!servers_.contains(url)) {
        servers_.append(url);
        qDebug() << "Added RPC server:" << url.toString();
    }
}

QString RpcClient::currentServer() const {
    if (currentServerIndex_ < servers_.size()) {
        return servers_[currentServerIndex_].toString();
    }
    return url_.toString();
}

void RpcClient::setDatadir(const QString& d) { 
    datadir_ = d; 
}

bool RpcClient::loadServerInfo() {
  // Search for serverinfo.json in standard locations
  QStringList serverInfoPaths = {
      // Configured datadir (highest priority)
      QDir(datadir_).filePath("serverinfo.json"),

#ifdef Q_OS_MAC
      // macOS Application Support
      QDir::home().filePath("Library/Application Support/Dinero/serverinfo.json"),
#endif

#ifdef Q_OS_WIN
      // Windows AppData
      QDir(defaultDatadir()).filePath("serverinfo.json"),
#endif

      // Legacy location kept as a read-only discovery fallback.
      QDir::home().filePath(".dinero/serverinfo.json"),
  };

  for (const QString& path : serverInfoPaths) {
      QFile f(path);
      if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
          QByteArray data = f.readAll();
          QJsonDocument doc = QJsonDocument::fromJson(data);

          if (!doc.isNull() && doc.isObject()) {
              QJsonObject info = doc.object();

              // Extract RPC configuration
              if (info.contains("rpc") && info["rpc"].isObject()) {
                  QJsonObject rpc = info["rpc"].toObject();
                  QString host = rpc["host"].toString("127.0.0.1");
                  int port = rpc["port"].toInt(20998);
                  bool tls = rpc["tls"].toBool(false);

                  QString protocol = tls ? "https" : "http";
                  QString endpoint = QString("%1://%2:%3/").arg(protocol).arg(host).arg(port);

                  servers_ = { QUrl(endpoint) };

                  qDebug() << "✅ Auto-discovered RPC endpoint from serverinfo.json:" << endpoint;
                  qDebug() << "   Features:" << info["features"].toArray();

                  // Update datadir to match where we found serverinfo
                  datadir_ = QFileInfo(path).absolutePath();

                  return true;
              }
          }
      }
  }

  return false;
}

bool RpcClient::loadCookie() {
  // Production: Only search in configured datadir and standard system locations
  QStringList cookiePaths = {
      // Configured datadir (highest priority)
      QDir(datadir_).filePath(".cookie"),

#ifdef Q_OS_MAC
      // macOS Application Support
      QDir::home().filePath("Library/Application Support/Dinero/.cookie"),
#endif

#ifdef Q_OS_WIN
      // Windows AppData
      QDir(defaultDatadir()).filePath(".cookie"),
#endif

      // Legacy location kept as a read-only discovery fallback.
      QDir::home().filePath(".dinero/.cookie"),

      // Testnet server-specific cookies (for remote RPC testing)
      QDir::home().filePath(".dinero/testnet-cookies/server1.cookie"),
      QDir::home().filePath(".dinero/testnet-cookies/server2.cookie")
  };
  
  for (const QString& path : cookiePaths) {
      QFile f(path);
      if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
          QString line = QString::fromUtf8(f.readAll());
          // Remove all whitespace (spaces, tabs, CR, LF)
          line = line.remove(' ').remove('\t').remove('\r').remove('\n');
          
          if (!line.isEmpty() && line.contains(':')) {
              // Store the entire cookie (username:password format)
              cookieToken_ = line;
              qDebug() << "Loaded cookie from:" << path;
              datadir_ = QFileInfo(path).absolutePath();
              return true;
          }
      }
  }
  
  qWarning() << "No cookie file found. RPC calls will fail without authentication.";
  qWarning() << "For testnet servers, run: ./fetch-testnet-cookies.sh";
  return false;
}

QByteArray RpcClient::authHeader() const {
  // Cookie is already in "username:password" format
  QByteArray creds = cookieToken_.toUtf8();
  return "Basic " + creds.toBase64();
}

void RpcClient::postJson(const QJsonObject& body) {
  QNetworkRequest req(url_);
  req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
  req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

  if (!cookieToken_.isEmpty()) {
      req.setRawHeader("Authorization", authHeader());
  } else {
      qWarning() << "⚠️  Making RPC call without authentication! Cookie not loaded. Method:" << body["method"].toString();
  }

  // Store method name and full request for potential retry
  QString method = body["method"].toString();
  
  auto *reply = nam_->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
  
  // Store method and body in reply's dynamic properties for failover
  reply->setProperty("rpcMethod", method);
  reply->setProperty("rpcBody", body);
  
  connect(reply, &QNetworkReply::finished, this, &RpcClient::onReplyFinished);
}

void RpcClient::tryNextServer(const QJsonObject& pendingRequest) {
  if (servers_.size() <= 1) {
    qWarning() << "No other servers available for failover";
    return;
  }
  
  // Switch to next server
  currentServerIndex_ = (currentServerIndex_ + 1) % servers_.size();
  url_ = servers_[currentServerIndex_];
  
  QString newServer = url_.toString();
  qDebug() << "✅ Switching to server:" << newServer;
  Q_EMIT serverChanged(newServer);
  Q_EMIT connectionFailed(QString("Switched to backup server: %1").arg(newServer));
  
  // Reset fail count for new server
  serverFailCount_[newServer] = 0;
  
  // Retry the request on new server
  if (!pendingRequest.isEmpty()) {
    qDebug() << "Retrying request on new server:" << pendingRequest["method"].toString();
    postJson(pendingRequest);
  }
}

void RpcClient::startHealthCheck() {
  // Quick ping to current server
  QNetworkRequest req(url_);
  req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
  if (!cookieToken_.isEmpty()) {
      req.setRawHeader("Authorization", authHeader());
  }
  
  QJsonObject ping{
    {"jsonrpc", "2.0"},
    {"id", static_cast<qint64>(nextId_++)},
    {"method", "blockchain.getblockcount"},
    {"params", QJsonArray{}}
  };
  
  auto *reply = nam_->post(req, QJsonDocument(ping).toJson(QJsonDocument::Compact));
  reply->setProperty("healthCheck", true);
  connect(reply, &QNetworkReply::finished, this, &RpcClient::onHealthCheckFinished);
}

void RpcClient::onHealthCheckFinished() {
  auto *reply = qobject_cast<QNetworkReply*>(sender());
  if (!reply) return;
  
  bool isHealthy = (reply->error() == QNetworkReply::NoError);
  reply->deleteLater();
  
  if (isHealthy) {
    serverFailCount_[url_.toString()] = 0; // Reset on successful health check
  } else {
    qDebug() << "Health check failed for" << url_.toString();
  }
}

void RpcClient::onReplyFinished() {
  auto *reply = qobject_cast<QNetworkReply*>(sender());
  if (!reply) {
    return;
  }

  QString method = reply->property("rpcMethod").toString();
  QJsonObject pendingRequest = reply->property("rpcBody").toJsonObject();

  // Read ALL data from reply before scheduling deletion.
  // After deleteLater(), the reply may be destroyed by nested event-loop
  // processing (e.g. during signal emission), so we must not access it
  // after this block.
  const QByteArray body = reply->readAll();
  const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  const QNetworkReply::NetworkError networkError = reply->error();
  const QString errorString = reply->errorString();

  // Schedule deletion — do NOT access reply after this line
  reply->deleteLater();
  reply = nullptr;

  if (networkError != QNetworkReply::NoError) {
    qDebug() << "RPC HTTP error for" << method << ":" << networkError << errorString;

    // Mark as disconnected on network error
    connected_ = false;

    // Track failure for current server
    QString currentServerUrl = url_.toString();
    serverFailCount_[currentServerUrl]++;

    // Try failover if we've had multiple failures
    if (serverFailCount_[currentServerUrl] >= MAX_FAILURES_BEFORE_SWITCH && servers_.size() > 1) {
      qWarning() << "Server" << currentServerUrl << "failed" << serverFailCount_[currentServerUrl]
                 << "times, trying next server...";
      tryNextServer(pendingRequest);
      return;
    }

    Q_EMIT rpcError(method, -1, errorString);

    // Enhancement #1: Auto-reload cookie on 401 error
    if (networkError == QNetworkReply::AuthenticationRequiredError) {
      qWarning() << "Authentication failed for" << method << ", attempting to reload cookie...";

      // Try to reload cookie (daemon may have restarted)
      if (loadCookie()) {
        qDebug() << "✅ Cookie reloaded successfully, retrying request";
        // Retry the request with new cookie (but only once to avoid infinite loop)
        if (!pendingRequest.isEmpty() && !pendingRequest.value("_cookieRetried").toBool()) {
          QJsonObject retryRequest = pendingRequest;
          retryRequest["_cookieRetried"] = true; // Mark as retried to prevent infinite loop
          postJson(retryRequest);
          return; // Don't emit connectionFailed yet, give retry a chance
        }
      }

      Q_EMIT connectionFailed("Unauthorized (cookie missing/invalid)");
    }
    return;
  }
  
  // Success! Reset failure count for this server and mark as connected
  serverFailCount_[url_.toString()] = 0;
  connected_ = true;  // We got a successful HTTP response, daemon is reachable

  // Parse JSON with error handling
  QJsonParseError parseError;
  const auto doc = QJsonDocument::fromJson(body, &parseError);

  if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
    qWarning() << "RPC parse error for" << method << ":" << parseError.errorString();
    qDebug().noquote() << "RPC body:" << QString::fromUtf8(body);
    Q_EMIT rpcError(method, -32700, QString("Parse error: %1").arg(parseError.errorString()));
    return;
  }

  const auto obj = doc.object();
  qDebug() << "RPC" << method << "HTTP" << httpStatus;
  qDebug().noquote() << "  Body:" << QString::fromUtf8(body.left(200));

  // Check for RPC-level error
  if (obj.contains("error") && !obj["error"].isNull() && obj["error"].isObject()) {
    const auto e = obj["error"].toObject();
    int code = e.value("code").toInt(-32000);
    QString message = e.value("message").toString("RPC error");
    qWarning() << "RPC error for" << method << "- code:" << code << message;
    Q_EMIT rpcError(method, code, message);
    return;
  }
  
  // Defensive: check result exists
  if (!obj.contains("result")) {
    qWarning() << "RPC missing result for" << method;
    Q_EMIT rpcError(method, -32603, "Missing result field");
    return;
  }
  
  Q_EMIT rpcResult(method, obj["result"]);
  Q_EMIT connectionOk();
}

void RpcClient::call(const QString& method, const QJsonArray& params) {
  QJsonObject body{
    {"jsonrpc", "2.0"},
    {"id", static_cast<qint64>(nextId_++)},
    {"method", method},
    {"params", params}
  };
  postJson(body);
}

void RpcClient::callNamed(const QString& method, const QJsonObject& params) {
  QJsonObject body{
    {"jsonrpc", "2.0"},
    {"id", static_cast<qint64>(nextId_++)},
    {"method", method},
    {"params", params}
  };
  postJson(body);
}

// Convenience methods
void RpcClient::getInfo() { call("blockchain.getinfo"); }
void RpcClient::getEconomics() { call("economics.getinfo"); }
void RpcClient::getSupply() { call("economics.getsupply"); }
void RpcClient::getNewAddress() { call("wallet.getnewaddress"); }
void RpcClient::validateAddress(const QString& a) { call("wallet.validateaddress", QJsonArray{a}); }
void RpcClient::getBalance() { call("wallet.getbalance"); }
void RpcClient::listAddresses() { call("wallet.listaddresses"); }
void RpcClient::getBlockCount() { call("blockchain.getblockcount"); }
void RpcClient::getBestBlockHash() { call("blockchain.getbestblockhash"); }
void RpcClient::getBlock(const QString& hash) { call("blockchain.getblock", QJsonArray{hash}); }
void RpcClient::getMempoolInfo() { call("mempool.getinfo"); }
void RpcClient::getPeerInfo() { call("getpeerinfo"); }
void RpcClient::getMiningInfo() { call("blockchain.getmininginfo"); }

// HD Wallet methods
void RpcClient::getWalletInfo() { call("wallet.getinfo"); }
void RpcClient::walletUnlock(const QString& password, int timeoutSec) {
  call("wallet.unlock", QJsonArray{password, timeoutSec});
}
void RpcClient::walletLock() { call("wallet.lock"); }
void RpcClient::walletPassphraseChange(const QString& oldPass, const QString& newPass) {
  call("wallet.passphrasechange", QJsonArray{oldPass, newPass});
}
void RpcClient::deriveAddress(int change, const QString& index) {
  call("wallet.deriveaddress", QJsonArray{change, index});
}

// Transaction methods
void RpcClient::listUnspent() {
  call("wallet.listunspent");
}

void RpcClient::createRawTransaction(const QJsonArray& inputs, const QJsonObject& outputs) {
  call("wallet.createrawtransaction", QJsonArray{inputs, outputs});
}

void RpcClient::signRawTransactionWithWallet(const QString& hexTx) {
  call("wallet.signrawtransaction", QJsonArray{hexTx});
}

void RpcClient::sendRawTransaction(const QString& hexTx) {
  call("wallet.sendrawtransaction", QJsonArray{hexTx});
}

void RpcClient::sendToAddress(const QString& address, double amount) {
  // Array format: [address, amount, fee_rate, comment, test_mode]
  // test_mode=true is REQUIRED to actually broadcast (not just preview)
  // fee_rate=0 means auto-estimate
  call("wallet.sendtoaddress", QJsonArray{address, amount, 0.0, "", true});
}

void RpcClient::sendToAddressWithFee(const QString& address, double amount, double feeRate) {
  // Array format: [address, amount, fee_rate, comment, test_mode]
  // test_mode=true is REQUIRED to actually broadcast (not just preview)
  call("wallet.sendtoaddress", QJsonArray{address, amount, feeRate, "", true});
}

void RpcClient::sendToAddressNamed(const QString& address, double amount, double feeRate, const QString& changeAddress) {
  QJsonObject params;
  params["address"] = address;
  params["amount"] = amount;
  if (feeRate > 0.0) params["fee_rate"] = feeRate;
  if (!changeAddress.isEmpty()) params["change_address"] = changeAddress;
  params["test_mode"] = true;
  callNamed("wallet.sendtoaddress", params);
}

void RpcClient::getTotalBalance() {
  call("wallet.getbalance");
}

void RpcClient::estimateSmartFee(int confTarget, const QString& mode) {
  // Phase X.4: Use DineroCoin wallet.estimatefee (takes conf_target as single param)
  Q_UNUSED(mode);  // DineroCoin estimatefee doesn't use mode parameter
  call("wallet.estimatefee", QJsonArray{confTarget});
}

// Mining methods
void RpcClient::miningInfo() {
  call("mining.info");
}

void RpcClient::miningStart(int threads) {
  QJsonObject params;
  params["threads"] = threads;
  call("mining.start", QJsonArray{params});
}

void RpcClient::miningStop() {
  call("mining.stop");
}

void RpcClient::miningSetAddress(const QString& address) {
  QJsonObject params;
  params["address"] = address;
  call("mining.setaddress", QJsonArray{params});
}

void RpcClient::miningGetAddress() {
  call("mining.getaddress");
}

// Bridge/Utreexo methods
void RpcClient::getUtreexoCacheStats() {
  call("blockchain.getutreexocachestats");
}

// Enhancement #2: Manual reconnect function
void RpcClient::reconnect() {
  qDebug() << "🔄 Manual reconnect requested";

  // Step 1: Reload cookie (may have changed if daemon restarted)
  bool cookieLoaded = loadCookie();
  if (cookieLoaded) {
    qDebug() << "✅ Cookie reloaded successfully";
  } else {
    qWarning() << "⚠️ Failed to load cookie";
  }

  // Step 2: Reset failure counters (give servers fresh start)
  for (auto& server : servers_) {
    serverFailCount_[server.toString()] = 0;
  }

  // Step 3: Force immediate health check
  startHealthCheck();

  qDebug() << "Reconnect initiated, health check in progress...";
}
