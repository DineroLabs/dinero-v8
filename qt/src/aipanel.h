#pragma once

#include "aibackend.h"
#include <QWidget>
#include <QPropertyAnimation>
#include <QProcess>
#include <QNetworkAccessManager>
#include <QNetworkReply>

class QTextEdit;
class QLineEdit;
class QPushButton;
class QLabel;
class QVBoxLayout;
class QRadioButton;
class QStackedWidget;

class AiPanel : public QWidget {
    Q_OBJECT
    Q_PROPERTY(int panelWidth READ panelWidth WRITE setPanelWidth)

public:
    explicit AiPanel(const QString& datadirHint, QWidget* parent = nullptr);

    int  panelWidth() const;
    void setPanelWidth(int w);

    void togglePanel();
    bool isPanelOpen() const { return panelOpen_; }

Q_SIGNALS:
    void panelToggled(bool open);

private Q_SLOTS:
    void onSendClicked();
    void onTextDelta(const QString& delta);
    void onTurnComplete(const QString& fullText);
    void onError(const QString& error);
    void onStreamingStarted();
    void onStreamingFinished();
    void onToolActivity(const QString& toolName, const QString& input);
    void onCostUpdate(double totalCostUsd);
    void onStartClicked();
    void onChipClicked(const QString& text);
    void onVoiceClicked();
    void onVoiceFinished(int exitCode, QProcess::ExitStatus status);
    void onSaveConfig();
    void onProviderChanged();
    void onAttachClicked();

private:
    void setupUI();
    void buildConfigScreen();
    void buildChatScreen();
    void applyBackend();

    void appendUserMessage(const QString& text);
    void startAssistantMessage();
    void finalizeAssistantMessage();
    void appendToolMessage(const QString& text);
    void appendErrorMessage(const QString& error);
    void appendSystemMessage(const QString& text);
    void addSuggestedChips();
    void scrollToBottom();
    void rebuildChatHtml();
    QString markdownToHtml(const QString& md) const;
    QString systemPrompt() const;
    void saveConversation();
    void loadConversation();

    // Backend
    AiBackend* backend_ = nullptr;
    QString    datadirHint_;

    // Screens
    QStackedWidget* stack_       = nullptr;
    QWidget*        configScreen_= nullptr;
    QWidget*        chatScreen_  = nullptr;

    // wDIN gate
    void    checkWdinBalance(const QString& address);
    void    onWdinCheckDone(quint64 balance);

    QNetworkAccessManager* netMgr_        = nullptr;
    QWidget*  wdinRow_                    = nullptr;
    QLineEdit* baseAddressEdit_           = nullptr;
    QPushButton* saveBtn_                 = nullptr;   // kept to disable during check

    // Config screen widgets
    QRadioButton* rbCli_    = nullptr;
    QRadioButton* rbClaude_ = nullptr;
    QRadioButton* rbGemini_ = nullptr;
    QRadioButton* rbGroq_   = nullptr;
    QRadioButton* rbOllama_ = nullptr;
    QRadioButton* rbMlx_    = nullptr;
    QWidget*      keyRow_   = nullptr;
    QWidget*      pathRow_  = nullptr;
    QWidget*      ollamaRow_= nullptr;
    QLineEdit*    apiKeyEdit_    = nullptr;
    QLineEdit*    claudePathEdit_= nullptr;
    QLineEdit*    ollamaUrlEdit_ = nullptr;
    QLineEdit*    ollamaModelEdit_= nullptr;
    QLabel*       configHint_   = nullptr;

    // Chat screen widgets
    QTextEdit*   chatDisplay_ = nullptr;
    QLineEdit*   inputField_  = nullptr;
    QPushButton* sendButton_   = nullptr;
    QPushButton* voiceButton_  = nullptr;
    QPushButton* attachButton_ = nullptr;
    QWidget*     imagePreviewStrip_ = nullptr;
    QLabel*      imageThumb_   = nullptr;
    QLabel*      imageFilename_= nullptr;
    QWidget*     chipsRow_     = nullptr;
    QLabel*      costLabel_   = nullptr;
    QLabel*      providerLabel_ = nullptr;
    QProcess*    voiceProcess_= nullptr;
    bool         isListening_ = false;

    // Animation
    QPropertyAnimation* slideAnim_ = nullptr;
    bool panelOpen_  = false;
    int  targetWidth_= 420;

    // Conversation
    struct ChatEntry {
        enum Type { User, Assistant, Tool, Error, System };
        Type    type;
        QString text;
        QString imageBase64; // non-empty = user message had image attached
    };
    QList<ChatEntry> chatLog_;
    QString          streamingText_;
    bool             isStreaming_ = false;
    double           sessionCost_ = 0.0;

    // Pending image attachment
    QString pendingImageBase64_;
    QString pendingImageMime_;
    QString pendingImagePath_;
};
