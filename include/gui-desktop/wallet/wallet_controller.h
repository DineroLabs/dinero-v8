#pragma once
#include <QObject>
#include <QString>
#include <memory>
#include <string>
#include "wallet/hd_wallet.h"

class WalletController : public QObject {
  Q_OBJECT
public:
  explicit WalletController(QObject* parent=nullptr);
  void setDataDir(const QString& dir);     // e.g. /Users/.../Dinero/data/mainnet/wallet
  void setCoinType(uint32_t coinType);     // Default: 1448 (Dinero SLIP-44)

signals:
  void addressReady(QString bech32);
  void errorMessage(QString msg);

public slots:
  void generateAddress();

private:
  std::unique_ptr<HDWallet> wallet_;
  QString datadir_;
  uint32_t coin_type_{1};
  void ensureWallet();
};
