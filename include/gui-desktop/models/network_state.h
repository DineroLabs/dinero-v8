#pragma once

#include <QString>
#include <QDateTime>
#include <QObject>

/**
 * NetworkState - Unified state model for all network/blockchain status
 * 
 * This centralizes all status information to prevent UI inconsistencies
 * between status bar, status tab, and other components.
 */
class NetworkState : public QObject {
    Q_OBJECT

public:
    enum class ConnectionStatus {
        Disconnected,    // No daemon connection
        Connecting,      // Authentication in progress  
        Connected        // Full RPC connection established
    };

    enum class SyncStatus {
        Unknown,         // Initial state
        Syncing,         // Downloading blocks
        Synced,          // Up to date
        LocalOnly        // Regtest with 0 connections (normal)
    };

    explicit NetworkState(QObject* parent = nullptr);

    // Connection state
    ConnectionStatus connectionStatus() const { return m_connectionStatus; }
    void setConnectionStatus(ConnectionStatus status);

    // Network info
    QString network() const { return m_network; }
    void setNetwork(const QString& network);
    
    QString networkDisplayName() const;  // "regtest (BETA SAFE)", "mainnet", etc.
    
    // Blockchain state
    int blockHeight() const { return m_blockHeight; }
    void setBlockHeight(int height);
    
    QString bestBlockHash() const { return m_bestBlockHash; }
    void setBestBlockHash(const QString& hash);
    
    double difficulty() const { return m_difficulty; }
    void setDifficulty(double difficulty);
    
    // Network connectivity
    int connections() const { return m_connections; }
    void setConnections(int connections);
    
    QString version() const { return m_version; }
    void setVersion(const QString& version);
    
    QDateTime lastBlockTime() const { return m_lastBlockTime; }
    void setLastBlockTime(const QDateTime& time);
    
    // Computed states
    SyncStatus syncStatus() const;
    QString syncStatusText() const;
    QString connectionStatusText() const;
    QString statusBarText() const;  // Unified status for footer
    
    // Network-specific helpers
    bool isRegtest() const { return m_network == "regtest"; }
    bool isMainnet() const { return m_network == "mainnet"; }
    bool isTestnet() const { return m_network == "testnet"; }
    
    // Update from RPC responses
    void updateFromBlockchainInfo(const QJsonObject& info);
    void updateFromNetworkInfo(const QJsonObject& info);
    
signals:
    void connectionStatusChanged(ConnectionStatus status);
    void networkChanged(const QString& network);
    void blockHeightChanged(int height);
    void syncStatusChanged();
    void stateUpdated();  // General update signal

private:
    // Connection
    ConnectionStatus m_connectionStatus;
    
    // Network
    QString m_network;
    QString m_version;
    int m_connections;
    
    // Blockchain  
    int m_blockHeight;
    QString m_bestBlockHash;
    double m_difficulty;
    QDateTime m_lastBlockTime;
    QDateTime m_lastUpdate;
};
