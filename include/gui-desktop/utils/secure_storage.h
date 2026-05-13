#pragma once
#include <QString>
#include <optional>

/**
 * Secure Storage - Cross-platform secure credential storage
 * 
 * - macOS: Uses Keychain Services
 * - Windows: Uses DPAPI (Data Protection API)
 * - Linux: Uses libsecret/gnome-keyring
 * 
 * Stores long-lived RPC tokens securely so users never see raw tokens.
 */
class SecureStorage {
public:
    // Service name for keychain entries
    static constexpr const char* SERVICE_NAME = "Dinero Desktop";
    
    // Store a token securely
    // account: unique identifier (e.g., "mainnet-token", "testnet-token")
    // token: the actual bearer token to store
    static bool storeToken(const QString& account, const QString& token);
    
    // Retrieve a token securely
    // account: unique identifier
    // Returns: token if found, nullopt if not found or error
    static std::optional<QString> retrieveToken(const QString& account);
    
    // Delete a token
    // account: unique identifier
    static bool deleteToken(const QString& account);
    
    // Check if secure storage is available on this platform
    static bool isAvailable();
    
    // Get a user-friendly description of the storage backend
    static QString getBackendDescription();

private:
#ifdef Q_OS_MAC
    static bool storeTokenKeychain(const QString& account, const QString& token);
    static std::optional<QString> retrieveTokenKeychain(const QString& account);
    static bool deleteTokenKeychain(const QString& account);
#endif

#ifdef Q_OS_WIN
    static bool storeTokenDPAPI(const QString& account, const QString& token);
    static std::optional<QString> retrieveTokenDPAPI(const QString& account);
    static bool deleteTokenDPAPI(const QString& account);
#endif

#ifdef Q_OS_LINUX
    static bool storeTokenLibsecret(const QString& account, const QString& token);
    static std::optional<QString> retrieveTokenLibsecret(const QString& account);
    static bool deleteTokenLibsecret(const QString& account);
#endif

    // Fallback: encrypted file storage (when system keychain unavailable)
    static bool storeTokenFile(const QString& account, const QString& token);
    static std::optional<QString> retrieveTokenFile(const QString& account);
    static bool deleteTokenFile(const QString& account);
    static QString getTokenFilePath(const QString& account);
};
