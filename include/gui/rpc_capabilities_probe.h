#pragma once
#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QString>
#include <QStringList>
#include <QHash>

// Forward declaration
class RpcClient;

/**
 * @brief Server capabilities discovered from rpc.capabilities call
 */
struct ServerCapabilities {
    QString daemonVersion;
    int rpcVersion = 1;
    QString style = "legacy";
    bool legacyAliases = true;
    bool walletPathSupported = false;
    QString authentication = "none";
    QStringList supportedTransports;
    QStringList supportedNamespaces;
    QHash<QString, int> methodCounts;
};

/**
 * @brief Probes RPC server capabilities on startup
 * 
 * This class discovers server capabilities by calling rpc.capabilities
 * and provides the information to the GUI for optimal client behavior.
 */
class RpcCapabilitiesProbe : public QObject {
    Q_OBJECT

public:
    explicit RpcCapabilitiesProbe(QObject* parent = nullptr);
    virtual ~RpcCapabilitiesProbe();
    
    /**
     * @brief Set the RPC client to use for probing
     * @param client The RPC client instance
     */
    void setRpcClient(RpcClient* client);
    
    /**
     * @brief Start the capabilities probe
     * 
     * This will call rpc.capabilities and emit capabilitiesProbed when complete.
     * If the call fails, fallback capabilities will be used.
     */
    void probeCapabilities();
    
    /**
     * @brief Check if the probe is completed
     * @return True if the probe is completed, false otherwise
     */
    bool isProbeCompleted() const { return m_probeCompleted; }
    
    /**
     * @brief Get the discovered capabilities
     * @return Server capabilities (only valid after capabilitiesProbed signal)
     */
    const ServerCapabilities& getCapabilities() const { return m_capabilities; }

signals:
    /**
     * @brief Emitted when capabilities probe is complete
     * @param capabilities The discovered server capabilities
     */
    void capabilitiesProbed(const ServerCapabilities& capabilities);

private slots:
    void handleCapabilitiesResponse(const QJsonObject& response, const QString& error);
    void handleCapabilitiesError(const QString& error);

private:
    ServerCapabilities parseCapabilities(const QJsonObject& caps);
    ServerCapabilities createFallbackCapabilities();
    void logCapabilities(const ServerCapabilities& caps);

    RpcClient* m_rpcClient;
    bool m_probeCompleted;
    ServerCapabilities m_capabilities;
};
