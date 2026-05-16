#include "minercontroller.h"
#include <solo_miner/miner.h>
#include <QDir>

using namespace dinero::solo;

MinerController::MinerController(QObject* parent)
    : QObject(parent)
{
    // Periodic stats update (every 1 second)
    connect(&statsTimer_, &QTimer::timeout, this, &MinerController::updateStats);
}

MinerController::~MinerController() {
    shutdownSilently();
}

bool MinerController::running() const {
    return miner_ && miner_->isRunning();
}

void MinerController::start(const QString& rpcUrl,
                            const QString& cookiePath,
                            const QString& payoutAddr,
                            int threads,
                            bool useGpu) {
    if (running()) {
        return;
    }

    // Validate address
    if (payoutAddr.isEmpty()) {
        status_ = "Error: Payout address required";
        Q_EMIT statusChanged();
        Q_EMIT logLine(status_);
        return;
    }

    // Create miner instance
    miner_ = std::make_unique<SoloMiner>();

    // Set up callbacks (must capture 'this' safely)
    miner_->setHashrateCallback([this](double hr) {
        QMetaObject::invokeMethod(this, [this, hr]() {
            onHashrate(hr);
        }, Qt::QueuedConnection);
    });

    miner_->setBlockFoundCallback([this](const BlockFoundInfo& info) {
        auto copy = info;  // copy for cross-thread safety
        QMetaObject::invokeMethod(this, [this, copy]() {
            onBlockFound(copy);
        }, Qt::QueuedConnection);
    });

    miner_->setErrorCallback([this](const std::string& error) {
        QMetaObject::invokeMethod(this, [this, error]() {
            onError(error);
        }, Qt::QueuedConnection);
    });

    miner_->setTemplateCallback([this](uint32_t height, uint32_t diff) {
        QMetaObject::invokeMethod(this, [this, height, diff]() {
            onTemplate(height, diff);
        }, Qt::QueuedConnection);
    });

    // Configure miner
    MinerConfig config;
    config.rpc_url = rpcUrl.toStdString();
    config.cookie_path = cookiePath.toStdString();
    config.payout_address = payoutAddr.toStdString();
    config.threads = threads;
    config.backend = useGpu ? MinerBackend::Auto : MinerBackend::Cpu;
    // Use the library default (500ms). The previous explicit 5000ms here
    // overrode the library default and caused Mac to waste up to 5 full
    // seconds hashing stale work every time a competing miner's block was
    // accepted on the network. See dinero-solo-miner 628abf9 for the
    // motivation and dinero p2p-fix 28c6ea069 for the related chainstate
    // fix that surfaced this behavior.

    // Reset stats
    hashrate_ = 0.0;
    accepted_ = 0;
    rejected_ = 0;
    currentHeight_ = 0;
    Q_EMIT statsChanged();

    status_ = "Starting miner...";
    Q_EMIT statusChanged();
    Q_EMIT logLine(status_);

    // Start mining
    if (miner_->start(config)) {
        const QString activeBackend =
            QString::fromStdString(minerBackendToString(miner_->getStats().active_backend));
        const bool activeGpu = activeBackend != QStringLiteral("cpu");
        status_ = useGpu && activeGpu ? "GPU mining..." : "Mining...";
        Q_EMIT statusChanged();
        Q_EMIT runningChanged();
        if (useGpu && activeGpu) {
            Q_EMIT logLine(QStringLiteral("GPU miner started successfully (%1)").arg(activeBackend));
        } else if (useGpu) {
            Q_EMIT logLine(QStringLiteral("No GPU backend available; CPU miner started"));
        } else {
            Q_EMIT logLine(QStringLiteral("Miner started successfully"));
        }

        // Start stats timer
        statsTimer_.start(1000);
    } else {
        QString error = QString::fromStdString(miner_->getLastError());
        status_ = "Failed to start: " + error;
        Q_EMIT statusChanged();
        Q_EMIT logLine(status_);
        miner_.reset();
    }
}

// Legacy signature for compatibility with existing QML/code
void MinerController::start(const QString& /*minerPath*/,
                            const QString& rpcUrl,
                            const QString& dataDir,
                            const QString& payoutAddr,
                            int threads) {
    // Build cookie path from dataDir
    QString cookiePath;
    if (!dataDir.isEmpty()) {
        // Try regtest first, then mainnet
        QDir dir(dataDir);
        if (dir.exists("regtest/.cookie")) {
            cookiePath = dir.filePath("regtest/.cookie");
        } else if (dir.exists(".cookie")) {
            cookiePath = dir.filePath(".cookie");
        }
    }

    start(rpcUrl, cookiePath, payoutAddr, threads);
}

void MinerController::stop() {
    if (!miner_) {
        return;
    }

    statsTimer_.stop();

    status_ = "Stopping...";
    Q_EMIT statusChanged();
    Q_EMIT logLine("Stopping miner...");

    miner_->stop();
    miner_.reset();

    status_ = "Stopped";
    Q_EMIT statusChanged();
    Q_EMIT runningChanged();
    Q_EMIT logLine("Miner stopped");
}

void MinerController::shutdownSilently() {
    blockSignals(true);
    statsTimer_.stop();
    statsTimer_.disconnect();

    if (miner_) {
        miner_->setHashrateCallback({});
        miner_->setBlockFoundCallback({});
        miner_->setErrorCallback({});
        miner_->setTemplateCallback({});
        miner_->stop();
        miner_.reset();
    }

    status_ = "Stopped";
    hashrate_ = 0.0;
    currentHeight_ = 0;
}

void MinerController::updateStats() {
    if (!miner_ || !miner_->isRunning()) {
        return;
    }

    auto stats = miner_->getStats();

    bool changed = false;

    if (hashrate_ != stats.hashrate) {
        hashrate_ = stats.hashrate;
        changed = true;
    }

    if (accepted_ != static_cast<int>(stats.blocks_accepted)) {
        accepted_ = static_cast<int>(stats.blocks_accepted);
        changed = true;
    }

    if (rejected_ != static_cast<int>(stats.blocks_rejected)) {
        rejected_ = static_cast<int>(stats.blocks_rejected);
        changed = true;
    }

    if (currentHeight_ != static_cast<int>(stats.current_height)) {
        currentHeight_ = static_cast<int>(stats.current_height);
        changed = true;
    }

    if (changed) {
        Q_EMIT statsChanged();
    }
}

void MinerController::onHashrate(double hr) {
    hashrate_ = hr;
    Q_EMIT statsChanged();

    // Format for log
    QString formatted;
    if (hr >= 1e9) {
        formatted = QString::number(hr / 1e9, 'f', 2) + " GH/s";
    } else if (hr >= 1e6) {
        formatted = QString::number(hr / 1e6, 'f', 2) + " MH/s";
    } else if (hr >= 1e3) {
        formatted = QString::number(hr / 1e3, 'f', 2) + " KH/s";
    } else {
        formatted = QString::number(hr, 'f', 2) + " H/s";
    }
    Q_EMIT logLine("Hashrate: " + formatted);
}

void MinerController::onBlockFound(const BlockFoundInfo& info) {
    QString hashStr = QString::fromStdString(info.block_hash);
    currentHeight_ = static_cast<int>(info.height);

    Q_EMIT blockFound(hashStr, static_cast<int>(info.height));
    Q_EMIT logLine(QString("🎉 *** BLOCK FOUND ***  height=%1  nonce=%2  difficulty=0x%3")
                   .arg(info.height)
                   .arg(info.nonce)
                   .arg(info.nbits, 8, 16, QChar('0')));
    Q_EMIT logLine(QString("  hash:         %1").arg(hashStr));
    Q_EMIT logLine(QString("  prev_hash:    %1").arg(QString::fromStdString(info.prev_hash)));
    Q_EMIT logLine(QString("  merkle_root:  %1").arg(QString::fromStdString(info.merkle_root)));
    Q_EMIT logLine(QString("  utreexo_root: %1").arg(QString::fromStdString(info.utreexo_root)));
    Q_EMIT logLine(QString("  nBits:        0x%1").arg(info.nbits, 8, 16, QChar('0')));
    Q_EMIT statsChanged();
}

void MinerController::onError(const std::string& error) {
    QString errorStr = QString::fromStdString(error);
    Q_EMIT logLine("❌ Error: " + errorStr);

    const bool daemonDisconnected =
        errorStr.contains("Couldn't connect to server", Qt::CaseInsensitive) ||
        errorStr.contains("Cannot connect to daemon", Qt::CaseInsensitive);
    if (daemonDisconnected && running()) {
        status_ = "Daemon disconnected; mining stopped";
        Q_EMIT statusChanged();
        Q_EMIT logLine("Daemon RPC is unavailable; stopping embedded miner");
        stop();
    }
}

void MinerController::onTemplate(uint32_t height, uint32_t difficulty) {
    currentHeight_ = static_cast<int>(height);
    Q_EMIT statsChanged();
    Q_EMIT logLine(QString("📦 New template: height=%1 difficulty=0x%2")
                   .arg(height)
                   .arg(difficulty, 8, 16, QChar('0')));
}
