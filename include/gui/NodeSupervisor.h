#pragma once
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonArray>
#include <QFileSystemWatcher>
#include <memory>

// ✅ NEW: Two-phase daemon spawning state machine
enum class Phase { Idle, Auto, Live };

struct NodeConfig {
    QString daemonPath;   // e.g. "<app>/dinerod"
    QString cliPath;      // e.g. "<app>/dinero-cli"
    QString dataDir;      // e.g. "/Users/haydarevich/Documents/Dinero/data/mainnet"
    QString rpcHost = "127.0.0.1";
    int     rpcPort = 20998;       // adjust per network
    int     wsPort = 22999;        // WebSocket port
    QString logFile = "logs/daemon.log";
    int     rpcWaitSec = 90;       // readiness timeout

    // NEW: Remote mode flag - if true, skip daemon spawning
    bool    remoteMode = false;    // If true, connect to existing daemon
    bool    rpcbind0000 = false;   // If true, listen on 0.0.0.0
};

class NodeSupervisor : public QObject {
    Q_OBJECT
public:
    explicit NodeSupervisor(QObject* parent=nullptr);
    ~NodeSupervisor();
    explicit NodeSupervisor(const NodeConfig& cfg, QObject* parent=nullptr);

    void setConfig(const NodeConfig& cfg);
    void ensureStarted();          // call on app start
    void stopGracefully();         // call on app quit

    bool isReady() const { return m_ready; }
    void setReady(bool ready) { 
        if (m_ready != ready) {
            m_ready = ready; 
            emit readyChanged(ready);
            if (ready) {
                m_health.start();
            } else {
                m_health.stop();
            }
        }
    }
    int  height()  const { return m_height; }
    QString rpcUrl() const;
    QString wsUrl() const;
    QString cookiePath() const;
    QString nodeInfoPath() const { return nodeinfoPath_; }
    
    // Public methods for GUI access
    bool pingOnce();               // quick RPC ping; returns true if 200 + OK
    bool hasCookie() const;
    bool readNodeInfo();           // Read actual ports from nodeinfo.json

signals:
    void readyChanged(bool);
    void heightChanged(int);
    void statusMessage(const QString&);
    void fatalError(const QString&);
    void daemonReady(int rpcPort, int wsPort);  // ✅ NEW: Single emission when daemon is ready

private slots:
    void onDaemonExited(int code, QProcess::ExitStatus status);
    void onDaemonError(QProcess::ProcessError error);
    void healthTick();
    
private:
    // ✅ NEW: Bullet-proof two-phase spawning system
    void startAuto();                           // Phase 1: -rpcport=0 -wsport=0 (auto-select)
    void startLive(int rpcPort, int wsPort);    // Phase 2: Use selected ports
    void wireProcessLogging();                  // Comprehensive process logging
    
    // ✅ NEW: Async RPC infrastructure (battle-tested)
    void rpcCallAsync(const QString& method,
                      const QJsonArray& params,
                      std::function<void(int http, QByteArray body, QNetworkReply::NetworkError err)> cb);
    void probeRpc();
    void scheduleProbeRetry(int backoffMs);
    
    // Re-entry guard
    struct EnsuringGuard { 
        bool& f; 
        explicit EnsuringGuard(bool& flag) : f(flag) { 
            if (f) return; 
            f = true; 
        } 
        ~EnsuringGuard() { f = false; } 
    };

private:
    NodeConfig m_cfg;
    QTimer     m_health;
    bool       m_ready = false;
    int        m_height = 0;
    
    // ✅ NEW: Bullet-proof daemon spawning state
    Phase phase_ = Phase::Idle;
    std::unique_ptr<QProcess> proc_;
    QString dinerodPath_, datadir_, nodeinfoPath_, cookiePath_;
    int rpcPort_ = 0, wsPort_ = 0;
    bool expectingSelectOnly_ = true;   // flip to false if daemon now stays running
    
    // ✅ NEW: Async RPC infrastructure
    QNetworkAccessManager nam_;
    bool probing_ = false;
    int backoffMs_ = 150;
    QTimer probeTimer_;
    
    // ✅ NEW: Nodeinfo.json monitoring
    QFileSystemWatcher nodeinfoWatcher_;
    
    void cleanupNodeInfo();  // Clean up nodeinfo.json file
    void createStableSymlink();  // Create stable symlink for shell scripts
};
