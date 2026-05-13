#pragma once

#include "aibackend.h"
#include "aitoolexecutor.h"
#include <QJsonArray>
#include <QJsonObject>

class QNetworkAccessManager;
class QNetworkReply;

// HTTP-based AI backend covering:
//   - Anthropic Messages API  (Claude models, user's own key)
//   - Google Gemini API       (free tier available)
//   - Groq API                (free tier, OpenAI-compatible)
//   - Ollama                  (local, no key needed)
//
// Handles multi-turn tool call loops internally.
class HttpAiBackend : public AiBackend {
    Q_OBJECT
public:
    explicit HttpAiBackend(const Config& cfg, const QString& datadirHint,
                           QObject* parent = nullptr);

    void    sendMessage(const QString& userText) override;
    void    cancel()                             override;
    void    clearSession()                       override;
    bool    isReady()                      const override;
    void    setSystemPrompt(const QString& p)    override { systemPrompt_ = p; }
    void    setAttachedImage(const QString& base64, const QString& mimeType) override;
    void    clearAttachedImage() override;

private:
    // Conversation loop
    void postToApi();
    void onApiReply(QNetworkReply* reply);

    // Provider-specific request building
    QNetworkReply* buildAnthropicRequest();
    QNetworkReply* buildOpenAIRequest();   // Groq + Ollama
    QNetworkReply* buildGeminiRequest();

    // Response parsing — returns true if tool calls were found
    bool parseAnthropicResponse(const QJsonObject& resp);
    bool parseOpenAIResponse(const QJsonObject& resp);
    bool parseGeminiResponse(const QJsonObject& resp);

    // Tool execution
    struct ToolCall {
        QString     id;
        QString     name;
        QJsonObject args;
    };
    void executeTools(const QList<ToolCall>& calls);
    void onAllToolsDone();

    // Message history builders per format
    void appendAnthropicToolResults();
    void appendOpenAIToolResults();
    void appendGeminiToolResults();

    // Helpers
    QString apiUrl() const;
    QUrl    geminiUrl() const;

    Config             cfg_;
    QString            systemPrompt_;
    QNetworkAccessManager* nam_;
    AiToolExecutor*    executor_;
    QNetworkReply*     activeReply_ = nullptr;
    bool               cancelled_   = false;

    // Conversation state
    QJsonArray  messages_;      // Anthropic/OpenAI format
    QJsonArray  geminiContents_;// Gemini format (separate because structure differs)
    QString     currentText_;
    bool        running_ = false;

    // Tool call tracking for current turn
    QList<ToolCall>    pendingTools_;
    QJsonArray         toolResults_;  // accumulated results
    int                toolsDone_ = 0;

    // Pending assistant message (for OpenAI tool_calls field)
    QJsonObject lastAssistantMsg_;
    // Pending function call parts (for Gemini)
    QJsonArray  lastFunctionCallParts_;

    // Attached image for current turn (cleared after first sendMessage)
    QString pendingImageBase64_;
    QString pendingImageMime_;
};
