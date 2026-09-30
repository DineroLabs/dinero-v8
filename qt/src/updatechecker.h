#pragma once
#include <QJsonObject>
#include <QObject>
#include <QUrl>
#include <functional>

class QJsonDocument;

class QNetworkAccessManager;

// Fetches the latest release tag and the optional network-upgrade.json notice.
// Any failure (offline, rate limit, 404, bad JSON) yields an empty value; the
// caller treats that as "nothing to report". Sends no identifiers.
class UpdateChecker : public QObject {
    Q_OBJECT
public:
    static QUrl defaultReleaseUrl();
    static QUrl defaultNoticeUrl();
    UpdateChecker(QNetworkAccessManager* nam, QUrl releaseUrl = defaultReleaseUrl(),
                  QUrl noticeUrl = defaultNoticeUrl(), QObject* parent = nullptr);
    void checkNow();

Q_SIGNALS:
    void resultReady(const QString& latestTag, const QJsonObject& noticeOrEmpty);

private:
    void fetchJson(const QUrl& url, std::function<void(const QJsonDocument&)> done);
    QNetworkAccessManager* nam_;
    QUrl releaseUrl_;
    QUrl noticeUrl_;
};
