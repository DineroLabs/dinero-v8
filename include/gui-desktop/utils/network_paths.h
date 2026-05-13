#pragma once
#include <QString>
#include <QDir>
#include <QStandardPaths>
#include "gui-desktop/utils/net_defaults.h"

/**
 * @brief Single source of truth for network-specific paths
 * 
 * Prevents duplicate path segments like regtest/regtest/.cookie
 * All network paths derived from one canonical function.
 */
class NetworkPaths {
public:
    /**
     * @brief Get base application directory
     * @return Base path: <AppDataLocation>/Dinero
     */
    static QString baseAppDir();
    
    /**
     * @brief Get data directory for network (daemon base)
     * @param n Network enum  
     * @return Path: <baseAppDir> (daemon will append /<network> automatically)
     */
    static QString dataDir(Network n);
    
    /**
     * @brief Get actual data directory where daemon stores files
     * @param n Network enum
     * @return Path: <baseAppDir>/<network> (where daemon actually stores data)
     */
    static QString actualDataDir(Network n);
    
    /**
     * @brief Get cookie file path for network
     * @param n Network enum
     * @return Path to .cookie file: <actualDataDir>/.cookie
     */
    static QString cookieFile(Network n);
    
    /**
     * @brief Get GUI lock file path for network
     * @param n Network enum
     * @return Path to gui.lock: <dataDir>/gui.lock  
     */
    static QString guiLock(Network n);
    
    /**
     * @brief Get nodeinfo.json path for network  
     * @param n Network enum
     * @return Path to nodeinfo.json: <dataDir>/nodeinfo.json
     */
    static QString nodeInfoPath(Network n);
    
    /**
     * @brief Get daemon lock file path for network
     * @param n Network enum  
     * @return Path to daemon.lock: <dataDir>/daemon.lock
     */
    static QString daemonLockPath(Network n);
    
    /**
     * @brief Ensure network directory exists
     * @param n Network enum
     * @return true if directory exists or was created
     */
    static bool ensureNetworkDir(Network n);
    
    // Legacy string-based methods for compatibility
    static QString networkDir(const QString& network);
    static QString cookiePath(const QString& network);
    static QString nodeInfoPath(const QString& network);
    static QString daemonLockPath(const QString& network);
    static QString guiLockPath(const QString& network);
    static bool ensureNetworkDir(const QString& network);
    
private:
    NetworkPaths() = delete; // Static utility class
};
