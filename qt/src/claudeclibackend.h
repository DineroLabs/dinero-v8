#pragma once

#include "aibackend.h"
#include <QProcess>
#include <QJsonObject>
#include <QJsonArray>

// AI backend that delegates to the claude CLI subprocess.
// Requires claude Code CLI installed — uses the user's own subscription/API key.
class ClaudeCliBackend : public AiBackend {
    Q_OBJECT
public:
    explicit ClaudeCliBackend(const Config& cfg, QObject* parent = nullptr);
    ~ClaudeCliBackend() override;

    void    sendMessage(const QString& userText) override;
    void    cancel()                             override;
    void    clearSession()                       override;
    bool    isReady()                      const override;
    void    setSystemPrompt(const QString& p)    override { systemPrompt_ = p; }
    QString sessionId()                    const override { return sessionId_; }

private Q_SLOTS:
    void onReadyReadStdout();
    void onReadyReadStderr();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onProcessError(QProcess::ProcessError error);

private:
    void processLine(const QByteArray& line);
    void handleStreamEvent(const QJsonObject& obj);
    void handleAssistantMessage(const QJsonObject& obj);
    void handleResult(const QJsonObject& obj);
    QString findClaudeBinary() const;

    QProcess*  process_ = nullptr;
    QByteArray lineBuffer_;
    QString    claudePath_;
    QString    systemPrompt_;
    QString    sessionId_;
    QString    currentText_;
    int        maxTurns_ = 25;
    bool       running_  = false;
};
