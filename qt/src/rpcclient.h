#pragma once

#include <QObject>
#include <QUrl>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkReply>
#include <QList>

class QNetworkAccessManager;
class QTimer;

class RpcClient : public QObject {
  Q_OBJECT
public:
  explicit RpcClient(QObject* parent = nullptr);

  // Configuration
  void setEndpoint(const QUrl& url);
  void setEndpoints(const QList<QUrl>& urls); // Multi-server support
  void addEndpoint(const QUrl& url);
  void setDatadir(const QString& datadir);
  QString datadir() const { return datadir_; }  // Get current datadir
  bool loadCookie();
  
  // Server status
  QString currentServer() const;
  int serverCount() const { return servers_.size(); }
  int currentServerIndex() const { return currentServerIndex_; }

  // RPC calls
  void call(const QString& method, const QJsonArray& params = {});
  void callNamed(const QString& method, const QJsonObject& params);
  
  // Specific methods
  void getInfo();
  void getEconomics();
  void getSupply();
  void getNewAddress();
  void validateAddress(const QString& addr);
  void getBalance();
  void listAddresses();
  void getBlockCount();
  void getBestBlockHash();
  void getBlock(const QString& hash);
  void getMempoolInfo();
  void getPeerInfo();
  void getMiningInfo();
  
  // HD Wallet methods
  void getWalletInfo();
  void walletUnlock(const QString& password, int timeoutSec = 300);
  void walletLock();
  void walletPassphraseChange(const QString& oldPass, const QString& newPass);
  void deriveAddress(int change = 0, const QString& index = "next");
  
  // Transaction methods
  void listUnspent();
  void createRawTransaction(const QJsonArray& inputs, const QJsonObject& outputs);
  void signRawTransactionWithWallet(const QString& hexTx);
  void sendRawTransaction(const QString& hexTx);
  void sendToAddress(const QString& address, double amount);
  void sendToAddressWithFee(const QString& address, double amount, double feeRate);
  void sendToAddressNamed(const QString& address, double amount, double feeRate, const QString& changeAddress);
  void getTotalBalance();

  // Fee estimation (Phase 34)
  void estimateSmartFee(int confTarget = 6, const QString& mode = "ECONOMICAL");

  // Mining methods
  void miningInfo();
  void miningStart(int threads = 1);
  void miningStop();
  void miningSetAddress(const QString& address);
  void miningGetAddress();

  // Bridge/Utreexo methods
  void getUtreexoCacheStats();

  // Connection management (Enhancement #2)
  void reconnect(); // Force reconnection (reload cookie + health check)
  bool isConnected() const { return connected_; }  // Check if daemon is connected

Q_SIGNALS:
  void rpcResult(const QString& method, const QJsonValue& result);
  void rpcError(const QString& method, int code, const QString& message);
  void connectionOk();
  void connectionFailed(const QString& reason);
  void serverChanged(const QString& newServer); // New: notify when switching servers

private Q_SLOTS:
  void onReplyFinished();
  void onHealthCheckFinished();

private:
  QByteArray authHeader() const;
  void postJson(const QJsonObject& body);
  void tryNextServer(const QJsonObject& pendingRequest);
  void startHealthCheck();
  bool loadServerInfo(); // Auto-discover RPC endpoint from serverinfo.json
  
  // Server management
  QList<QUrl> servers_;
  int currentServerIndex_ = 0;
  QTimer* healthCheckTimer_;
  
  QUrl url_; // Current active URL
  QString datadir_;
  QString cookieToken_;
  quint64 nextId_ = 1;
  QNetworkAccessManager* nam_;
  
  // Failover tracking
  QMap<QString, int> serverFailCount_; // Track consecutive failures per server
  static constexpr int MAX_FAILURES_BEFORE_SWITCH = 2;

  // Connection state
  bool connected_ = false;  // Track if we successfully connected to daemon
};
