#pragma once

// Standard C++ implementation for daemon builds
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>

// Non-Qt version - stub implementation for daemon builds
class DineroRpcClient {

public:
    struct HealthInfo {
        std::string status;
        std::string chain;
        int height;
        double hashrate;
        std::string difficulty_bits;
        std::string target;
        bool mining_enabled;
        int mempool_size;
        int last_block_age;
        int uptime;
        int connections;
    };

    struct MiningInfo {
        int blocks;
        double difficulty;
        double networkhashps;
        bool mining_enabled;
        std::string mining_address;
        std::string difficulty_bits;
        std::string target;
        double hashrate_hps;
    };

    struct BlockchainInfo {
        std::string chain;
        int blocks;
        std::string bestblockhash;
        double difficulty;
        bool initialblockdownload;
        double verificationprogress;
    };

#ifdef QT_CORE_LIB
    explicit DineroRpcClient(QObject* parent = nullptr);
    ~DineroRpcClient();

    // Configuration
    void setDataDir(const QString& datadir);
    void setRpcUrl(const QString& url);
    void setCookieFile(const QString& cookieFile);
#else
    explicit DineroRpcClient() = default;
    ~DineroRpcClient() = default;

    // Configuration stubs for non-Qt builds
    void setDataDir(const std::string& datadir) {}
    void setRpcUrl(const std::string& url) {}
    void setCookieFile(const std::string& cookieFile) {}
#endif
#ifdef QT_CORE_LIB
    void setCredentials(const QString& user, const QString& password);

    // Health contract endpoints
    HealthInfo getHealth();
    MiningInfo getMiningInfo();
    BlockchainInfo getBlockchainInfo();

    // Mining control
    bool startMining(int threads = 1);
    bool stopMining();

    // Wallet operations
    QString getNewAddress();
    double getBalance();

    // Async versions with callbacks
    void getHealthAsync(std::function<void(const HealthInfo&)> callback);
    void getMiningInfoAsync(std::function<void(const MiningInfo&)> callback);
    void getBlockchainInfoAsync(std::function<void(const BlockchainInfo&)> callback);

signals:
    void healthUpdated(const HealthInfo& info);
    void miningInfoUpdated(const MiningInfo& info);
    void blockchainInfoUpdated(const BlockchainInfo& info);
    void errorOccurred(const QString& error);

private slots:
    void onReplyFinished();
    void onAuthRequired(QNetworkReply* reply, QAuthenticator* auth);
    void refreshHealth();
#else
    // Non-Qt stub methods
    void setCredentials(const std::string& user, const std::string& password) {}
    HealthInfo getHealth() { return {}; }
    MiningInfo getMiningInfo() { return {}; }
    BlockchainInfo getBlockchainInfo() { return {}; }
    bool startMining(int threads = 1) { return false; }
    bool stopMining() { return false; }
    std::string getNewAddress() { return ""; }
    double getBalance() { return 0.0; }
#endif

private:
#ifdef QT_CORE_LIB
    // Path resolution
    QString resolveDataDir() const;
    QString resolveCookieFile() const;
    QString resolveRpcUrl() const;

    // Authentication
    QByteArray getAuthHeader();
    void reloadCookie();
    bool readCookieFile(const QString& path, QString& user, QString& password);

    // RPC calls
    QNetworkReply* makeRpcCall(const QString& method, const QJsonArray& params = QJsonArray());
    QJsonObject parseRpcResponse(QNetworkReply* reply);

    // Data conversion
    HealthInfo parseHealthInfo(const QJsonObject& json);
    MiningInfo parseMiningInfo(const QJsonObject& json);
    BlockchainInfo parseBlockchainInfo(const QJsonObject& json);
#else
    // Non-Qt stub methods
    std::string resolveDataDir() const { return ""; }
    std::string resolveCookieFile() const { return ""; }
    std::string resolveRpcUrl() const { return ""; }
    std::string getAuthHeader() { return ""; }
    void reloadCookie() {}
    bool readCookieFile(const std::string& path, std::string& user, std::string& password) { return false; }
#endif

private:
#ifdef QT_CORE_LIB
    // Qt-based implementation members
    QNetworkAccessManager* m_nam;
    QTimer* m_healthTimer;
    QString m_dataDir;
    QString m_rpcUrl;
    QString m_cookieFile;
    QString m_rpcUser;
    QString m_rpcPassword;
    QByteArray m_authHeader;
    bool m_useStaticAuth;
    
    // Pending callbacks
    std::function<void(const HealthInfo&)> m_healthCallback;
    std::function<void(const MiningInfo&)> m_miningCallback;
    std::function<void(const BlockchainInfo&)> m_blockchainCallback;
#else
    // Non-Qt stub implementation members
    std::string m_dataDir;
    std::string m_rpcUrl;
    std::string m_cookieFile;
    std::string m_rpcUser;
    std::string m_rpcPassword;
    std::string m_authHeader;
    bool m_useStaticAuth = false;
#endif
};
