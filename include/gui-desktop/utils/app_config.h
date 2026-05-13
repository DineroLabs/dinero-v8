#pragma once

#include <QObject>
#include <QString>

class AppConfig : public QObject {
    Q_OBJECT
    
public:
    explicit AppConfig(QObject *parent = nullptr);
    
    bool initialize();
    
    // Getters
    QString rpcUrl() const { return m_rpcUrl; }
    QString cookiePath() const { return m_cookiePath; }
    QString network() const { return m_network; }
    bool autoConnect() const { return m_autoConnect; }
    
    // Setters
    void setRpcUrl(const QString &url) { m_rpcUrl = url; saveSettings(); }
    void setCookiePath(const QString &path) { m_cookiePath = path; saveSettings(); }
    void setNetwork(const QString &network) { m_network = network; saveSettings(); }
    void setAutoConnect(bool autoConnect) { m_autoConnect = autoConnect; saveSettings(); }

private:
    void loadSettings();
    void saveSettings();
    
    QString m_rpcUrl;
    QString m_cookiePath;
    QString m_network;
    bool m_autoConnect;
};
