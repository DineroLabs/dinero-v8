#pragma once

#include <QWidget>
#include <QLabel>
#include <QPropertyAnimation>
#include <QGraphicsOpacityEffect>
#include <QTimer>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFrame>
#include <QEasingCurve>

class CelebrationEffects;

class AnimatedBalanceWidget : public QWidget {
    Q_OBJECT
    Q_PROPERTY(double balance READ getBalance WRITE setBalance)

public:
    explicit AnimatedBalanceWidget(QWidget* parent = nullptr);
    
    // Balance management
    void updateBalance(double newBalance, bool animate = true);
    void updateUnconfirmedBalance(double unconfirmed);
    void updateImmatureBalance(double immature);
    
    // Visual effects
    void celebrateIncrease(double amount);
    void showBalanceChange(double oldBalance, double newBalance);
    
    // Property animation support
    double getBalance() const { return m_currentBalance; }
    void setBalance(double balance);

signals:
    void balanceClicked();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private slots:
    void onAnimationFinished();
    void onCelebrationTimer();

private:
    void setupUI();
    void startGlowEffect();
    void startCelebrationEffect();
    void updateBalanceDisplay();
    void formatBalance(double balance, QLabel* label, const QString& suffix = "");
    
    // UI Components
    QVBoxLayout* m_mainLayout;
    QFrame* m_balanceFrame;
    QLabel* m_confirmedLabel;
    QLabel* m_confirmedValue;
    QLabel* m_unconfirmedLabel;
    QLabel* m_unconfirmedValue;
    QLabel* m_immatureLabel;
    QLabel* m_immatureValue;
    QLabel* m_changeIndicator;
    
    // Animation components
    QPropertyAnimation* m_balanceAnimation;
    QPropertyAnimation* m_glowAnimation;
    QPropertyAnimation* m_celebrationAnimation;
    QGraphicsOpacityEffect* m_glowEffect;
    QGraphicsOpacityEffect* m_celebrationEffect;
    QTimer* m_celebrationTimer;
    
    // Epic celebration effects
    CelebrationEffects* m_celebrationEffects;
    
    // State
    double m_currentBalance = 0.0;
    double m_targetBalance = 0.0;
    double m_unconfirmedBalance = 0.0;
    double m_immatureBalance = 0.0;
    double m_lastBalance = 0.0;
    bool m_isAnimating = false;
    bool m_isCelebrating = false;
    
    // Visual settings
    static constexpr int ANIMATION_DURATION = 800;
    static constexpr int CELEBRATION_DURATION = 2000;
    static constexpr int GLOW_DURATION = 1000;
};
