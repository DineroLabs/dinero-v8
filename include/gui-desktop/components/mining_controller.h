#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QJsonObject>
#include <memory>
#include <thread>

class RpcClient;

/**
 * @brief Qt wrapper for the new mining.* RPC methods
 * 
 * Provides a clean Qt interface for the "done-right" mining system:
 * - No time limits - mining runs until stopped
 * - Smart CPU defaults (Auto 80% cores)
 * - Real-time status updates
 * - Battery/thermal awareness
 * - Network safety gating
 */
class MiningController : public QObject {
    Q_OBJECT

public:
    struct MiningConfig {
        QString address;
        QString threads = "auto";  // "auto" or numeric string
        double throttle = 0.35;    // 0.15-0.90
        bool lowPowerMode = true;
        bool iUnderstand = false;  // Required for testnet/mainnet
    };

    struct MiningStatus {
        bool running = false;
        int threads = 0;
        double throttle = 0.0;
        double hashrateHps = 0.0;
        int blocksFound = 0;
        qint64 lastSubmitTs = 0;
        QString pauseReason;       // null, "on_battery", "thermal", "user_paused"
        QString network;
    };

    explicit MiningController(QObject* parent = nullptr);
    
    // Configuration
    void setRpcClient(std::shared_ptr<RpcClient> client);
    void setNetwork(const QString& network);
    
    // CPU Detection
    static unsigned detectCpuCores();
    static unsigned suggestedThreadsAuto();
    static unsigned clampThreads(unsigned requested);
    static double clampThrottle(double throttle);
    
    // Mining Control
    void startMining(const MiningConfig& config);
    void stopMining();
    void requestStatus();
    
    // Auto-refresh
    void setStatusUpdateInterval(int ms = 2000);
    void startStatusUpdates();
    void stopStatusUpdates();
    
    // Current state
    const MiningStatus& currentStatus() const { return m_status; }
    bool isRunning() const { return m_status.running; }
    bool isRegtest() const { return m_network == "regtest"; }

signals:
    void miningStarted(const MiningStatus& status);
    void miningStopped(const QString& reason);
    void statusUpdated(const MiningStatus& status);
    void errorOccurred(const QString& error);

private slots:
    void onStartResponse(const QJsonObject& response);
    void onStopResponse(const QJsonObject& response);
    void onStatusResponse(const QJsonObject& response);
    void onRpcError(const QString& error);
    void updateStatus();

private:
    void updateStatusFromJson(const QJsonObject& json);
    
    std::shared_ptr<RpcClient> m_rpcClient;
    QTimer* m_statusTimer;
    MiningStatus m_status;
    QString m_network = "regtest";
};

/**
 * @brief CPU detection utilities (header-only for easy reuse)
 */
namespace MiningDefaults {
    inline unsigned detectCpuCores() {
        unsigned n = std::thread::hardware_concurrency();
        if (n == 0) n = 2; // conservative fallback
        return n;
    }

    inline unsigned suggestedThreadsAuto() {
        const unsigned n = detectCpuCores();
        const unsigned cap = (n > 1) ? n - 1 : 1;                // never starve UI
        const unsigned auto80 = std::max(1u, (unsigned)std::floor(n * 0.80));
        return std::clamp(auto80, 1u, cap);
    }

    inline unsigned clampThreads(unsigned req) {
        const unsigned n = detectCpuCores();
        const unsigned cap = (n > 1) ? n - 1 : 1;
        return std::clamp(req, 1u, cap);
    }

    inline double clampThrottle(double t) {
        if (!std::isfinite(t)) t = 0.35;
        return std::clamp(t, 0.15, 0.90);
    }
}
