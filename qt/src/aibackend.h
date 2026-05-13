#pragma once

#include <QObject>
#include <QString>

class RpcClient;

// Abstract base for all AI backends.
// All implementations emit the same signals so AiPanel is backend-agnostic.
class AiBackend : public QObject {
    Q_OBJECT
public:
    enum class Provider {
        ClaudeCli,  // claude -p subprocess — user's own install
        ClaudeApi,  // Anthropic Messages API — user's own key
        Gemini,     // Google Gemini API — free tier available
        Groq,       // Groq API — free tier, llama models
        Ollama,     // Local Ollama — free, no account needed
        Mlx         // Apple MLX — native Apple Silicon, free, no account needed
    };
    Q_ENUM(Provider)

    struct Config {
        Provider provider    = Provider::Gemini;
        QString  apiKey;
        QString  claudePath;                              // ClaudeCli: path to binary
        QString  ollamaModel = QStringLiteral("llama3.2");
        QString  ollamaUrl   = QStringLiteral("http://localhost:11434");
        QString  baseAddress;                             // Base chain address for wDIN gate

        static Config load();
        void           save() const;
        bool           isConfigured() const;
        QString        providerLabel() const;
    };

    explicit AiBackend(QObject* parent = nullptr) : QObject(parent) {}

    // Factory: creates the right subclass for cfg.provider
    static AiBackend* create(const Config& cfg, const QString& datadirHint,
                             QObject* parent = nullptr);

    virtual void    sendMessage(const QString& userText) = 0;
    virtual void    cancel()                             = 0;
    virtual void    clearSession()                       = 0;
    virtual bool    isReady()                      const = 0;
    virtual void    setSystemPrompt(const QString& p)    = 0;
    virtual QString sessionId()                    const { return {}; }

    // Attach an image for the next sendMessage() call (vision providers only).
    // base64: raw base64-encoded image data. mimeType: "image/jpeg" or "image/png".
    // Automatically cleared after the next sendMessage().
    virtual void setAttachedImage(const QString& /*base64*/, const QString& /*mimeType*/) {}
    virtual void clearAttachedImage() {}

Q_SIGNALS:
    void textDelta(const QString& delta);
    void turnComplete(const QString& fullText);
    void errorOccurred(const QString& error);
    void streamingStarted();
    void streamingFinished();
    void toolActivity(const QString& toolName, const QString& input);
    void costUpdate(double totalCostUsd);
};
