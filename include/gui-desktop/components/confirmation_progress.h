#pragma once

#include <QWidget>
#include <QLabel>
#include <QProgressBar>
#include <QPropertyAnimation>
#include <QGraphicsOpacityEffect>
#include <QTimer>
#include <QPainter>
#include <QColor>

class ConfirmationProgress : public QWidget {
    Q_OBJECT
    Q_PROPERTY(double progress READ getProgress WRITE setProgress)

public:
    enum ConfirmationLevel {
        UNCONFIRMED = 0,    // 0/6 - Red, high risk
        LOW_SECURITY = 1,   // 1/6 - Orange, some risk
        MEDIUM_SECURITY = 3, // 3/6 - Yellow, moderate security
        HIGH_SECURITY = 6,   // 6/6 - Green, fully secure
        COINBASE_IMMATURE = 100 // Special state for immature coinbase
    };

    explicit ConfirmationProgress(QWidget* parent = nullptr);
    
    // Main interface
    void setConfirmations(int confirmations, int required = 6);
    void setCoinbaseConfirmations(int confirmations, int required = 100);
    void setTransactionHash(const QString& txid);
    
    // Visual states
    void setCompactMode(bool compact) { m_compactMode = compact; updateLayout(); }
    void setShowAnimation(bool animate) { m_showAnimation = animate; }
    
    // Getters
    int getConfirmations() const { return m_currentConfirmations; }
    int getRequiredConfirmations() const { return m_requiredConfirmations; }
    bool isFullyConfirmed() const { return m_currentConfirmations >= m_requiredConfirmations; }
    double getProgress() const { return m_progress; }

signals:
    void confirmationChanged(int confirmations);
    void fullyConfirmed();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private slots:
    void onAnimationUpdate();
    void updateSecurityLevel();

private:
    // Core data
    int m_currentConfirmations = 0;
    int m_requiredConfirmations = 6;
    QString m_transactionHash;
    bool m_isCoinbase = false;
    
    // Visual properties
    double m_progress = 0.0;
    double m_targetProgress = 0.0;
    bool m_compactMode = false;
    bool m_showAnimation = true;
    
    // UI Components
    QLabel* m_statusLabel;
    QLabel* m_progressLabel;
    QLabel* m_securityLabel;
    QProgressBar* m_progressBar;
    
    // Animations
    QPropertyAnimation* m_progressAnimation;
    QPropertyAnimation* m_glowAnimation;
    QTimer* m_updateTimer;
    QGraphicsOpacityEffect* m_glowEffect;
    
    // Visual state
    QColor m_currentColor;
    QColor m_targetColor;
    QString m_securityText;
    
    // Helper methods
    void setupUI();
    void setupAnimations();
    void updateLayout();
    void setProgress(double progress);
    
    QColor getColorForConfirmations(int confirmations) const;
    QString getSecurityText(int confirmations) const;
    QString getStatusText(int confirmations) const;
    
    void drawProgressRing(QPainter& painter, const QRect& rect);
    void drawConfirmationDots(QPainter& painter, const QRect& rect);
    void drawSecurityShield(QPainter& painter, const QRect& rect);
    
    // Constants
    static constexpr int ANIMATION_DURATION = 800;
    static constexpr int UPDATE_INTERVAL = 100;
    static constexpr int COMPACT_HEIGHT = 40;
    static constexpr int FULL_HEIGHT = 80;
};
