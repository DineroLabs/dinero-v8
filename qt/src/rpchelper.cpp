#include "rpchelper.h"
#include <QFile>
#include <QDir>
#include <QNetworkRequest>
#include <QJsonArray>
#include <QDebug>

RpcHelper::RpcHelper(QObject* parent) 
  : QObject(parent), nam_(new QNetworkAccessManager(this)), requestId_(1) {
}

void RpcHelper::call(const QString& method, 
                     const QVariantList& params,
                     const QString& rpcUrl,
                     const QString& dataDir) {
  // Read cookie for auth
  QString cookie = readCookie(dataDir);
  if (cookie.isEmpty()) {
    Q_EMIT errorOccurred("Failed to read RPC cookie");
    return;
  }
  
  // Build JSON-RPC request
  QJsonObject request;
  request["jsonrpc"] = "2.0";
  request["id"] = requestId_++;
  request["method"] = method;
  
  QJsonArray jsonParams;
  for (const auto& param : params) {
    if (param.type() == QVariant::String) {
      jsonParams.append(param.toString());
    } else if (param.type() == QVariant::Int || param.type() == QVariant::LongLong) {
      jsonParams.append(param.toInt());
    } else if (param.type() == QVariant::Double) {
      jsonParams.append(param.toDouble());
    } else if (param.type() == QVariant::Bool) {
      jsonParams.append(param.toBool());
    }
  }
  request["params"] = jsonParams;
  
  QJsonDocument doc(request);
  QByteArray data = doc.toJson(QJsonDocument::Compact);
  
  // Setup HTTP request
  QUrl url(rpcUrl);
  QNetworkRequest netReq(url);
  netReq.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
  
  // Add Basic auth with cookie
  QString authHeader = makeAuthHeader(cookie);
  netReq.setRawHeader("Authorization", authHeader.toUtf8());
  
  // Send request
  QNetworkReply* reply = nam_->post(netReq, data);
  connect(reply, &QNetworkReply::finished, this, &RpcHelper::onReplyFinished);
}

void RpcHelper::onReplyFinished() {
  QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
  if (!reply) return;

  // Read all data from reply before scheduling deletion to avoid
  // use-after-free if the event loop processes deleteLater early.
  const QNetworkReply::NetworkError networkError = reply->error();
  const QString errorString = reply->errorString();
  const QByteArray response = reply->readAll();

  reply->deleteLater();
  reply = nullptr;

  if (networkError != QNetworkReply::NoError) {
    Q_EMIT errorOccurred(errorString);
    return;
  }
  QJsonDocument doc = QJsonDocument::fromJson(response);
  
  if (!doc.isObject()) {
    Q_EMIT errorOccurred("Invalid JSON response");
    return;
  }
  
  QJsonObject obj = doc.object();
  Q_EMIT resultReady(obj);
}

QString RpcHelper::readCookie(const QString& dataDir) {
  QDir dir(dataDir);
  QString cookiePath = dir.filePath(".cookie");
  
  QFile file(cookiePath);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    qWarning() << "Failed to open cookie file:" << cookiePath;
    return QString();
  }
  
  QString cookie = QString::fromUtf8(file.readAll()).trimmed();
  
  // Remove any CR/LF characters
  cookie = cookie.remove('\r').remove('\n').remove(' ').remove('\t');
  
  return cookie;
}

QString RpcHelper::makeAuthHeader(const QString& cookie) {
  // Cookie format is already "username:password"
  // We need to base64 encode it for Basic auth
  QByteArray auth = cookie.toUtf8().toBase64();
  return QString("Basic %1").arg(QString::fromUtf8(auth));
}

