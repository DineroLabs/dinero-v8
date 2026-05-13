#pragma once
#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QString>
#include <QJsonDocument>
#include <QJsonObject>
#include <functional>

class RpcHelper : public QObject {
  Q_OBJECT
  
public:
  explicit RpcHelper(QObject* parent = nullptr);
  
  Q_INVOKABLE void call(const QString& method, 
                       const QVariantList& params,
                       const QString& rpcUrl,
                       const QString& dataDir);
  
Q_SIGNALS:
  void resultReady(const QJsonObject& result);
  void errorOccurred(const QString& error);
  
private Q_SLOTS:
  void onReplyFinished();
  
private:
  QString readCookie(const QString& dataDir);
  QString makeAuthHeader(const QString& cookie);
  
  QNetworkAccessManager* nam_;
  int requestId_;
};

