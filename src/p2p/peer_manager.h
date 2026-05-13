#pragma once

#ifdef QT_CORE_LIB
#include <QTcpServer>
#include <QTimer>
#include <QStringList>
#include <QList>
#else
#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#endif
#include "peer.h"

// Forward declaration for context injection (Week 3)
struct DaemonContext;

#ifdef QT_CORE_LIB
class PeerManager : public QObject {
    Q_OBJECT
#else
class PeerManager {
#endif

public:
    // ========================================================================
    // Phase W.2.6 Enhancement #4: Peer Quality Metrics
    // ========================================================================

    /**
     * @brief Aggregated peer quality statistics
     *
     * Provides read-only metrics about peer network quality.
     * Used by SlowReasonAnalyzer to detect LOW_PEER_QUALITY conditions.
     */
    struct PeerQualityStats {
        double avg_ping_ms = 0.0;          ///< Average ping across all connected peers
        double avg_download_kbps = 0.0;    ///< Average download rate (future use)
        uint32_t good_peers = 0;           ///< Peers with good ping (< 300ms)
        uint32_t bad_peers = 0;            ///< Peers with poor ping (>= 300ms)
        uint32_t total_peers = 0;          ///< Total connected peers
    };

    struct Options {
        bool enabled = false;
#ifdef QT_CORE_LIB
        quint16 listenPort = 20999;  // 0 = auto
        int maxPeers = 16;
        QStringList addNodes;        // Persistent outbound peers
        QStringList connectOnly;     // Exclusive connections (ignore addNodes)
#else
        uint16_t listenPort = 20999;  // 0 = auto
        int maxPeers = 16;
        std::vector<std::string> addNodes;        // Persistent outbound peers
        std::vector<std::string> connectOnly;     // Exclusive connections (ignore addNodes)
#endif
    };

#ifdef QT_CORE_LIB
    explicit PeerManager(const Options& opts, QObject* parent = nullptr);
#else
    explicit PeerManager(const Options& opts);
#endif

    // Week 3: Context injection (removes dinero::legacy::g_chain_db_direct() dependency)
    void SetContext(DaemonContext* ctx) { m_context = ctx; }

    bool start();
    void stop();

    /**
     * @brief Get aggregated peer quality statistics (Phase W.2.6 Enhancement #4)
     *
     * @return PeerQualityStats with current peer metrics
     */
#ifdef QT_CORE_LIB
    PeerQualityStats GetQualityStats() const;
#else
    PeerQualityStats GetQualityStats() const;  // Implemented in peer_manager_posix.cpp
#endif

#ifdef QT_CORE_LIB
public slots:
    void startP2P();  // Create Qt objects in target thread
    void stopP2P();   // Clean shutdown

    quint16 boundPort() const { return boundPort_; }
    int peerCount() const { return peers_.size(); }
    QList<Peer*> peers() const { return peers_; }

signals:
    void p2pStarted(quint16 boundPort);  // Emitted when P2P is ready
    void peerReady(Peer* peer);
    void messageFromPeer(Peer* peer, QByteArray cmd, QByteArray payload);

public slots:
    void requestHeaders(Peer* peer);

private slots:
    void onNewConnection();
    void onPeerHandshake(Peer* peer);
    void onPeerClosed(Peer* peer, QString reason);
    void onPeerMessage(Peer* peer, QByteArray cmd, QByteArray payload);
    void onRetryTimer();
#else
public:
    void startP2P();   // Implemented in peer_manager_posix.cpp
    void stopP2P();    // Implemented in peer_manager_posix.cpp

    uint16_t boundPort() const;  // Implemented in peer_manager_posix.cpp
    int peerCount() const;       // Implemented in peer_manager_posix.cpp
    void requestHeaders(void* peer);  // Implemented in peer_manager_posix.cpp
#endif
    
private:
#ifdef QT_CORE_LIB
    void dialPeer(const QString& host, quint16 port);
    void scheduleRetries();
    Peer* findBestPeer() const;

    Options options_;
    QTcpServer* server_;        // Created in target thread
    QList<Peer*> peers_;
    QTimer* retryTimer_;        // Created in target thread
    quint16 boundPort_;
    QStringList pendingConnections_;  // Failed connections to retry
#else
    // Non-Qt implementation uses POSIX sockets
    void dialPeer(const std::string& host, uint16_t port);
    void scheduleRetries();
    void runNetworkLoop();      // Background thread for socket I/O
    void acceptConnections();   // Accept incoming connections
    void handlePeer(int socket_fd, const std::string& addr);

    Options options_;
    int server_socket_ = -1;                    // Listening socket
    std::vector<int> peer_sockets_;             // Connected peer sockets
    std::vector<std::string> peer_addresses_;   // Peer addresses for stats
    std::vector<double> peer_latencies_;        // Ping latencies (ms)
    uint16_t boundPort_ = 0;
    std::vector<std::string> pendingConnections_;  // Failed connections to retry

    // Threading for non-Qt builds
    std::atomic<bool> running_{false};
    std::thread network_thread_;
    mutable std::mutex peers_mutex_;
#endif

    // Week 3: Context injection (removes dinero::legacy::g_chain_db_direct() dependency)
    DaemonContext* m_context{nullptr};
};
