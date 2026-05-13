#pragma once

#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkReply>
#include <QByteArray>
#include <QList>

class QNetworkAccessManager;

struct AiToolCall {
    QString id;
    QString name;
    QJsonObject input;
};

class AiProvider : public QObject {
    Q_OBJECT
public:
    explicit AiProvider(QObject* parent = nullptr);

    void setApiKey(const QString& key);
    bool hasApiKey() const { return !apiKey_.isEmpty(); }
    void setBackend(const QString& baseUrl, const QString& model);
    bool isLocalMode() const { return localMode_; }
    bool isReady() const { return localMode_ || !apiKey_.isEmpty(); }
    void setSystemPrompt(const QString& prompt);
    void setTools(const QJsonArray& toolDefinitions);

    void clearConversation();
    void sendMessage(const QString& userText);
    void feedToolResults(const QList<AiToolCall>& calls,
                         const QJsonArray& toolResultContents);
    void cancel();
    bool isStreaming() const { return streaming_; }

Q_SIGNALS:
    void textDelta(const QString& delta);
    void textUpdated(const QString& fullText);
    void allToolCallsReady(const QList<AiToolCall>& calls);
    void turnComplete(const QString& fullText);
    void errorOccurred(const QString& error);
    void streamingStarted();
    void streamingFinished();

private Q_SLOTS:
    void onReadyRead();
    void onFinished();

private:
    void postMessages();
    void processEvent(const QString& eventType, const QJsonObject& data);

    QNetworkAccessManager* nam_;
    QNetworkReply* activeReply_ = nullptr;
    QString apiKey_;
    QString baseUrl_ = "http://localhost:11434";
    QString model_ = "qwen3-coder";
    bool localMode_ = true;
    QString systemPrompt_;
    QJsonArray toolDefinitions_;
    QJsonArray conversationHistory_;
    bool streaming_ = false;

    // Current streaming state
    QString currentText_;
    QList<AiToolCall> currentToolCalls_;
    QByteArray sseBuffer_;
    QString currentToolJson_;       // accumulator for partial_json
    QString currentToolId_;
    QString currentToolName_;
    QString currentBlockType_;
};
