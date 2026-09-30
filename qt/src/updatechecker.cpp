#include "updatechecker.h"
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <functional>

QUrl UpdateChecker::defaultReleaseUrl() {
    return QUrl(QStringLiteral("https://api.github.com/repos/DineroLabs/dinero-v8/releases/latest"));
}

QUrl UpdateChecker::defaultNoticeUrl() {
    return QUrl(QStringLiteral(
        "https://github.com/DineroLabs/dinero-v8/releases/latest/download/network-upgrade.json"));
}

UpdateChecker::UpdateChecker(QNetworkAccessManager* nam, QUrl releaseUrl, QUrl noticeUrl, QObject* parent)
    : QObject(parent), nam_(nam), releaseUrl_(std::move(releaseUrl)), noticeUrl_(std::move(noticeUrl)) {}

void UpdateChecker::fetchJson(const QUrl& url, std::function<void(const QJsonDocument&)> done) {
    QNetworkRequest req(url);
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setRawHeader("User-Agent", "dinero-qt");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(10000);
    QNetworkReply* reply = nam_->get(req);
    connect(reply, &QNetworkReply::finished, this, [reply, done]() {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool httpOk = status == 0 || (status >= 200 && status < 300);  // 0 = non-HTTP (file://)
        if (reply->error() != QNetworkReply::NoError || !httpOk) {
            done(QJsonDocument());
            return;
        }
        done(QJsonDocument::fromJson(reply->readAll()));
    });
}

void UpdateChecker::checkNow() {
    fetchJson(releaseUrl_, [this](const QJsonDocument& release) {
        const QString tag = release.isObject() ? release.object().value(QStringLiteral("tag_name")).toString()
                                               : QString();
        fetchJson(noticeUrl_, [this, tag](const QJsonDocument& notice) {
            Q_EMIT resultReady(tag, notice.isObject() ? notice.object() : QJsonObject());
        });
    });
}
