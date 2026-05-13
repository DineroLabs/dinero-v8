#pragma once

#ifdef QT_CORE_LIB
#include <QTcpSocket>
#include <QObject>
#include <QTimer>
#include <QHostAddress>
#include <QRandomGenerator>
#include <QByteArray>
#else
// Non-Qt fallback includes
#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <functional>
#endif

#include "p2p_message.h"

#ifdef QT_CORE_LIB
class Peer : public QObject {
    Q_OBJECT
    
public:
    enum State { 
        Connecting, 
        Handshake, 
        Ready, 
        Closed 
    };
    
    // Outbound connection
    Peer(const QHostAddress& host, quint16 port, QObject* parent = nullptr);
    
    // Inbound connection (from accepted socket)
    Peer(QTcpSocket* accepted, QObject* parent = nullptr);
#else
// Non-Qt Peer implementation
class Peer {
public:
    enum State { 
        Connecting, 
        Handshake, 
        Ready, 
        Closed 
    };
    
    // Outbound connection
    Peer(const std::string& host, uint16_t port);
    
    // Inbound connection (from accepted socket)
    Peer(int socket_fd);
#endif
    
    void start();
    
#ifdef QT_CORE_LIB
    void send(const QByteArray& cmd, const QByteArray& payload);
    void close(const QString& reason);
    
    // Getters
    QString id() const { return id_; }
    State state() const { return state_; }
    int dosScore() const { return dosScore_; }
    bool isInbound() const { return isInbound_; }
    QString address() const;
    int peerHeight() const { return peerHeight_; }
    qint64 lastPingMs() const { return lastPingMs_; }
    qint64 connectedTime() const { return connectedTime_; }
    QString direction() const { return isInbound_ ? "inbound" : "outbound"; }
    quint64 services() const { return services_; }
    
signals:
    void handshakeDone(Peer* peer);
    void gotMessage(Peer* peer, QByteArray cmd, QByteArray payload);
    void closed(Peer* peer, QString reason);
    
private slots:
    void onReadyRead();
    void onConnected();
    void onError(QAbstractSocket::SocketError error);
#else
    void send(const std::string& cmd, const std::vector<uint8_t>& payload);
    void close(const std::string& reason);
    
    // Getters
    std::string id() const { return id_; }
    State state() const { return state_; }
    int dosScore() const { return dosScore_; }
    bool isInbound() const { return isInbound_; }
    std::string address() const;
    int peerHeight() const { return peerHeight_; }
    int64_t lastPingMs() const { return lastPingMs_; }
    int64_t connectedTime() const { return connectedTime_; }
    std::string direction() const { return isInbound_ ? "inbound" : "outbound"; }
    uint64_t services() const { return services_; }
    
    // Non-Qt callback mechanism (placeholder)
    void setHandshakeCallback(std::function<void(Peer*)> cb) { handshakeCallback_ = cb; }
    void setMessageCallback(std::function<void(Peer*, std::string, std::vector<uint8_t>)> cb) { messageCallback_ = cb; }
    void setClosedCallback(std::function<void(Peer*, std::string)> cb) { closedCallback_ = cb; }
#endif
    void onPingTick();
    
private:
#ifdef QT_CORE_LIB
    void sendVersion();
    void handleMessage(const QByteArray& cmd, const QByteArray& payload);
    void addDosScore(int points, const QString& reason = QString());
    
    QTcpSocket* socket_;
    QByteArray inBuffer_;
    QTimer pingTimer_;
    QString id_;
    QString host_;
    quint16 port_;
    qint64 lastPingMs_;
    qint64 connectedTime_;
    quint64 services_;
#else
    void sendVersion();
    void handleMessage(const std::string& cmd, const std::vector<uint8_t>& payload);
    void addDosScore(int points, const std::string& reason = "");
    
    int socket_;
    std::vector<uint8_t> inBuffer_;
    std::string id_;
    std::string host_;
    uint16_t port_;
    int64_t lastPingMs_;
    int64_t connectedTime_;
    uint64_t services_;
    
    // Callback functions for non-Qt version
    std::function<void(Peer*)> handshakeCallback_;
    std::function<void(Peer*, std::string, std::vector<uint8_t>)> messageCallback_;
    std::function<void(Peer*, std::string)> closedCallback_;
#endif
    
    // Common member variables
    uint64_t lastPingNonce_;
    State state_;
    int dosScore_;
    bool isInbound_;
    int peerHeight_;
    
    // Protocol state
    bool versionSent_;
    bool versionReceived_;
    bool verackReceived_;

    // Peer's advertised services (received in version message)
#ifdef QT_CORE_LIB
    quint64 peerServices_ = 0;
#else
    uint64_t peerServices_ = 0;
#endif
};
