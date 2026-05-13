#include "httpaibackend.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>
#include <QDebug>

// ──────────────────────────────────────────────────────────────
// Construction
// ──────────────────────────────────────────────────────────────

HttpAiBackend::HttpAiBackend(const Config& cfg, const QString& datadirHint, QObject* parent)
    : AiBackend(parent)
    , cfg_(cfg)
    , nam_(new QNetworkAccessManager(this))
    , executor_(new AiToolExecutor(datadirHint, this))
{
}

// ──────────────────────────────────────────────────────────────
// Public interface
// ──────────────────────────────────────────────────────────────

void HttpAiBackend::setAttachedImage(const QString& base64, const QString& mimeType)
{
    pendingImageBase64_ = base64;
    pendingImageMime_   = mimeType;
}

void HttpAiBackend::clearAttachedImage()
{
    pendingImageBase64_.clear();
    pendingImageMime_.clear();
}

bool HttpAiBackend::isReady() const
{
    if (cfg_.provider == Provider::Ollama || cfg_.provider == Provider::Mlx) return true;
    return !cfg_.apiKey.isEmpty();
}

void HttpAiBackend::clearSession()
{
    messages_       = QJsonArray();
    geminiContents_ = QJsonArray();
    currentText_.clear();
    pendingTools_.clear();
    toolResults_    = QJsonArray();
    toolsDone_      = 0;
}

void HttpAiBackend::cancel()
{
    cancelled_ = true;
    if (activeReply_ && activeReply_->isRunning())
        activeReply_->abort();
    running_ = false;
}

void HttpAiBackend::sendMessage(const QString& userText)
{
    if (running_) cancel();
    cancelled_ = false;
    running_   = true;
    currentText_.clear();
    pendingTools_.clear();
    toolResults_  = QJsonArray();
    toolsDone_    = 0;

    // Append user message to history — with optional image attachment
    bool hasImage = !pendingImageBase64_.isEmpty();

    if (cfg_.provider == Provider::Gemini) {
        QJsonArray parts;
        parts.append(QJsonObject{{"text", userText}});
        if (hasImage) {
            parts.append(QJsonObject{{"inline_data", QJsonObject{
                {"mime_type", pendingImageMime_},
                {"data",      pendingImageBase64_}
            }}});
        }
        geminiContents_.append(QJsonObject{{"role","user"},{"parts", parts}});
    } else if (cfg_.provider == Provider::ClaudeApi) {
        if (hasImage) {
            // Anthropic multipart content: image first, then text
            messages_.append(QJsonObject{
                {"role","user"},
                {"content", QJsonArray{
                    QJsonObject{{"type","image"}, {"source", QJsonObject{
                        {"type","base64"},
                        {"media_type", pendingImageMime_},
                        {"data", pendingImageBase64_}
                    }}},
                    QJsonObject{{"type","text"},{"text", userText}}
                }}
            });
        } else {
            messages_.append(QJsonObject{{"role","user"},{"content", userText}});
        }
    } else {
        // OpenAI-compatible (Groq, Ollama, MLX)
        if (hasImage) {
            messages_.append(QJsonObject{
                {"role","user"},
                {"content", QJsonArray{
                    QJsonObject{{"type","text"},{"text", userText}},
                    QJsonObject{{"type","image_url"}, {"image_url", QJsonObject{
                        {"url", QString("data:%1;base64,%2")
                               .arg(pendingImageMime_).arg(pendingImageBase64_)}
                    }}}
                }}
            });
        } else {
            messages_.append(QJsonObject{{"role","user"},{"content", userText}});
        }
    }

    // Image consumed — clear for next turn
    pendingImageBase64_.clear();
    pendingImageMime_.clear();

    Q_EMIT streamingStarted();
    postToApi();
}

// ──────────────────────────────────────────────────────────────
// Conversation loop
// ──────────────────────────────────────────────────────────────

void HttpAiBackend::postToApi()
{
    if (cancelled_) return;

    QNetworkReply* reply = nullptr;
    switch (cfg_.provider) {
    case Provider::ClaudeApi: reply = buildAnthropicRequest(); break;
    case Provider::Gemini:    reply = buildGeminiRequest();    break;
    case Provider::Groq:
    case Provider::Ollama:
    case Provider::Mlx:       reply = buildOpenAIRequest();    break;
    default:                  break;
    }

    if (!reply) {
        Q_EMIT errorOccurred("Backend error: could not build request");
        Q_EMIT streamingFinished();
        running_ = false;
        return;
    }

    activeReply_ = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onApiReply(reply);
    });
}

void HttpAiBackend::onApiReply(QNetworkReply* reply)
{
    reply->deleteLater();
    activeReply_ = nullptr;

    if (cancelled_) { running_ = false; Q_EMIT streamingFinished(); return; }

    if (reply->error() != QNetworkReply::NoError) {
        QByteArray body = reply->readAll();
        QString errMsg = reply->errorString();
        // Try to extract API error message
        auto doc = QJsonDocument::fromJson(body);
        if (doc.isObject()) {
            auto e = doc.object()["error"].toObject();
            if (!e["message"].toString().isEmpty())
                errMsg = e["message"].toString();
        }
        Q_EMIT errorOccurred(errMsg);
        Q_EMIT streamingFinished();
        running_ = false;
        return;
    }

    auto doc = QJsonDocument::fromJson(reply->readAll());
    if (!doc.isObject()) {
        Q_EMIT errorOccurred("Invalid JSON response from API");
        Q_EMIT streamingFinished();
        running_ = false;
        return;
    }

    bool hasTools = false;
    switch (cfg_.provider) {
    case Provider::ClaudeApi: hasTools = parseAnthropicResponse(doc.object()); break;
    case Provider::Gemini:    hasTools = parseGeminiResponse(doc.object());    break;
    case Provider::Groq:
    case Provider::Ollama:
    case Provider::Mlx:       hasTools = parseOpenAIResponse(doc.object());    break;
    default: break;
    }

    if (!hasTools) {
        Q_EMIT turnComplete(currentText_);
        Q_EMIT streamingFinished();
        running_ = false;
    }
    // If hasTools == true, executeTools() was called and will call postToApi() when done
}

// ──────────────────────────────────────────────────────────────
// Request builders
// ──────────────────────────────────────────────────────────────

QNetworkReply* HttpAiBackend::buildAnthropicRequest()
{
    QJsonObject body;
    body["model"]      = QStringLiteral("claude-sonnet-4-5");
    body["max_tokens"] = 4096;
    body["tools"]      = AiToolExecutor::anthropicToolDefs();

    if (!systemPrompt_.isEmpty())
        body["system"] = systemPrompt_;

    body["messages"] = messages_;

    QNetworkRequest req(QUrl("https://api.anthropic.com/v1/messages"));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("x-api-key", cfg_.apiKey.toUtf8());
    req.setRawHeader("anthropic-version", "2023-06-01");

    return nam_->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
}

QNetworkReply* HttpAiBackend::buildOpenAIRequest()
{
    QJsonArray msgs;
    if (!systemPrompt_.isEmpty())
        msgs.append(QJsonObject{{"role","system"},{"content", systemPrompt_}});
    for (const auto& m : messages_)
        msgs.append(m);

    QJsonObject body;
    body["messages"] = msgs;
    body["tools"]    = AiToolExecutor::openaiToolDefs();

    if (cfg_.provider == Provider::Groq) {
        body["model"] = QStringLiteral("llama-3.3-70b-versatile");
    } else {  // Ollama or MLX
        body["model"] = cfg_.ollamaModel;
    }

    QString baseUrl;
    if (cfg_.provider == Provider::Groq)
        baseUrl = "https://api.groq.com/openai/v1/chat/completions";
    else if (cfg_.provider == Provider::Mlx)
        baseUrl = cfg_.ollamaUrl + "/v1/chat/completions";
    else
        baseUrl = cfg_.ollamaUrl + "/v1/chat/completions";

    QUrl reqUrl(baseUrl);
    QNetworkRequest req(reqUrl);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (cfg_.provider == Provider::Groq)
        req.setRawHeader("Authorization", ("Bearer " + cfg_.apiKey).toUtf8());

    return nam_->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
}

QNetworkReply* HttpAiBackend::buildGeminiRequest()
{
    QJsonObject body;
    body["contents"] = geminiContents_;
    body["tools"]    = AiToolExecutor::geminiToolDefs();

    if (!systemPrompt_.isEmpty()) {
        body["system_instruction"] = QJsonObject{
            {"parts", QJsonArray{QJsonObject{{"text", systemPrompt_}}}}
        };
    }

    return nam_->post(QNetworkRequest(geminiUrl()),
                      QJsonDocument(body).toJson(QJsonDocument::Compact));
}

// ──────────────────────────────────────────────────────────────
// Response parsers
// ──────────────────────────────────────────────────────────────

bool HttpAiBackend::parseAnthropicResponse(const QJsonObject& resp)
{
    QJsonArray content = resp["content"].toArray();
    QList<ToolCall> calls;
    QString text;

    for (const auto& v : content) {
        auto block = v.toObject();
        if (block["type"].toString() == "text") {
            text += block["text"].toString();
        } else if (block["type"].toString() == "tool_use") {
            calls.append({
                block["id"].toString(),
                block["name"].toString(),
                block["input"].toObject()
            });
        }
    }

    // Add assistant message to history
    messages_.append(QJsonObject{
        {"role","assistant"},
        {"content", content}
    });

    if (!text.isEmpty()) {
        currentText_ += text;
        Q_EMIT textDelta(text);
    }

    if (!calls.isEmpty()) {
        executeTools(calls);
        return true;
    }
    return false;
}

bool HttpAiBackend::parseOpenAIResponse(const QJsonObject& resp)
{
    auto choice  = resp["choices"].toArray().first().toObject();
    auto message = choice["message"].toObject();
    QString finishReason = choice["finish_reason"].toString();

    QList<ToolCall> calls;
    QString text = message["content"].toString();

    auto toolCallsArr = message["tool_calls"].toArray();
    for (const auto& v : toolCallsArr) {
        auto tc = v.toObject();
        auto fn = tc["function"].toObject();
        calls.append({
            tc["id"].toString(),
            fn["name"].toString(),
            QJsonDocument::fromJson(fn["arguments"].toString().toUtf8()).object()
        });
    }

    // Store full assistant message for history (includes tool_calls field)
    lastAssistantMsg_ = message;
    messages_.append(message);

    if (!text.isEmpty()) {
        currentText_ += text;
        Q_EMIT textDelta(text);
    }

    if (!calls.isEmpty()) {
        executeTools(calls);
        return true;
    }
    Q_UNUSED(finishReason)
    return false;
}

bool HttpAiBackend::parseGeminiResponse(const QJsonObject& resp)
{
    auto candidate = resp["candidates"].toArray().first().toObject();
    auto parts     = candidate["content"].toObject()["parts"].toArray();
    QString finishReason = candidate["finishReason"].toString();

    QList<ToolCall> calls;
    QString text;
    QJsonArray functionCallParts;

    for (const auto& v : parts) {
        auto part = v.toObject();
        if (part.contains("text")) {
            text += part["text"].toString();
        } else if (part.contains("functionCall")) {
            auto fc = part["functionCall"].toObject();
            calls.append({
                fc["name"].toString(), // Gemini uses name as ID
                fc["name"].toString(),
                fc["args"].toObject()
            });
            functionCallParts.append(part);
        }
    }

    // Append model response to gemini contents
    geminiContents_.append(QJsonObject{
        {"role","model"},
        {"parts", parts}
    });
    lastFunctionCallParts_ = functionCallParts;

    if (!text.isEmpty()) {
        currentText_ += text;
        Q_EMIT textDelta(text);
    }

    if (!calls.isEmpty()) {
        executeTools(calls);
        return true;
    }
    Q_UNUSED(finishReason)
    return false;
}

// ──────────────────────────────────────────────────────────────
// Tool execution
// ──────────────────────────────────────────────────────────────

void HttpAiBackend::executeTools(const QList<ToolCall>& calls)
{
    pendingTools_ = calls;
    toolResults_  = QJsonArray();
    toolsDone_    = 0;

    for (const auto& call : calls) {
        Q_EMIT toolActivity(call.name, "");

        executor_->execute(call.name, call.args,
            [this, call](const QString& resultJson) {
                // Store result indexed by tool call id
                QJsonObject entry;
                entry["id"]     = call.id;
                entry["name"]   = call.name;
                entry["result"] = resultJson;
                toolResults_.append(entry);

                if (++toolsDone_ == pendingTools_.size())
                    onAllToolsDone();
            });
    }
}

void HttpAiBackend::onAllToolsDone()
{
    if (cancelled_) return;

    // Append tool results to conversation history in the right format
    switch (cfg_.provider) {
    case Provider::ClaudeApi: appendAnthropicToolResults(); break;
    case Provider::Gemini:    appendGeminiToolResults();    break;
    case Provider::Groq:
    case Provider::Ollama:
    case Provider::Mlx:       appendOpenAIToolResults();    break;
    default: break;
    }

    // Continue conversation with the tool results
    postToApi();
}

void HttpAiBackend::appendAnthropicToolResults()
{
    QJsonArray resultBlocks;
    for (const auto& v : toolResults_) {
        auto e = v.toObject();
        resultBlocks.append(QJsonObject{
            {"type",        "tool_result"},
            {"tool_use_id", e["id"].toString()},
            {"content",     e["result"].toString()}
        });
    }
    messages_.append(QJsonObject{{"role","user"},{"content", resultBlocks}});
}

void HttpAiBackend::appendOpenAIToolResults()
{
    for (const auto& v : toolResults_) {
        auto e = v.toObject();
        messages_.append(QJsonObject{
            {"role",         "tool"},
            {"tool_call_id", e["id"].toString()},
            {"name",         e["name"].toString()},
            {"content",      e["result"].toString()}
        });
    }
}

void HttpAiBackend::appendGeminiToolResults()
{
    QJsonArray responseParts;
    for (const auto& v : toolResults_) {
        auto e = v.toObject();
        QJsonObject resp;
        resp["name"]     = e["name"];
        resp["response"] = QJsonDocument::fromJson(
            e["result"].toString().toUtf8()).object();
        responseParts.append(QJsonObject{{"functionResponse", resp}});
    }
    geminiContents_.append(QJsonObject{
        {"role","user"},
        {"parts", responseParts}
    });
}

// ──────────────────────────────────────────────────────────────
// URL helpers
// ──────────────────────────────────────────────────────────────

QUrl HttpAiBackend::geminiUrl() const
{
    return QUrl(QString(
        "https://generativelanguage.googleapis.com/v1beta/models/"
        "gemini-2.0-flash:generateContent?key=%1"
    ).arg(cfg_.apiKey));
}

// ──────────────────────────────────────────────────────────────
// AiBackend::Config — persistence
// ──────────────────────────────────────────────────────────────

#include <QSettings>

AiBackend::Config AiBackend::Config::load()
{
    QSettings s("Dinero", "dinero-qt");
    s.beginGroup("ai");
    Config c;
    c.provider     = static_cast<Provider>(s.value("provider", static_cast<int>(Provider::Gemini)).toInt());
    c.apiKey       = s.value("apiKey").toString();
    c.claudePath   = s.value("claudePath").toString();
    c.ollamaModel  = s.value("ollamaModel", "llama3.2").toString();
    c.ollamaUrl    = s.value("ollamaUrl",   "http://localhost:11434").toString();
    c.baseAddress  = s.value("baseAddress").toString();
    s.endGroup();
    return c;
}

void AiBackend::Config::save() const
{
    QSettings s("Dinero", "dinero-qt");
    s.beginGroup("ai");
    s.setValue("provider",    static_cast<int>(provider));
    s.setValue("apiKey",      apiKey);
    s.setValue("claudePath",  claudePath);
    s.setValue("ollamaModel", ollamaModel);
    s.setValue("ollamaUrl",   ollamaUrl);
    s.setValue("baseAddress", baseAddress);
    s.endGroup();
}

bool AiBackend::Config::isConfigured() const
{
    switch (provider) {
    case Provider::ClaudeCli: return !claudePath.isEmpty();
    case Provider::Ollama:
    case Provider::Mlx:       return true;
    default:                  return !apiKey.isEmpty();
    }
}

QString AiBackend::Config::providerLabel() const
{
    switch (provider) {
    case Provider::ClaudeCli: return "Claude Code CLI";
    case Provider::ClaudeApi: return "Claude API";
    case Provider::Gemini:    return "Gemini (Google)";
    case Provider::Groq:      return "Groq";
    case Provider::Ollama:    return "Ollama (local)";
    case Provider::Mlx:       return "MLX (Apple Silicon)";
    }
    return {};
}

// ──────────────────────────────────────────────────────────────
// AiBackend::create factory
// ──────────────────────────────────────────────────────────────

#include "claudeclibackend.h"

AiBackend* AiBackend::create(const Config& cfg, const QString& datadirHint, QObject* parent)
{
    if (cfg.provider == Provider::ClaudeCli)
        return new ClaudeCliBackend(cfg, parent);
    return new HttpAiBackend(cfg, datadirHint, parent);
}
