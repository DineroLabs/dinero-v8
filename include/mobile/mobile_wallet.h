#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <memory>

namespace dinero {
class WalletManager;
}

namespace Dinero {
namespace Mobile {

/**
 * Mobile Wallet Controller for Dinero HD Wallet
 * 
 * Features:
 * - Cross-platform mobile support (iOS/Android)
 * - HD wallet integration
 * - QR code generation and scanning
 * - Touch-friendly UI
 * - Mobile-specific security
 * - Offline transaction signing
 * - Biometric authentication
 */

class MobileWalletController : public QObject {
    Q_OBJECT
    
    Q_PROPERTY(QString currentAddress READ currentAddress NOTIFY currentAddressChanged)
    Q_PROPERTY(QStringList addressHistory READ addressHistory NOTIFY addressHistoryChanged)
    Q_PROPERTY(double balance READ balance NOTIFY balanceChanged)
    Q_PROPERTY(bool isConnected READ isConnected NOTIFY connectionChanged)
    Q_PROPERTY(QString networkStatus READ networkStatus NOTIFY networkStatusChanged)
    Q_PROPERTY(bool hasMnemonic READ hasMnemonic NOTIFY mnemonicChanged)
    Q_PROPERTY(QStringList recentTransactions READ recentTransactions NOTIFY transactionsChanged)

public:
    explicit MobileWalletController(QObject* parent = nullptr);
    ~MobileWalletController();

    // Core wallet operations
    Q_INVOKABLE bool createNewWallet();
    Q_INVOKABLE bool restoreFromMnemonic(const QString& mnemonic);
    Q_INVOKABLE QString generateNewAddress();
    Q_INVOKABLE QString getMnemonic();
    Q_INVOKABLE bool validateMnemonic(const QString& mnemonic);
    
    // Transaction operations
    Q_INVOKABLE bool sendTransaction(const QString& toAddress, double amount, const QString& memo = "");
    Q_INVOKABLE bool signTransaction(const QString& transactionData);
    Q_INVOKABLE QString createTransaction(const QString& toAddress, double amount);
    
    // QR Code operations
    Q_INVOKABLE QString generateQRCode(const QString& data);
    Q_INVOKABLE QString scanQRCode();
    Q_INVOKABLE QString generateAddressQR(const QString& address);
    Q_INVOKABLE QString generatePaymentQR(const QString& address, double amount, const QString& memo = "");
    
    // Mobile-specific features
    Q_INVOKABLE bool enableBiometricAuth();
    Q_INVOKABLE bool authenticateWithBiometrics();
    Q_INVOKABLE void lockWallet();
    Q_INVOKABLE void unlockWallet(const QString& pin);
    Q_INVOKABLE bool isWalletLocked();
    
    // Network operations
    Q_INVOKABLE void connectToNetwork();
    Q_INVOKABLE void disconnectFromNetwork();
    Q_INVOKABLE void refreshBalance();
    Q_INVOKABLE void syncTransactions();
    
    // Settings
    Q_INVOKABLE void setNetwork(const QString& network);
    Q_INVOKABLE void setCurrency(const QString& currency);
    Q_INVOKABLE void setLanguage(const QString& language);
    Q_INVOKABLE void setTheme(const QString& theme);
    
    // Getters
    QString currentAddress() const { return currentAddress_; }
    QStringList addressHistory() const { return addressHistory_; }
    double balance() const { return balance_; }
    bool isConnected() const { return isConnected_; }
    QString networkStatus() const { return networkStatus_; }
    bool hasMnemonic() const { return hasMnemonic_; }
    QStringList recentTransactions() const { return recentTransactions_; }

signals:
    void currentAddressChanged();
    void addressHistoryChanged();
    void balanceChanged();
    void connectionChanged();
    void networkStatusChanged();
    void mnemonicChanged();
    void transactionsChanged();
    
    // Transaction signals
    void transactionSent(const QString& txId);
    void transactionReceived(const QString& txId, double amount);
    void transactionFailed(const QString& error);
    
    // QR Code signals
    void qrCodeGenerated(const QString& qrData);
    void qrCodeScanned(const QString& scannedData);
    
    // Authentication signals
    void biometricAuthEnabled();
    void biometricAuthFailed();
    void walletLocked();
    void walletUnlocked();
    
    // Network signals
    void networkConnected();
    void networkDisconnected();
    void balanceUpdated(double balance);
    void transactionsSynced();

private slots:
    void onNetworkReply();
    void onBalanceTimer();
    void onSyncTimer();
    void onBiometricAuthResult(bool success);

private:
    // Core wallet
    std::unique_ptr<dinero::WalletManager> walletManager_;
    QString walletDataDir_;
    QString sessionMnemonic_;
    
    // State
    QString currentAddress_;
    QStringList addressHistory_;
    double balance_;
    bool isConnected_;
    QString networkStatus_;
    bool hasMnemonic_;
    QStringList recentTransactions_;
    
    // Network
    QNetworkAccessManager* networkManager_;
    QTimer* balanceTimer_;
    QTimer* syncTimer_;
    QString rpcUrl_;
    QString rpcCookie_;
    
    // Mobile features
    bool biometricEnabled_;
    bool walletLocked_;
    QString currentPin_;
    
    // Settings
    QString currentNetwork_;
    QString currentCurrency_;
    QString currentLanguage_;
    QString currentTheme_;
    
    // Helper methods
    void initializeWallet();
    void loadWalletSettings();
    void saveWalletSettings();
    bool makeRpcCall(const QString& method, const QVariantMap& params = QVariantMap());
    void parseRpcResponse(const QByteArray& response);
    void updateBalance();
    void updateTransactions();
    QString generateQRCodeData(const QString& data);
    bool validateAddress(const QString& address);
    bool validateAmount(double amount);
    void logTransaction(const QString& txId, const QString& type, double amount);
};

/**
 * Mobile Security Manager
 * Handles mobile-specific security features
 */
class MobileSecurityManager : public QObject {
    Q_OBJECT
    
public:
    explicit MobileSecurityManager(QObject* parent = nullptr);
    
    Q_INVOKABLE bool isBiometricAvailable();
    Q_INVOKABLE bool enableBiometricAuth();
    Q_INVOKABLE bool authenticateWithBiometrics();
    Q_INVOKABLE void disableBiometricAuth();
    
    Q_INVOKABLE bool isDeviceSecure();
    Q_INVOKABLE bool isJailbroken();
    Q_INVOKABLE QString getDeviceId();
    
    Q_INVOKABLE void enableScreenLock();
    Q_INVOKABLE void disableScreenLock();
    Q_INVOKABLE bool isScreenLockEnabled();

signals:
    void biometricAuthResult(bool success);
    void securityStatusChanged();

private:
    bool biometricAvailable_;
    bool biometricEnabled_;
    bool deviceSecure_;
    QString deviceId_;
    bool screenLockEnabled_;
    
    void checkBiometricAvailability();
    void checkDeviceSecurity();
    QString generateDeviceId();
};

/**
 * QR Code Manager
 * Handles QR code generation and scanning
 */
class QRCodeManager : public QObject {
    Q_OBJECT
    
public:
    explicit QRCodeManager(QObject* parent = nullptr);
    
    Q_INVOKABLE QString generateQRCode(const QString& data, int size = 256);
    Q_INVOKABLE QString generateAddressQR(const QString& address);
    Q_INVOKABLE QString generatePaymentQR(const QString& address, double amount, const QString& memo = "");
    Q_INVOKABLE QString generateMnemonicQR(const QString& mnemonic);
    
    Q_INVOKABLE QString scanQRCode();
    Q_INVOKABLE bool isValidQRCode(const QString& qrData);
    Q_INVOKABLE QVariantMap parsePaymentQR(const QString& qrData);

signals:
    void qrCodeGenerated(const QString& qrData);
    void qrCodeScanned(const QString& scannedData);
    void qrCodeError(const QString& error);

private:
    QString generateQRCodeData(const QString& data);
    bool validateQRData(const QString& data);
    QVariantMap parseQRData(const QString& data);
};

} // namespace Mobile
} // namespace Dinero
