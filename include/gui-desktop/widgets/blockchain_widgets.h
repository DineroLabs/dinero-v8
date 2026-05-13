#pragma once

#include <QWidget>
#include <QLabel>
#include <QProgressBar>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFrame>
#include <QPainter>
#include <QTimer>
#include <QPropertyAnimation>
#include <QGraphicsDropShadowEffect>
#include <QDateTime>
#include <QFontMetrics>
#include "gui-desktop/animations/smooth_transitions.h"

namespace dinero::widgets {

/**
 * Modern Card Widget
 * Base class for all dashboard cards with consistent styling
 */
class ModernCard : public QFrame {
    Q_OBJECT

public:
    explicit ModernCard(const QString& title = "", QWidget* parent = nullptr);

    void setTitle(const QString& title);
    void setSubtitle(const QString& subtitle);
    void setIcon(const QIcon& icon);
    void setCardColor(const QColor& color);
    
    QWidget* contentWidget() const { return m_contentWidget; }
    
    void setLoading(bool loading);
    void setError(const QString& errorMessage);
    void clearError();

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    void setupUI();
    void updateShadow();

    QLabel* m_titleLabel;
    QLabel* m_subtitleLabel;
    QLabel* m_iconLabel;
    QLabel* m_errorLabel;
    QWidget* m_contentWidget;
    QVBoxLayout* m_mainLayout;
    QHBoxLayout* m_headerLayout;
    
    QGraphicsDropShadowEffect* m_shadowEffect;
    QPropertyAnimation* m_shadowAnimation;
    
    bool m_isHovered = false;
    bool m_isLoading = false;
    QColor m_cardColor = QColor(255, 255, 255);
};

/**
 * Network Status Widget
 * Displays current network with animated status indicator
 */
class NetworkStatusWidget : public ModernCard {
    Q_OBJECT

public:
    enum class NetworkType { Mainnet, Testnet, Regtest, Offline };
    enum class ConnectionStatus { Connected, Connecting, Disconnected, Syncing };

    explicit NetworkStatusWidget(QWidget* parent = nullptr);

    void setNetwork(NetworkType network);
    void setConnectionStatus(ConnectionStatus status);
    void setBlockHeight(int height);
    void setSyncProgress(double progress);

signals:
    void networkSwitchRequested(NetworkType network);

protected:
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private slots:
    void updateStatusIndicator();
    void animateNetworkChange();

private:
    void setupUI();
    QString networkName(NetworkType network) const;
    QColor networkColor(NetworkType network) const;
    
    NetworkType m_currentNetwork = NetworkType::Regtest;
    ConnectionStatus m_connectionStatus = ConnectionStatus::Disconnected;
    
    QLabel* m_statusIndicator;
    QLabel* m_networkLabel;
    QLabel* m_heightLabel;
    QProgressBar* m_syncProgress;
    
    QTimer* m_blinkTimer;
    bool m_indicatorVisible = true;
    
    dinero::ui::NetworkStatusAnimator* m_animator;
};

/**
 * Blockchain Info Widget
 * Displays key blockchain metrics with live updates
 */
class BlockchainInfoWidget : public ModernCard {
    Q_OBJECT

public:
    explicit BlockchainInfoWidget(QWidget* parent = nullptr);

    void setBestBlockHash(const QString& hash);
    void setChainwork(const QString& chainwork);
    void setDifficulty(double difficulty);
    void setDifficultyString(const QString& difficultyStr);
    void setBlockCount(int count);
    void setUptime(int seconds);

private slots:
    void animateValueChange();

private:
    void setupUI();
    void setupMetricsGrid();
    QString formatUptime(int seconds) const;
    QString formatHash(const QString& hash, int maxLength = 12) const;
    
    QGridLayout* m_metricsLayout;
    
    QLabel* m_hashLabel;
    QLabel* m_chainworkLabel;
    QLabel* m_difficultyLabel;
    QLabel* m_blockCountLabel;
    QLabel* m_uptimeLabel;
    
    QString m_previousHash;
    int m_previousBlockCount = -1;
    
    dinero::ui::DataUpdateAnimator* m_updateAnimator;
};

/**
 * Block List Widget
 * Displays recent blocks with expandable details
 */
class BlockListWidget : public ModernCard {
    Q_OBJECT

public:
    struct BlockInfo {
        int height;
        QString hash;
        QDateTime timestamp;
        int transactionCount;
        QString difficulty;
        int size;
    };

    explicit BlockListWidget(QWidget* parent = nullptr);

    void addBlock(const BlockInfo& block);
    void updateBlock(int height, const BlockInfo& block);
    void clearBlocks();
    void setMaxBlocks(int maxBlocks);

signals:
    void blockSelected(int height, const QString& hash);
    void blockDetailsRequested(int height);

private slots:
    void onBlockItemClicked();
    void refreshBlockList();

private:
    void setupUI();
    QWidget* createBlockItem(const BlockInfo& block);
    void animateNewBlock(QWidget* blockWidget);
    
    QVBoxLayout* m_blockLayout;
    QScrollArea* m_scrollArea;
    QWidget* m_scrollContent;
    
    QList<BlockInfo> m_blocks;
    int m_maxBlocks = 10;
    
    QTimer* m_refreshTimer;
};

/**
 * Mempool Status Widget
 * Shows mempool statistics with visual indicators
 */
class MempoolStatusWidget : public ModernCard {
    Q_OBJECT

public:
    explicit MempoolStatusWidget(QWidget* parent = nullptr);

    void setTransactionCount(int count);
    void setMempoolSize(qint64 bytes);
    void setMemoryUsage(qint64 usage);
    void setFeeRate(double feeRate);

private slots:
    void updateMempoolVisuals();

private:
    void setupUI();
    QString formatBytes(qint64 bytes) const;
    QColor getMempoolColor(int transactionCount) const;
    
    QLabel* m_countLabel;
    QLabel* m_sizeLabel;
    QLabel* m_usageLabel;
    QLabel* m_feeRateLabel;
    
    QProgressBar* m_usageProgress;
    
    int m_transactionCount = 0;
    qint64 m_mempoolSize = 0;
    qint64 m_memoryUsage = 0;
    
    QTimer* m_updateTimer;
};

/**
 * Hash Display Widget
 * Specialized widget for displaying blockchain hashes with copy functionality
 */
class HashDisplayWidget : public QWidget {
    Q_OBJECT

public:
    explicit HashDisplayWidget(QWidget* parent = nullptr);

    void setHash(const QString& hash);
    void setHashType(const QString& type); // "Block", "Transaction", "Address", etc.
    void setTruncateLength(int length);
    void setShowCopyButton(bool show);

signals:
    void hashCopied(const QString& hash);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private slots:
    void copyToClipboard();
    void showCopyConfirmation();

private:
    void setupUI();
    QString truncatedHash() const;
    
    QString m_fullHash;
    QString m_hashType = "Hash";
    int m_truncateLength = 12;
    bool m_showCopyButton = true;
    bool m_isHovered = false;
    
    QLabel* m_hashLabel;
    QPushButton* m_copyButton;
    QHBoxLayout* m_layout;
    
    QTimer* m_copyConfirmTimer;
};

/**
 * Difficulty Chart Widget
 * Mini chart showing difficulty changes over time
 */
class DifficultyChartWidget : public QWidget {
    Q_OBJECT

public:
    explicit DifficultyChartWidget(QWidget* parent = nullptr);

    void addDifficultyPoint(double difficulty, const QDateTime& timestamp);
    void setMaxDataPoints(int maxPoints);
    void clearData();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    struct DifficultyPoint {
        double difficulty;
        QDateTime timestamp;
    };
    
    void updateChart();
    QPointF mapToWidget(const DifficultyPoint& point, const QRectF& chartRect) const;
    
    QList<DifficultyPoint> m_dataPoints;
    int m_maxDataPoints = 50;
    
    double m_minDifficulty = 0.0;
    double m_maxDifficulty = 1.0;
    
    QColor m_lineColor = QColor(0, 102, 204);
    QColor m_fillColor = QColor(0, 102, 204, 50);
    QColor m_gridColor = QColor(200, 200, 200);
};

/**
 * Status Indicator Widget
 * Animated status dot with customizable colors and patterns
 */
class StatusIndicatorWidget : public QWidget {
    Q_OBJECT

public:
    enum class Status { Online, Offline, Syncing, Warning, Error };
    enum class Animation { None, Pulse, Blink, Rotate };

    explicit StatusIndicatorWidget(QWidget* parent = nullptr);

    void setStatus(Status status);
    void setAnimation(Animation animation);
    void setSize(int size);
    void setCustomColor(const QColor& color);

protected:
    void paintEvent(QPaintEvent* event) override;

private slots:
    void updateAnimation();

private:
    void setupAnimation();
    QColor getStatusColor() const;
    
    Status m_status = Status::Offline;
    Animation m_animation = Animation::None;
    int m_size = 12;
    QColor m_customColor;
    
    QTimer* m_animationTimer;
    double m_animationValue = 0.0;
    bool m_animationDirection = true;
};

} // namespace dinero::widgets
