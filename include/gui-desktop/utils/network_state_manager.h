#pragma once
#include <QObject>
#include <QUrl>
#include <QString>
#include <QMutex>
#include <QMutexLocker>
#include "gui-desktop/utils/net_defaults.h"
#include "gui-desktop/utils/network_paths.h"

/**
 * @brief Centralized network state manager for GUI consistency
 * 
 * Ensures all GUI components use the same network state and routing information.
 * Provides thread-safe access to current network and derived properties.
 */
class NetworkStateManager : public QObject {
    Q_OBJECT

public:
    /**
     * @brief Get singleton instance
     */
    static NetworkStateManager* instance();

    /**
     * @brief Get current active network
     */
    Network currentNetwork() const;

    /**
     * @brief Set current network and notify all components
     * @param network New network to set as active
     */
    void setCurrentNetwork(Network network);

    /**
     * @brief Get RPC port for current network
     */
    int rpcPortForCurrentNetwork() const;

    /**
     * @brief Get WebSocket port for current network
     */
    int wsPortForCurrentNetwork() const;

    /**
     * @brief Get RPC base URL for current network
     */
    QUrl rpcUrlForCurrentNetwork() const;

    /**
     * @brief Get WebSocket URL for current network
     */
    QUrl wsUrlForCurrentNetwork() const;

    /**
     * @brief Get cookie file path for current network
     */
    QString cookiePathForCurrentNetwork() const;

    /**
     * @brief Get data directory for current network
     */
    QString dataDirectoryForCurrentNetwork() const;

    /**
     * @brief Get network display name
     */
    QString currentNetworkDisplayName() const;

    /**
     * @brief Check if network is production (mainnet)
     */
    bool isProductionNetwork() const;

    /**
     * @brief Initialize with detected network from daemon
     * @param detectedNetwork Network detected from daemon response
     */
    void initializeFromDaemon(Network detectedNetwork);

signals:
    /**
     * @brief Emitted when network changes
     * @param oldNetwork Previous network
     * @param newNetwork New active network
     */
    void networkChanged(Network oldNetwork, Network newNetwork);

    /**
     * @brief Emitted when network routing info changes
     */
    void routingChanged();

private:
    explicit NetworkStateManager(QObject* parent = nullptr);
    ~NetworkStateManager() = default;

    // Prevent copying
    NetworkStateManager(const NetworkStateManager&) = delete;
    NetworkStateManager& operator=(const NetworkStateManager&) = delete;

private:
    static NetworkStateManager* s_instance;
    mutable QMutex m_mutex;
    Network m_currentNetwork{Network::Regtest}; // Default to regtest for safety
    bool m_initialized{false};
};
