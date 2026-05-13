#pragma once

#include <QWidget>
#include <QTimer>
#include <QPropertyAnimation>
#include <QSequentialAnimationGroup>
#include <QParallelAnimationGroup>
#include <QGraphicsOpacityEffect>
#include <QGraphicsColorizeEffect>
// #include <QSoundEffect> // Disabled for now - requires Qt6 Multimedia
#include <QLabel>
#include <QVBoxLayout>
#include <QRandomGenerator>
#include <QPainter>
#include <QPixmap>
#include <vector>

class CelebrationEffects : public QWidget {
    Q_OBJECT

public:
    enum CelebrationType {
        SMALL_REWARD,    // < 10 DIN
        MEDIUM_REWARD,   // 10-50 DIN  
        LARGE_REWARD,    // 50-100 DIN
        MINING_JACKPOT,  // >= 99 DIN (full mining reward)
        MILESTONE        // Special achievements
    };

    explicit CelebrationEffects(QWidget* parent = nullptr);
    ~CelebrationEffects();

    // Main celebration triggers
    void celebrateMiningReward(double amount);
    void celebrateReceived(double amount);
    void celebrateMilestone(const QString& message, const QString& achievement);
    
    // Configuration
    void setSoundEnabled(bool enabled) { m_soundEnabled = enabled; }
    void setParticlesEnabled(bool enabled) { m_particlesEnabled = enabled; }
    void setIntensity(int level) { m_intensity = qBound(1, level, 5); } // 1-5 scale

signals:
    void celebrationFinished();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void updateParticles();
    void onCelebrationComplete();
    void onSoundFinished();

private:
    struct Particle {
        QPointF position;
        QPointF velocity;
        double rotation = 0.0;
        double rotationSpeed = 0.0;
        double scale = 1.0;
        double opacity = 1.0;
        double life = 1.0;
        QColor color;
        QString symbol; // 💰, ⭐, 🎉, 💎, etc.
        int type = 0; // 0=coin, 1=star, 2=sparkle, 3=gem
    };

    void setupSounds();
    void startCelebration(CelebrationType type, double amount, const QString& message = "");
    
    // Particle system
    void createParticles(CelebrationType type, int count);
    void updateParticleSystem();
    void drawParticles(QPainter& painter);
    
    // Animation effects
    void startScreenFlash();
    void startBounceEffect();
    void startGlowPulse();
    void startFloatingText(const QString& text);
    
    // Sound effects
    void playSound(CelebrationType type);
    
    // Visual components
    QLabel* m_celebrationText;
    QLabel* m_amountText;
    QLabel* m_achievementText;
    
    // Animation system
    QTimer* m_particleTimer;
    QTimer* m_celebrationTimer;
    QPropertyAnimation* m_flashAnimation;
    QPropertyAnimation* m_bounceAnimation;
    QPropertyAnimation* m_glowAnimation;
    QSequentialAnimationGroup* m_celebrationSequence;
    
    // Effects
    QGraphicsOpacityEffect* m_flashEffect;
    QGraphicsColorizeEffect* m_colorizeEffect;
    
    // Sound system (disabled for now)
    // QSoundEffect* m_coinSound;
    // QSoundEffect* m_jackpotSound;  
    // QSoundEffect* m_chimeSound;
    // QSoundEffect* m_fanfareSound;
    
    // Particle system
    std::vector<Particle> m_particles;
    QRandomGenerator m_random;
    
    // State
    bool m_isActive = false;
    bool m_soundEnabled = true;
    bool m_particlesEnabled = true;
    int m_intensity = 3;
    CelebrationType m_currentType = SMALL_REWARD;
    
    // Timing constants
    static constexpr int PARTICLE_UPDATE_MS = 16; // 60 FPS
    static constexpr int CELEBRATION_DURATION_MS = 3000;
    static constexpr int PARTICLE_COUNT_SMALL = 15;
    static constexpr int PARTICLE_COUNT_MEDIUM = 30;
    static constexpr int PARTICLE_COUNT_LARGE = 50;
    static constexpr int PARTICLE_COUNT_JACKPOT = 100;
};
