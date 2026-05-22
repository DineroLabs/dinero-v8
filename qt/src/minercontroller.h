#pragma once
#include <QObject>
#include <QTimer>
#include <memory>

// Forward declaration - avoid including solo_miner headers in Qt header
namespace dinero { namespace solo { class SoloMiner; struct MinerConfig; struct MinerStats; struct BlockFoundInfo; } }

/**
 * MinerController - Qt wrapper for solo mining
 *
 * Uses dinero-solo-miner library for direct RPC mining (no external process).
 * Exposes properties and signals for QML/Qt Widgets binding.
 */
class MinerController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(double hashrate READ hashrate NOTIFY statsChanged)
    Q_PROPERTY(int accepted READ accepted NOTIFY statsChanged)
    Q_PROPERTY(int rejected READ rejected NOTIFY statsChanged)
    Q_PROPERTY(int currentHeight READ currentHeight NOTIFY statsChanged)

public:
    explicit MinerController(QObject* parent = nullptr);
    ~MinerController() override;

    /**
     * Start mining
     * @param rpcUrl       RPC endpoint (e.g., "http://127.0.0.1:20998")
     * @param cookiePath   Path to .cookie file for auth (or empty for user/pass)
     * @param payoutAddr   Payout address (required)
     * @param threads      Number of mining threads (0 = auto)
     */
    Q_INVOKABLE void start(const QString& rpcUrl,
                           const QString& cookiePath,
                           const QString& payoutAddr,
                           int threads = 0,
                           bool useGpu = false);

    /**
     * Start mining (legacy signature for compatibility)
     */
    Q_INVOKABLE void start(const QString& minerPath,
                           const QString& rpcUrl,
                           const QString& dataDir,
                           const QString& payoutAddr,
                           int threads);

    /**
     * Stop mining
     */
    Q_INVOKABLE void stop();

    /**
     * Stop mining during owner destruction without emitting UI signals.
     */
    void shutdownSilently();

    // Property getters
    bool running() const;
    QString status() const { return status_; }
    double hashrate() const { return hashrate_; }
    int accepted() const { return accepted_; }
    int rejected() const { return rejected_; }
    int currentHeight() const { return currentHeight_; }

Q_SIGNALS:
    void runningChanged();
    void statusChanged();
    void statsChanged();
    void logLine(const QString& line);
    void blockFound(const QString& hash, int height);

    // Fired when the embedded miner starts (true) or stops (false). Lets the
    // owning UI forward the state to the daemon via mining.setrelayactive so
    // that p2p.relay=auto advertises NODE_RELAY while embedded mining runs.
    void miningRelayStateRequested(bool active);

private Q_SLOTS:
    void updateStats();

private:
    void onHashrate(double hashrate);
    void onBlockFound(const dinero::solo::BlockFoundInfo& info);
    void onError(const std::string& error);
    void onTemplate(uint32_t height, uint32_t difficulty);

    std::unique_ptr<dinero::solo::SoloMiner> miner_;
    QTimer statsTimer_;

    QString status_ = "Stopped";
    double hashrate_ = 0.0;
    int accepted_ = 0;
    int rejected_ = 0;
    int currentHeight_ = 0;
};
