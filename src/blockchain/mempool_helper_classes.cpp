#include "blockchain/real_mempool_integration.h"
#include "wallet/transaction.h"
#include "common/sha256d.h"
#include <QDebug>
#include <algorithm>
#include <numeric>

// TransactionBuilder Implementation
TransactionBuilder::TransactionBuilder() : valid_(true) {
    tx_.version = 1;
    tx_.lockTime = 0;
}

TransactionBuilder::~TransactionBuilder() {
}

TransactionBuilder& TransactionBuilder::setVersion(uint32_t version) {
    tx_.version = version;
    return *this;
}

TransactionBuilder& TransactionBuilder::setLockTime(uint32_t lockTime) {
    tx_.lockTime = lockTime;
    return *this;
}

TransactionBuilder& TransactionBuilder::addInput(const QString& txid, uint32_t vout, const QString& scriptSig) {
    dinero::TxIn input;
    input.prevout.txid = txid.toStdString();
    input.prevout.vout = vout;
    input.scriptSig = scriptSig.toStdString();
    input.sequence = 0xffffffff;
    
    tx_.vin.push_back(input);
    return *this;
}

TransactionBuilder& TransactionBuilder::addOutput(double amount, const QString& scriptPubKey) {
    dinero::TxOut output;
    output.value = static_cast<uint64_t>(amount * 100000000); // Convert to una
    output.scriptPubKey = scriptPubKey.toStdString();
    
    tx_.vout.push_back(output);
    return *this;
}

TransactionBuilder& TransactionBuilder::addOutput(double amount, const QString& address, bool isP2WPKH) {
    QString scriptPubKey = createScriptPubKey(address, isP2WPKH);
    dinero::TxOut output;
    output.value = static_cast<uint64_t>(amount * 100000000); // Convert to una
    output.scriptPubKey = scriptPubKey.toStdString();
    
    tx_.vout.push_back(output);
    return *this;
}

dinero::Transaction TransactionBuilder::build() {
    if (!validateInputs() || !validateOutputs()) {
        valid_ = false;
        error_ = "Transaction validation failed";
        return dinero::Transaction();
    }
    
    valid_ = true;
    return tx_;
}

QString TransactionBuilder::buildHex() {
    dinero::Transaction tx = build();
    if (!valid_) {
        return QString();
    }
    
    return QString::fromStdString(tx.Serialize());
}

bool TransactionBuilder::isValid() const {
    return valid_ && !tx_.vin.empty() && !tx_.vout.empty();
}

QString TransactionBuilder::getError() const {
    return error_;
}

double TransactionBuilder::calculateFee(double feeRate) const {
    double size = calculateSize();
    return size * feeRate;
}

double TransactionBuilder::calculateSize() const {
    // Simplified size calculation
    size_t tx_size = tx_.Serialize().size() / 2; // Hex string size / 2 = bytes
    return static_cast<double>(tx_size);
}

QString TransactionBuilder::createScriptPubKey(const QString& address, bool isP2WPKH) {
    if (isP2WPKH) {
        // P2WPKH: OP_0 <20-byte-hash>
        return "0014" + address.mid(4, 40); // Extract hash from bech32 address
    } else {
        // P2PKH: OP_DUP OP_HASH160 <20-byte-hash> OP_EQUALVERIFY OP_CHECKSIG
        return "76a914" + address.mid(4, 40) + "88ac";
    }
}

bool TransactionBuilder::validateInputs() {
    if (tx_.vin.empty()) {
        error_ = "Transaction has no inputs";
        return false;
    }
    
    for (const auto& input : tx_.vin) {
        if (input.prevout.txid.empty()) {
            error_ = "Input has empty transaction ID";
            return false;
        }
    }
    
    return true;
}

bool TransactionBuilder::validateOutputs() {
    if (tx_.vout.empty()) {
        error_ = "Transaction has no outputs";
        return false;
    }
    
    for (const auto& output : tx_.vout) {
        if (output.value == 0) {
            error_ = "Output has zero value";
            return false;
        }
        if (output.scriptPubKey.empty()) {
            error_ = "Output has empty script";
            return false;
        }
    }
    
    return true;
}

// FeeCalculator Implementation
FeeCalculator::FeeCalculator() {
}

FeeCalculator::~FeeCalculator() {
}

double FeeCalculator::calculateFee(double amount, double feeRate) const {
    // Simplified fee calculation based on estimated transaction size
    double estimatedSize = 250.0; // bytes (typical transaction size)
    return estimatedSize * feeRate;
}

double FeeCalculator::calculateFeeRate(double amount, double fee) const {
    double estimatedSize = 250.0; // bytes (typical transaction size)
    return fee / estimatedSize;
}

double FeeCalculator::calculateSize(const dinero::Transaction& tx) const {
    size_t tx_size = tx.Serialize().size() / 2; // Hex string size / 2 = bytes
    return static_cast<double>(tx_size);
}

QJsonObject FeeCalculator::estimateFee(const QString& accountId, double amount, int targetBlocks) {
    Q_UNUSED(accountId)
    
    QJsonObject estimate;
    
    if (targetBlocks <= 1) {
        estimate["feeRate"] = 0.0001; // High priority
        estimate["estimatedBlocks"] = 1;
        estimate["confidence"] = 0.99;
    } else if (targetBlocks <= 3) {
        estimate["feeRate"] = 0.00005; // Medium priority
        estimate["estimatedBlocks"] = 3;
        estimate["confidence"] = 0.95;
    } else if (targetBlocks <= 6) {
        estimate["feeRate"] = 0.00001; // Low priority
        estimate["estimatedBlocks"] = 6;
        estimate["confidence"] = 0.90;
    } else {
        estimate["feeRate"] = 0.000005; // Very low priority
        estimate["estimatedBlocks"] = 12;
        estimate["confidence"] = 0.85;
    }
    
    // Calculate total fee based on estimated transaction size
    double estimatedSize = estimateTransactionSize(accountId, amount);
    estimate["fee"] = estimate["feeRate"].toDouble() * estimatedSize;
    estimate["amount"] = amount;
    estimate["targetBlocks"] = targetBlocks;
    
    return estimate;
}

QJsonObject FeeCalculator::getRecommendedFeeRate() const {
    QJsonObject recommendation;
    recommendation["fast"] = 0.0001;     // 1 block
    recommendation["medium"] = 0.00005; // 3 blocks
    recommendation["slow"] = 0.00001;   // 6 blocks
    recommendation["verySlow"] = 0.000005; // 12 blocks
    
    return recommendation;
}

bool FeeCalculator::isValidFeeRate(double feeRate) const {
    return feeRate >= MIN_FEE_RATE && feeRate <= MAX_FEE_RATE;
}

bool FeeCalculator::isValidFee(double fee, double amount) const {
    // Fee should be reasonable compared to amount
    double maxFee = amount * 0.1; // Max 10% of amount
    return fee > 0 && fee <= maxFee;
}

double FeeCalculator::estimateTransactionSize(const QString& accountId, double amount) const {
    Q_UNUSED(accountId)
    Q_UNUSED(amount)
    
    // Simplified size estimation
    // Typical transaction: 1 input + 2 outputs = ~250 bytes
    return 250.0;
}

double FeeCalculator::getCurrentMempoolFeeRate() const {
    // Simplified mempool fee rate
    return DEFAULT_FEE_RATE;
}

// UTXOManager Implementation
UTXOManager::UTXOManager() {
}

UTXOManager::~UTXOManager() {
}

QJsonArray UTXOManager::getUTXOs(const QString& accountId) {
    std::lock_guard<std::mutex> lock(utxoMutex_);
    
    QJsonArray utxos;
    auto it = accountUTXOs_.find(accountId);
    if (it != accountUTXOs_.end()) {
        for (const auto& utxo : it->second) {
            utxos.append(utxo);
        }
    }
    
    return utxos;
}

QJsonObject UTXOManager::selectUTXOs(const QString& accountId, double amount) {
    std::lock_guard<std::mutex> lock(utxoMutex_);
    
    QJsonObject selection;
    selection["totalAmount"] = 0.0;
    selection["selectedAmount"] = amount;
    selection["changeAmount"] = 0.0;
    selection["utxoCount"] = 0;
    
    auto it = accountUTXOs_.find(accountId);
    if (it == accountUTXOs_.end()) {
        return selection;
    }
    
    std::vector<QJsonObject> availableUtxos;
    double totalAmount = 0.0;
    
    for (const auto& utxo : it->second) {
        QString utxoKey = createUTXOKey(utxo["txid"].toString(), utxo["vout"].toInt());
        if (spentUTXOs_.find(utxoKey) == spentUTXOs_.end()) {
            availableUtxos.push_back(utxo);
            totalAmount += utxo["amount"].toDouble();
        }
    }
    
    // Simple selection algorithm: use all available UTXOs
    selection["totalAmount"] = totalAmount;
    selection["selectedAmount"] = amount;
    selection["changeAmount"] = totalAmount - amount;
    selection["utxoCount"] = static_cast<int>(availableUtxos.size());
    
    return selection;
}

bool UTXOManager::spendUTXO(const QString& txid, uint32_t vout) {
    std::lock_guard<std::mutex> lock(utxoMutex_);
    
    QString utxoKey = createUTXOKey(txid, vout);
    spentUTXOs_.insert(utxoKey);
    
    return true;
}

bool UTXOManager::addUTXO(const QString& txid, uint32_t vout, double amount, const QString& scriptPubKey) {
    std::lock_guard<std::mutex> lock(utxoMutex_);
    
    QJsonObject utxo = createUTXO(txid, vout, amount, scriptPubKey);
    
    // Add to account UTXOs (simplified - in real implementation, would need account mapping)
    QString accountId = "default"; // Simplified
    accountUTXOs_[accountId].push_back(utxo);
    
    return true;
}

bool UTXOManager::isValidUTXO(const QJsonObject& utxo) const {
    return utxo.contains("txid") && 
           utxo.contains("vout") && 
           utxo.contains("amount") && 
           utxo.contains("scriptPubKey") &&
           utxo["amount"].toDouble() > 0;
}

bool UTXOManager::isUTXOSpent(const QString& txid, uint32_t vout) {
    std::lock_guard<std::mutex> lock(utxoMutex_);
    
    QString utxoKey = createUTXOKey(txid, vout);
    return spentUTXOs_.find(utxoKey) != spentUTXOs_.end();
}

double UTXOManager::getBalance(const QString& accountId) {
    std::lock_guard<std::mutex> lock(utxoMutex_);
    
    double balance = 0.0;
    auto it = accountUTXOs_.find(accountId);
    if (it != accountUTXOs_.end()) {
        for (const auto& utxo : it->second) {
            QString utxoKey = createUTXOKey(utxo["txid"].toString(), utxo["vout"].toInt());
            if (spentUTXOs_.find(utxoKey) == spentUTXOs_.end()) {
                balance += utxo["amount"].toDouble();
            }
        }
    }
    
    return balance;
}

double UTXOManager::getSpendableBalance(const QString& accountId) const {
    std::lock_guard<std::mutex> lock(utxoMutex_);
    
    double balance = 0.0;
    auto it = accountUTXOs_.find(accountId);
    if (it != accountUTXOs_.end()) {
        for (const auto& utxo : it->second) {
            if (isValidUTXO(utxo)) {
                balance += utxo["amount"].toDouble();
            }
        }
    }
    
    return balance;
}

QString UTXOManager::createUTXOKey(const QString& txid, uint32_t vout) const {
    return txid + ":" + QString::number(vout);
}

QJsonObject UTXOManager::createUTXO(const QString& txid, uint32_t vout, double amount, const QString& scriptPubKey) const {
    QJsonObject utxo;
    utxo["txid"] = txid;
    utxo["vout"] = static_cast<int>(vout);
    utxo["amount"] = amount;
    utxo["scriptPubKey"] = scriptPubKey;
    utxo["height"] = 12340; // Simulated
    utxo["confirmations"] = 6; // Simulated
    
    return utxo;
}
