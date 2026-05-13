#pragma once

#include <QWidget>
#include <QPropertyAnimation>
#include <QGraphicsOpacityEffect>
#include <QParallelAnimationGroup>
#include <QSequentialAnimationGroup>
#include <QEasingCurve>
#include <QTimer>
#include <QLabel>
#include <QListWidget>
#include <QProgressBar>
#include <functional>

namespace dinero::ui {

/**
 * Modern Animation System for Dinero Desktop
 * Provides smooth, professional transitions for all UI interactions
 */
class SmoothTransitions : public QObject {
    Q_OBJECT

public:
    enum class AnimationType {
        FadeIn,
        FadeOut,
        SlideInLeft,
        SlideInRight,
        SlideUp,
        SlideDown,
        ScaleIn,
        ScaleOut,
        Bounce,
        Elastic,
        NetworkSwitch,
        StatusUpdate,
        DataRefresh
    };

    enum class EasingType {
        Linear,
        EaseInOut,
        EaseIn,
        EaseOut,
        Bounce,
        Elastic,
        Back
    };

    struct AnimationConfig {
        int duration = 300;           // milliseconds
        EasingType easing = EasingType::EaseInOut;
        int delay = 0;               // milliseconds
        bool autoReverse = false;
        std::function<void()> onFinished = nullptr;
    };

    static SmoothTransitions* instance();

    // Core animation methods
    void fadeIn(QWidget* widget, const AnimationConfig& config = {});
    void fadeOut(QWidget* widget, const AnimationConfig& config = {});
    void slideIn(QWidget* widget, AnimationType direction, const AnimationConfig& config = {});
    void slideOut(QWidget* widget, AnimationType direction, const AnimationConfig& config = {});
    void scaleTransition(QWidget* widget, double fromScale, double toScale, const AnimationConfig& config = {});
    void bounceIn(QWidget* widget, const AnimationConfig& config = {});
    
    // Specialized animations for Dinero Desktop
    void networkSwitchAnimation(QWidget* oldWidget, QWidget* newWidget, const AnimationConfig& config = {});
    void statusUpdateAnimation(QWidget* statusWidget, const QString& newStatus, const AnimationConfig& config = {});
    void dataRefreshAnimation(QWidget* dataWidget, const AnimationConfig& config = {});
    void blockchainSyncAnimation(QWidget* syncWidget, double progress, const AnimationConfig& config = {});
    
    // Utility animations
    void pulseEffect(QWidget* widget, int cycles = 3, const AnimationConfig& config = {});
    void shakeEffect(QWidget* widget, int intensity = 10, const AnimationConfig& config = {});
    void glowEffect(QWidget* widget, const QColor& glowColor, const AnimationConfig& config = {});
    
    // Animation groups for complex transitions
    QParallelAnimationGroup* createParallelGroup();
    QSequentialAnimationGroup* createSequentialGroup();
    
    // Global animation settings
    void setGlobalDuration(int milliseconds);
    void setAnimationsEnabled(bool enabled);
    void setReducedMotion(bool reduced); // Accessibility
    
    bool animationsEnabled() const { return m_animationsEnabled; }
    bool reducedMotionEnabled() const { return m_reducedMotion; }

signals:
    void animationStarted(QWidget* widget, AnimationType type);
    void animationFinished(QWidget* widget, AnimationType type);

private:
    explicit SmoothTransitions(QObject* parent = nullptr);
    
    QEasingCurve::Type convertEasingType(EasingType type) const;
    QPropertyAnimation* createAnimation(QObject* target, const QByteArray& property, const AnimationConfig& config);
    void setupOpacityEffect(QWidget* widget);
    
    static SmoothTransitions* s_instance;
    bool m_animationsEnabled = true;
    bool m_reducedMotion = false;
    int m_globalDuration = 300;
    
    QTimer* m_cleanupTimer;
    QList<QPropertyAnimation*> m_activeAnimations;
};

/**
 * Animation Helper Macros for Easy Integration
 */
#define FADE_IN(widget) dinero::ui::SmoothTransitions::instance()->fadeIn(widget)
#define FADE_OUT(widget) dinero::ui::SmoothTransitions::instance()->fadeOut(widget)
#define SLIDE_IN_LEFT(widget) dinero::ui::SmoothTransitions::instance()->slideIn(widget, dinero::ui::SmoothTransitions::AnimationType::SlideInLeft)
#define SLIDE_IN_RIGHT(widget) dinero::ui::SmoothTransitions::instance()->slideIn(widget, dinero::ui::SmoothTransitions::AnimationType::SlideInRight)
#define BOUNCE_IN(widget) dinero::ui::SmoothTransitions::instance()->bounceIn(widget)
#define PULSE(widget) dinero::ui::SmoothTransitions::instance()->pulseEffect(widget)

/**
 * Animated Widget Base Class
 * Provides built-in animation support for custom widgets
 */
class AnimatedWidget : public QWidget {
    Q_OBJECT

public:
    explicit AnimatedWidget(QWidget* parent = nullptr);

    void setEntryAnimation(SmoothTransitions::AnimationType type);
    void setExitAnimation(SmoothTransitions::AnimationType type);
    
    void animateShow();
    void animateHide();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    SmoothTransitions::AnimationType m_entryAnimation = SmoothTransitions::AnimationType::FadeIn;
    SmoothTransitions::AnimationType m_exitAnimation = SmoothTransitions::AnimationType::FadeOut;
    bool m_animateOnShow = true;
    bool m_animateOnHide = true;
};

/**
 * Network Status Animator
 * Specialized animations for network state changes
 */
class NetworkStatusAnimator : public QObject {
    Q_OBJECT

public:
    explicit NetworkStatusAnimator(QWidget* statusWidget, QObject* parent = nullptr);

    void animateNetworkSwitch(const QString& fromNetwork, const QString& toNetwork);
    void animateConnectionStatus(bool connected);
    void animateSyncProgress(double progress);
    void animateBlockUpdate(int newHeight);

signals:
    void switchAnimationFinished();
    void statusAnimationFinished();

private slots:
    void onSwitchPhaseOne();
    void onSwitchPhaseTwo();

private:
    QWidget* m_statusWidget;
    QString m_pendingNetwork;
    QPropertyAnimation* m_currentAnimation = nullptr;
};

/**
 * Data Update Animator
 * Smooth animations for data refreshes and updates
 */
class DataUpdateAnimator : public QObject {
    Q_OBJECT

public:
    explicit DataUpdateAnimator(QWidget* dataWidget, QObject* parent = nullptr);

    void animateDataRefresh();
    void animateValueUpdate(QLabel* label, const QString& oldValue, const QString& newValue);
    void animateListUpdate(QListWidget* list);
    void animateProgressUpdate(QProgressBar* progress, int newValue);

private:
    QWidget* m_dataWidget;
    QGraphicsOpacityEffect* m_opacityEffect = nullptr;
};

} // namespace dinero::ui
