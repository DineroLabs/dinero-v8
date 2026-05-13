#pragma once

#include <QObject>
#include <QProcess>
#include <QJsonObject>
#include <QJsonArray>

class ClaudeProcess : public QObject {
    Q_OBJECT
public:
    explicit ClaudeProcess(QObject* parent = nullptr);
    ~ClaudeProcess();

    void sendMessage(const QString& userText);
    void cancel();
    void clearSession();

    bool isClaudeInstalled() const;
    QString claudeVersion() const;
    bool isRunning() const;

    void setClaudePath(const QString& path);
    void setSystemPrompt(const QString& prompt);
    void setMaxTurns(int turns);

    QString sessionId() const { return sessionId_; }

Q_SIGNALS:
    void textDelta(const QString& delta);
    void turnComplete(const QString& fullText);
    void errorOccurred(const QString& error);
    void streamingStarted();
    void streamingFinished();
    void toolActivity(const QString& toolName, const QString& input);
    void costUpdate(double totalCostUsd);

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
    void handleSystemMessage(const QJsonObject& obj);
    QString findClaudeBinary() const;

    QProcess* process_ = nullptr;
    QByteArray lineBuffer_;
    QString claudePath_;
    QString systemPrompt_;
    QString sessionId_;
    QString currentText_;
    int maxTurns_ = 25;
    bool running_ = false;
};
