#pragma once
#include <QObject>
#include <QFileSystemWatcher>
#include <QReadWriteLock>

class CookieAuthManager : public QObject {
    Q_OBJECT
public:
    static CookieAuthManager& instance();                // singleton

    void setDataDir(const QString& dataDir);             // call once on startup
    QString dataDir() const;

    bool isAvailable() const;                            // cookie present & loaded
    QString rawCookie() const;                           // "__cookie__:secret"
    QByteArray authHeader() const;                       // "Basic base64(__cookie__:secret)"
    QString  rpcUserPass() const;                        // "__cookie__:secret" (for -rpcuserpass)
    bool waitForCookie(int timeoutMs = 90000);           // block until cookie appears (startup)

signals:
    void cookieChanged();                                // fired on rotation or first load
    void availabilityChanged(bool available);

private slots:
    void onDirectoryChanged(const QString& dir);

private:
    explicit CookieAuthManager(QObject* parent=nullptr);
    void refresh();                                      // read file, update cache, emit signals
    void watchFile();

    QString dataDir_;
    QString cookiePath_;
    mutable QReadWriteLock lock_;
    QString cookie_;
    QFileSystemWatcher watcher_;
};
