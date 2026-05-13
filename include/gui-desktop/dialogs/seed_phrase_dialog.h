#pragma once

#include <QDialog>
#include <QWizard>
#include <QWizardPage>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QTextEdit>
#include <QCheckBox>
#include <QProgressBar>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTimer>
#include <QPropertyAnimation>
#include <QGraphicsOpacityEffect>
#include <QStringList>
#include <QRandomGenerator>

class SeedPhraseDialog : public QWizard {
    Q_OBJECT

public:
    enum Mode {
        BACKUP_SEED,     // Show existing seed phrase for backup
        RESTORE_SEED,    // Enter seed phrase to restore wallet
        VERIFY_SEED      // Verify user wrote down seed phrase correctly
    };

    explicit SeedPhraseDialog(Mode mode, QWidget* parent = nullptr);
    
    // For backup mode - set the seed phrase to display
    void setSeedPhrase(const QStringList& seedWords);
    
    // For restore mode - get the entered seed phrase
    QStringList getSeedPhrase() const;
    
    // Security settings
    void setSecurityLevel(int level) { m_securityLevel = level; } // 1-5 scale
    void setRequireVerification(bool require) { m_requireVerification = require; }

signals:
    void seedPhraseBackedUp();
    void seedPhraseRestored(const QStringList& seedWords);
    void seedPhraseVerified(bool success);

private:
    Mode m_mode;
    QStringList m_seedWords;
    int m_securityLevel = 3;
    bool m_requireVerification = true;
    
    void setupWizardPages();
};

// Individual wizard pages
class SeedIntroPage : public QWizardPage {
    Q_OBJECT
    
public:
    explicit SeedIntroPage(SeedPhraseDialog::Mode mode, QWidget* parent = nullptr);
    
protected:
    void initializePage() override;
    bool isComplete() const override;
    
private slots:
    void onUnderstandingChecked(bool checked);
    
private:
    SeedPhraseDialog::Mode m_mode;
    QLabel* m_titleLabel;
    QLabel* m_descriptionLabel;
    QLabel* m_warningLabel;
    QCheckBox* m_understandCheckbox;
    QCheckBox* m_secureLocationCheckbox;
    QCheckBox* m_noScreenshotCheckbox;
    bool m_userUnderstands = false;
};

class SeedDisplayPage : public QWizardPage {
    Q_OBJECT
    
public:
    explicit SeedDisplayPage(QWidget* parent = nullptr);
    
    void setSeedPhrase(const QStringList& seedWords);
    
protected:
    void initializePage() override;
    bool isComplete() const override;
    void showEvent(QShowEvent* event) override;
    
private slots:
    void onRevealButtonClicked();
    void onCopyButtonClicked();
    void onWrittenDownChecked(bool checked);
    void startSecurityTimer();
    void updateSecurityProgress();
    
private:
    QLabel* m_instructionLabel;
    QPushButton* m_revealButton;
    QPushButton* m_copyButton;
    QWidget* m_seedGrid;
    QCheckBox* m_writtenDownCheckbox;
    QProgressBar* m_securityProgressBar;
    QLabel* m_securityLabel;
    
    QStringList m_seedWords;
    bool m_seedRevealed = false;
    bool m_userConfirmedWritten = false;
    
    QTimer* m_securityTimer;
    int m_securityProgress = 0;
    
    void createSeedGrid();
    void revealSeedWords();
    void hideSeedWords();
    void applySeedStyling(QLabel* label, int index);
};

class SeedInputPage : public QWizardPage {
    Q_OBJECT
    
public:
    explicit SeedInputPage(QWidget* parent = nullptr);
    
    QStringList getSeedPhrase() const;
    
protected:
    void initializePage() override;
    bool isComplete() const override;
    
private slots:
    void onSeedWordChanged();
    void onPasteButtonClicked();
    void onClearButtonClicked();
    void validateSeedPhrase();
    
private:
    QLabel* m_instructionLabel;
    QGridLayout* m_seedInputGrid;
    QList<QLineEdit*> m_seedInputs;
    QPushButton* m_pasteButton;
    QPushButton* m_clearButton;
    QLabel* m_validationLabel;
    
    bool m_seedValid = false;
    
    void createSeedInputGrid();
    void updateValidationStatus();
    bool isValidBIP39Word(const QString& word) const;
    QString suggestCorrection(const QString& word) const;
};

class SeedVerificationPage : public QWizardPage {
    Q_OBJECT
    
public:
    explicit SeedVerificationPage(QWidget* parent = nullptr);
    
    void setSeedPhrase(const QStringList& seedWords);
    
protected:
    void initializePage() override;
    bool isComplete() const override;
    
private slots:
    void onVerificationWordChanged();
    void onRetryButtonClicked();
    
private:
    QLabel* m_instructionLabel;
    QLabel* m_progressLabel;
    QVBoxLayout* m_verificationLayout;
    QPushButton* m_retryButton;
    
    QStringList m_originalSeedWords;
    QList<int> m_testIndices;
    QList<QLineEdit*> m_verificationInputs;
    int m_currentVerificationStep = 0;
    bool m_verificationPassed = false;
    
    void generateVerificationTest();
    void createVerificationInputs();
    void checkVerificationStep();
    void showVerificationResult(bool success);
};

class SeedCompletionPage : public QWizardPage {
    Q_OBJECT
    
public:
    explicit SeedCompletionPage(SeedPhraseDialog::Mode mode, QWidget* parent = nullptr);
    
    void setSuccess(bool success);
    
protected:
    void initializePage() override;
    
private:
    SeedPhraseDialog::Mode m_mode;
    QLabel* m_resultLabel;
    QLabel* m_messageLabel;
    QLabel* m_nextStepsLabel;
    bool m_success = true;
};
