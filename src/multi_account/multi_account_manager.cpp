#include "multi_account/multi_account_manager.h"
#include "wallet/hd_wallet.h"
#include "wallet/bip39.h"
#include <mutex>
#include <set>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSettings>
#include <QDebug>
#include <QDateTime>
#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <random>
#include <sstream>
#include <iomanip>

namespace Dinero {
namespace MultiAccount {

MultiAccountManager::MultiAccountManager(QObject* parent)
    : QObject(parent)
    , dataDir_(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation).toStdString() + "/multi_account")
{
    // Create data directory
    QDir().mkpath(QString::fromStdString(dataDir_));
    
    // Load all accounts
    loadAllAccounts();
    
    // Initialize default account if none exist
    if (accounts_.empty()) {
        initializeDefaultAccount();
    }
    
    // Set current account to first available
    if (!accounts_.empty() && currentAccountId_.empty()) {
        currentAccountId_ = accounts_.begin()->first;
    }
}

MultiAccountManager::~MultiAccountManager() {
    saveAllAccounts();
}

QString MultiAccountManager::createAccount(const QString& name, const QString& description, 
                                          const QString& type, const QString& color) {
    std::lock_guard<std::mutex> lock(accountsMutex_);
    
    try {
        // Generate new account ID
        std::string accountId = generateAccountId();
        
        // Create account info
        AccountInfo account;
        account.accountId = accountId;
        account.settings.name = name.toStdString();
        account.settings.description = description.toStdString();
        account.settings.type = stringToAccountType(type);
        account.settings.color = color.toStdString();
        account.settings.accountIndex = accounts_.size(); // Use next available index
        account.createdAt = QDateTime::currentMSecsSinceEpoch();
        account.lastUsed = account.createdAt;
        
        // Create HD wallet for this account
        std::string walletDir = dataDir_ + "/" + accountId;
        account.mnemonic = Dinero::BIP39::MnemonicGenerator::generateMnemonic();
        
        // Generate initial address
        auto hdWallet = ::HDWallet::CreateFromMnemonic(account.mnemonic, walletDir, account.settings.coinType);
        if (hdWallet) {
            account.currentAddress = hdWallet->DeriveNextAddress();
            account.addresses.push_back(account.currentAddress);
        }
        
        // Store account
        accounts_[accountId] = account;
        saveAccount(accountId);
        
        emit accountCreated(QString::fromStdString(accountId));
        emit accountsChanged();
        
        return QString::fromStdString(accountId);
    } catch (const std::exception& e) {
        qWarning() << "Failed to create account:" << e.what();
        return QString();
    }
}

bool MultiAccountManager::deleteAccount(const QString& accountId) {
    std::lock_guard<std::mutex> lock(accountsMutex_);
    
    std::string accountIdStr = accountId.toStdString();
    
    if (accounts_.find(accountIdStr) == accounts_.end()) {
        return false;
    }
    
    // Don't allow deleting the last account
    if (accounts_.size() <= 1) {
        return false;
    }
    
    // Remove account file
    std::string accountFile = getAccountFilePath(accountIdStr);
    QFile::remove(QString::fromStdString(accountFile));
    
    // Remove from memory
    accounts_.erase(accountIdStr);
    
    // Switch to another account if this was current
    if (currentAccountId_ == accountIdStr) {
        currentAccountId_ = accounts_.begin()->first;
        emit currentAccountChanged(QString::fromStdString(currentAccountId_));
    }
    
    emit accountDeleted(accountId);
    emit accountsChanged();
    
    return true;
}

bool MultiAccountManager::switchToAccount(const QString& accountId) {
    std::lock_guard<std::mutex> lock(accountsMutex_);
    
    std::string accountIdStr = accountId.toStdString();
    
    if (accounts_.find(accountIdStr) == accounts_.end()) {
        return false;
    }
    
    currentAccountId_ = accountIdStr;
    updateAccountLastUsed(accountIdStr);
    
    emit currentAccountChanged(accountId);
    emit accountSwitched(accountId);
    
    return true;
}

QString MultiAccountManager::generateNewAddress(const QString& accountId) {
    std::lock_guard<std::mutex> lock(accountsMutex_);
    
    std::string accountIdStr = accountId.toStdString();
    
    if (accounts_.find(accountIdStr) == accounts_.end()) {
        return QString();
    }
    
    try {
        AccountInfo& account = accounts_[accountIdStr];
        
        // Load HD wallet for this account
        std::string walletDir = dataDir_ + "/" + accountIdStr;
        auto hdWallet = ::HDWallet::Open(walletDir, account.settings.coinType);
        
        if (hdWallet) {
            std::string newAddress = hdWallet->DeriveNextAddress();
            account.currentAddress = newAddress;
            account.addresses.push_back(newAddress);
            account.addressIndex++;
            
            updateAccountLastUsed(accountIdStr);
            saveAccount(accountIdStr);
            
            emit addressGenerated(accountId, QString::fromStdString(newAddress));
            emit accountUpdated(accountId);
            
            return QString::fromStdString(newAddress);
        }
    } catch (const std::exception& e) {
        qWarning() << "Failed to generate address:" << e.what();
    }
    
    return QString();
}

bool MultiAccountManager::sendTransaction(const QString& accountId, const QString& toAddress, 
                                         double amount, const QString& memo) {
    std::lock_guard<std::mutex> lock(accountsMutex_);
    
    std::string accountIdStr = accountId.toStdString();
    
    if (accounts_.find(accountIdStr) == accounts_.end()) {
        return false;
    }
    
    try {
        AccountInfo& account = accounts_[accountIdStr];
        
        // Validate transaction
        if (amount <= 0 || amount > account.balance) {
            return false;
        }
        
        // Create transaction ID
        std::string txId = "tx_" + std::to_string(QDateTime::currentMSecsSinceEpoch());
        
        // Update account balance
        account.balance -= amount;
        
        // Add to transaction history
        account.transactions.push_back(txId);
        
        updateAccountLastUsed(accountIdStr);
        saveAccount(accountIdStr);
        
        emit transactionAdded(accountId, QString::fromStdString(txId));
        emit balanceUpdated(accountId, account.balance);
        emit accountUpdated(accountId);
        
        return true;
    } catch (const std::exception& e) {
        qWarning() << "Failed to send transaction:" << e.what();
        return false;
    }
}

QString MultiAccountManager::getAccountName(const QString& accountId) {
    std::lock_guard<std::mutex> lock(accountsMutex_);
    
    std::string accountIdStr = accountId.toStdString();
    auto it = accounts_.find(accountIdStr);
    
    if (it != accounts_.end()) {
        return QString::fromStdString(it->second.settings.name);
    }
    
    return QString();
}

double MultiAccountManager::getAccountBalance(const QString& accountId) {
    std::lock_guard<std::mutex> lock(accountsMutex_);
    
    std::string accountIdStr = accountId.toStdString();
    auto it = accounts_.find(accountIdStr);
    
    if (it != accounts_.end()) {
        return it->second.balance;
    }
    
    return 0.0;
}

QString MultiAccountManager::getCurrentAddress(const QString& accountId) {
    std::lock_guard<std::mutex> lock(accountsMutex_);
    
    std::string accountIdStr = accountId.toStdString();
    auto it = accounts_.find(accountIdStr);
    
    if (it != accounts_.end()) {
        return QString::fromStdString(it->second.currentAddress);
    }
    
    return QString();
}

QStringList MultiAccountManager::accountIds() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(accountsMutex_));
    
    QStringList ids;
    for (const auto& pair : accounts_) {
        ids.append(QString::fromStdString(pair.first));
    }
    
    return ids;
}

QStringList MultiAccountManager::getAllAccountIds() {
    return accountIds();
}

double MultiAccountManager::getTotalBalance() {
    std::lock_guard<std::mutex> lock(accountsMutex_);
    
    double total = 0.0;
    for (const auto& pair : accounts_) {
        total += pair.second.balance;
    }
    
    return total;
}

QString MultiAccountManager::exportAccount(const QString& accountId) {
    std::lock_guard<std::mutex> lock(accountsMutex_);
    
    std::string accountIdStr = accountId.toStdString();
    auto it = accounts_.find(accountIdStr);
    
    if (it == accounts_.end()) {
        return QString();
    }
    
    QJsonObject accountObj;
    accountObj["accountId"] = QString::fromStdString(it->second.accountId);
    accountObj["name"] = QString::fromStdString(it->second.settings.name);
    accountObj["description"] = QString::fromStdString(it->second.settings.description);
    accountObj["type"] = accountTypeToString(it->second.settings.type);
    accountObj["color"] = QString::fromStdString(it->second.settings.color);
    accountObj["mnemonic"] = QString::fromStdString(it->second.mnemonic);
    accountObj["createdAt"] = static_cast<qint64>(it->second.createdAt);
    
    QJsonDocument doc(accountObj);
    return doc.toJson(QJsonDocument::Compact);
}

bool MultiAccountManager::importAccount(const QString& accountData) {
    try {
        QJsonDocument doc = QJsonDocument::fromJson(accountData.toUtf8());
        QJsonObject obj = doc.object();
        
        std::string mnemonic = obj["mnemonic"].toString().toStdString();
        std::string name = obj["name"].toString().toStdString();
        std::string description = obj["description"].toString().toStdString();
        std::string type = obj["type"].toString().toStdString();
        
        bool restored = restoreAccount(QString::fromStdString(mnemonic), 
                                      QString::fromStdString(name),
                                      QString::fromStdString(description),
                                      QString::fromStdString(type));
        return restored;
    } catch (const std::exception& e) {
        qWarning() << "Failed to import account:" << e.what();
        return false;
    }
}

QString MultiAccountManager::getAccountMnemonic(const QString& accountId) {
    std::lock_guard<std::mutex> lock(accountsMutex_);
    
    std::string accountIdStr = accountId.toStdString();
    auto it = accounts_.find(accountIdStr);
    
    if (it != accounts_.end()) {
        return QString::fromStdString(it->second.mnemonic);
    }
    
    return QString();
}

// Helper Methods
std::string MultiAccountManager::generateAccountId() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    
    std::stringstream ss;
    ss << "acc_";
    for (int i = 0; i < 16; ++i) {
        ss << std::hex << dis(gen);
    }
    
    return ss.str();
}

std::string MultiAccountManager::getAccountFilePath(const std::string& accountId) {
    return dataDir_ + "/" + accountId + ".json";
}

bool MultiAccountManager::loadAccount(const std::string& accountId) {
    std::string filePath = getAccountFilePath(accountId);
    QFile file(QString::fromStdString(filePath));
    
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    QJsonObject obj = doc.object();
    
    AccountInfo account;
    account.accountId = accountId;
    account.settings.name = obj["name"].toString().toStdString();
    account.settings.description = obj["description"].toString().toStdString();
    account.settings.type = stringToAccountType(obj["type"].toString());
    account.settings.color = obj["color"].toString().toStdString();
    account.settings.accountIndex = obj["accountIndex"].toInt();
    account.currentAddress = obj["currentAddress"].toString().toStdString();
    account.addressIndex = obj["addressIndex"].toInt();
    account.balance = obj["balance"].toDouble();
    account.mnemonic = obj["mnemonic"].toString().toStdString();
    account.createdAt = obj["createdAt"].toVariant().toULongLong();
    account.lastUsed = obj["lastUsed"].toVariant().toULongLong();
    
    // Load addresses array
    QJsonArray addressesArray = obj["addresses"].toArray();
    for (const QJsonValue& value : addressesArray) {
        account.addresses.push_back(value.toString().toStdString());
    }
    
    // Load transactions array
    QJsonArray transactionsArray = obj["transactions"].toArray();
    for (const QJsonValue& value : transactionsArray) {
        account.transactions.push_back(value.toString().toStdString());
    }
    
    accounts_[accountId] = account;
    return true;
}

bool MultiAccountManager::saveAccount(const std::string& accountId) {
    auto it = accounts_.find(accountId);
    if (it == accounts_.end()) {
        return false;
    }
    
    const AccountInfo& account = it->second;
    std::string filePath = getAccountFilePath(accountId);
    QFile file(QString::fromStdString(filePath));
    
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    
    QJsonObject obj;
    obj["accountId"] = QString::fromStdString(account.accountId);
    obj["name"] = QString::fromStdString(account.settings.name);
    obj["description"] = QString::fromStdString(account.settings.description);
    obj["type"] = accountTypeToString(account.settings.type);
    obj["color"] = QString::fromStdString(account.settings.color);
    obj["accountIndex"] = static_cast<int>(account.settings.accountIndex);
    obj["currentAddress"] = QString::fromStdString(account.currentAddress);
    obj["addressIndex"] = static_cast<int>(account.addressIndex);
    obj["balance"] = account.balance;
    obj["mnemonic"] = QString::fromStdString(account.mnemonic);
    obj["createdAt"] = static_cast<qint64>(account.createdAt);
    obj["lastUsed"] = static_cast<qint64>(account.lastUsed);
    
    // Save addresses array
    QJsonArray addressesArray;
    for (const std::string& address : account.addresses) {
        addressesArray.append(QString::fromStdString(address));
    }
    obj["addresses"] = addressesArray;
    
    // Save transactions array
    QJsonArray transactionsArray;
    for (const std::string& tx : account.transactions) {
        transactionsArray.append(QString::fromStdString(tx));
    }
    obj["transactions"] = transactionsArray;
    
    QJsonDocument doc(obj);
    file.write(doc.toJson());
    
    return true;
}

bool MultiAccountManager::loadAllAccounts() {
    QDir dir(QString::fromStdString(dataDir_));
    QStringList filters;
    filters << "*.json";
    QFileInfoList files = dir.entryInfoList(filters, QDir::Files);
    
    for (const QFileInfo& fileInfo : files) {
        std::string accountId = fileInfo.baseName().toStdString();
        loadAccount(accountId);
    }
    
    return true;
}

bool MultiAccountManager::saveAllAccounts() {
    std::lock_guard<std::mutex> lock(accountsMutex_);
    
    for (const auto& pair : accounts_) {
        saveAccount(pair.first);
    }
    
    return true;
}

AccountType MultiAccountManager::stringToAccountType(const QString& type) {
    if (type == "PERSONAL") return AccountType::PERSONAL;
    if (type == "BUSINESS") return AccountType::BUSINESS;
    if (type == "SAVINGS") return AccountType::SAVINGS;
    if (type == "INVESTMENT") return AccountType::INVESTMENT;
    if (type == "FAMILY") return AccountType::FAMILY;
    if (type == "CHARITY") return AccountType::CHARITY;
    if (type == "CUSTOM") return AccountType::CUSTOM;
    return AccountType::PERSONAL;
}

QString MultiAccountManager::accountTypeToString(AccountType type) {
    switch (type) {
        case AccountType::PERSONAL: return "PERSONAL";
        case AccountType::BUSINESS: return "BUSINESS";
        case AccountType::SAVINGS: return "SAVINGS";
        case AccountType::INVESTMENT: return "INVESTMENT";
        case AccountType::FAMILY: return "FAMILY";
        case AccountType::CHARITY: return "CHARITY";
        case AccountType::CUSTOM: return "CUSTOM";
        default: return "PERSONAL";
    }
}

void MultiAccountManager::updateAccountLastUsed(const std::string& accountId) {
    auto it = accounts_.find(accountId);
    if (it != accounts_.end()) {
        it->second.lastUsed = QDateTime::currentMSecsSinceEpoch();
    }
}

void MultiAccountManager::initializeDefaultAccount() {
    createAccount("Personal Account", "My personal Dinero wallet", "PERSONAL", "#3498db");
}

// Missing method implementations
bool MultiAccountManager::restoreAccount(const QString& mnemonic, const QString& name, 
                                        const QString& description, const QString& type) {
    std::lock_guard<std::mutex> lock(accountsMutex_);
    
    try {
        // Generate new account ID
        std::string accountId = generateAccountId();
        
        // Create account info
        AccountInfo account;
        account.accountId = accountId;
        account.settings.name = name.toStdString();
        account.settings.description = description.toStdString();
        account.settings.type = stringToAccountType(type);
        account.settings.accountIndex = accounts_.size();
        account.mnemonic = mnemonic.toStdString();
        account.createdAt = QDateTime::currentMSecsSinceEpoch();
        account.lastUsed = account.createdAt;
        
        // Create HD wallet for this account
        std::string walletDir = dataDir_ + "/" + accountId;
        auto hdWallet = ::HDWallet::CreateFromMnemonic(account.mnemonic, walletDir, account.settings.coinType);
        if (hdWallet) {
            account.currentAddress = hdWallet->DeriveNextAddress();
            account.addresses.push_back(account.currentAddress);
        }
        
        // Store account
        accounts_[accountId] = account;
        saveAccount(accountId);
        
        emit accountCreated(QString::fromStdString(accountId));
        emit accountsChanged();
        
        return true;
    } catch (const std::exception& e) {
        qWarning() << "Failed to restore account:" << e.what();
        return false;
    }
}

QVariantMap MultiAccountManager::getAccountSettings(const QString& accountId) {
    std::lock_guard<std::mutex> lock(accountsMutex_);
    
    std::string accountIdStr = accountId.toStdString();
    auto it = accounts_.find(accountIdStr);
    
    if (it != accounts_.end()) {
        QVariantMap settings;
        settings["name"] = QString::fromStdString(it->second.settings.name);
        settings["description"] = QString::fromStdString(it->second.settings.description);
        settings["type"] = accountTypeToString(it->second.settings.type);
        settings["color"] = QString::fromStdString(it->second.settings.color);
        settings["accountIndex"] = static_cast<int>(it->second.settings.accountIndex);
        settings["isActive"] = it->second.settings.isActive;
        settings["isHidden"] = it->second.settings.isHidden;
        return settings;
    }
    
    return QVariantMap();
}

bool MultiAccountManager::setAccountSettings(const QString& accountId, const QVariantMap& settings) {
    std::lock_guard<std::mutex> lock(accountsMutex_);
    
    std::string accountIdStr = accountId.toStdString();
    auto it = accounts_.find(accountIdStr);
    
    if (it != accounts_.end()) {
        if (settings.contains("name")) {
            it->second.settings.name = settings["name"].toString().toStdString();
        }
        if (settings.contains("description")) {
            it->second.settings.description = settings["description"].toString().toStdString();
        }
        if (settings.contains("color")) {
            it->second.settings.color = settings["color"].toString().toStdString();
        }
        if (settings.contains("isActive")) {
            it->second.settings.isActive = settings["isActive"].toBool();
        }
        if (settings.contains("isHidden")) {
            it->second.settings.isHidden = settings["isHidden"].toBool();
        }
        
        saveAccount(accountIdStr);
        emit accountUpdated(accountId);
        emit accountsChanged();
        
        return true;
    }
    
    return false;
}

QStringList MultiAccountManager::getActiveAccountIds() {
    std::lock_guard<std::mutex> lock(accountsMutex_);
    
    QStringList activeIds;
    for (const auto& pair : accounts_) {
        if (pair.second.settings.isActive) {
            activeIds.append(QString::fromStdString(pair.first));
        }
    }
    
    return activeIds;
}

QStringList MultiAccountManager::getTransactionHistory(const QString& accountId) {
    std::lock_guard<std::mutex> lock(accountsMutex_);
    
    std::string accountIdStr = accountId.toStdString();
    auto it = accounts_.find(accountIdStr);
    
    if (it != accounts_.end()) {
        QStringList transactions;
        for (const std::string& tx : it->second.transactions) {
            transactions.append(QString::fromStdString(tx));
        }
        return transactions;
    }
    
    return QStringList();
}

} // namespace MultiAccount
} // namespace Dinero
