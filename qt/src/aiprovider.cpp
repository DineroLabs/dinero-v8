#include "aiprovider.h"
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QSslConfiguration>
#include <QDebug>

static constexpr int MAX_TOKENS = 4096;

AiProvider::AiProvider(QObject* parent)
    : QObject(parent)
    , nam_(new QNetworkAccessManager(this))
{
}

void AiProvider::setApiKey(const QString& key) { apiKey_ = key.trimmed(); }

void AiProvider::setBackend(const QString& baseUrl, const QString& model)
{
    baseUrl_ = baseUrl;
    model_ = model;
    localMode_ = !baseUrl.contains("anthropic.com");
}

void AiProvider::setSystemPrompt(const QString& prompt) { systemPrompt_ = prompt; }

void AiProvider::setTools(const QJsonArray& toolDefinitions) { toolDefinitions_ = toolDefinitions; }

void AiProvider::clearConversation()
{
    conversationHistory_ = QJsonArray();
    currentText_.clear();
    currentToolCalls_.clear();
}

void AiProvider::sendMessage(const QString& userText)
{
    if (streaming_) {
        cancel();
    }

    QJsonObject userMsg;
    userMsg["role"] = "user";
    userMsg["content"] = userText;
    conversationHistory_.append(userMsg);

    currentText_.clear();
    currentToolCalls_.clear();
    sseBuffer_.clear();
    currentToolJson_.clear();
    currentBlockType_.clear();

    postMessages();
}

void AiProvider::feedToolResults(const QList<AiToolCall>& calls,
                                  const QJsonArray& toolResultContents)
{
    // Append assistant message with tool_use blocks
    QJsonArray assistantContent;
    for (const auto& call : calls) {
        QJsonObject block;
        block["type"] = "tool_use";
        block["id"] = call.id;
        block["name"] = call.name;
        block["input"] = call.input;
        assistantContent.append(block);
    }
    // If there was text before the tool calls, prepend it
    if (!currentText_.isEmpty()) {
        QJsonObject textBlock;
        textBlock["type"] = "text";
        textBlock["text"] = currentText_;
        assistantContent.prepend(textBlock);
    }

    QJsonObject assistantMsg;
    assistantMsg["role"] = "assistant";
    assistantMsg["content"] = assistantContent;
    conversationHistory_.append(assistantMsg);

    // Append user message with tool_result blocks
    QJsonObject userMsg;
    userMsg["role"] = "user";
    userMsg["content"] = toolResultContents;
    conversationHistory_.append(userMsg);

    // Reset streaming state for continuation
    currentText_.clear();
    currentToolCalls_.clear();
    sseBuffer_.clear();
    currentToolJson_.clear();
    currentBlockType_.clear();

    postMessages();
}

void AiProvider::cancel()
{
    if (activeReply_) {
        activeReply_->abort();
        activeReply_->deleteLater();
        activeReply_ = nullptr;
    }
    streaming_ = false;
    Q_EMIT streamingFinished();
}

void AiProvider::postMessages()
{
    if (!localMode_ && apiKey_.isEmpty()) {
        Q_EMIT errorOccurred("No API key configured");
        return;
    }

    QJsonObject body;
    body["model"] = model_;
    body["max_tokens"] = MAX_TOKENS;
    body["stream"] = true;

    if (!systemPrompt_.isEmpty()) {
        body["system"] = systemPrompt_;
    }
    if (!toolDefinitions_.isEmpty()) {
        body["tools"] = toolDefinitions_;
    }
    body["messages"] = conversationHistory_;

    QString url = baseUrl_ + "/v1/messages";
    QNetworkRequest req{QUrl(url)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("x-api-key",
        localMode_ ? QByteArray("ollama") : apiKey_.toUtf8());
    req.setRawHeader("anthropic-version", "2023-06-01");

    // Enable streaming — don't buffer the full response
    req.setAttribute(QNetworkRequest::HttpPipeliningAllowedAttribute, false);

    QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    activeReply_ = nam_->post(req, payload);
    connect(activeReply_, &QNetworkReply::readyRead, this, &AiProvider::onReadyRead);
    connect(activeReply_, &QNetworkReply::finished, this, &AiProvider::onFinished);

    streaming_ = true;
    Q_EMIT streamingStarted();
}

void AiProvider::onReadyRead()
{
    if (!activeReply_) return;

    sseBuffer_ += activeReply_->readAll();

    // Process complete SSE events (delimited by \n\n)
    while (true) {
        int idx = sseBuffer_.indexOf("\n\n");
        if (idx < 0) break;

        QByteArray eventBlock = sseBuffer_.left(idx);
        sseBuffer_ = sseBuffer_.mid(idx + 2);

        QString eventType;
        QByteArray dataLine;

        for (const QByteArray& line : eventBlock.split('\n')) {
            if (line.startsWith("event: ")) {
                eventType = QString::fromUtf8(line.mid(7)).trimmed();
            } else if (line.startsWith("data: ")) {
                dataLine = line.mid(6);
            }
        }

        if (eventType.isEmpty() || dataLine.isEmpty()) continue;

        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(dataLine, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) continue;

        processEvent(eventType, doc.object());
    }
}

void AiProvider::onFinished()
{
    if (!activeReply_) return;

    if (activeReply_->error() != QNetworkReply::NoError &&
        activeReply_->error() != QNetworkReply::OperationCanceledError) {

        // Try to parse error body — check both unread data and sseBuffer_
        // (onReadyRead may have consumed it into sseBuffer_ already)
        QByteArray body = activeReply_->readAll();
        if (body.isEmpty() && !sseBuffer_.isEmpty()) {
            body = sseBuffer_;
            sseBuffer_.clear();
        }

        QString errMsg = activeReply_->errorString();
        qWarning() << "AI API error:" << errMsg;
        qWarning() << "Response body:" << body;

        // Friendly messages for local mode errors
        if (localMode_) {
            if (activeReply_->error() == QNetworkReply::ConnectionRefusedError) {
                errMsg = "Cannot connect to Ollama. Start it with:\n"
                         "ollama serve\n\n"
                         "Install from ollama.com if needed.";
            } else if (body.contains("not found") || body.contains("model")) {
                errMsg = QString("Model \"%1\" not found. Pull it with:\n"
                         "ollama pull %1").arg(model_);
            }
        }

        QJsonDocument doc = QJsonDocument::fromJson(body);
        if (doc.isObject()) {
            auto obj = doc.object();
            if (obj.contains("error")) {
                auto errObj = obj["error"].toObject();
                errMsg = errObj["message"].toString(errMsg);
            }
        }
        Q_EMIT errorOccurred(errMsg);
    }

    activeReply_->deleteLater();
    activeReply_ = nullptr;
    streaming_ = false;
    Q_EMIT streamingFinished();
}

void AiProvider::processEvent(const QString& eventType, const QJsonObject& data)
{
    if (eventType == "content_block_start") {
        auto block = data["content_block"].toObject();
        currentBlockType_ = block["type"].toString();

        if (currentBlockType_ == "tool_use") {
            currentToolId_ = block["id"].toString();
            currentToolName_ = block["name"].toString();
            currentToolJson_.clear();
        }
    }
    else if (eventType == "content_block_delta") {
        auto delta = data["delta"].toObject();
        QString deltaType = delta["type"].toString();

        if (deltaType == "text_delta") {
            QString text = delta["text"].toString();
            currentText_ += text;
            Q_EMIT textDelta(text);
            Q_EMIT textUpdated(currentText_);
        }
        else if (deltaType == "input_json_delta") {
            currentToolJson_ += delta["partial_json"].toString();
        }
    }
    else if (eventType == "content_block_stop") {
        if (currentBlockType_ == "tool_use" && !currentToolId_.isEmpty()) {
            AiToolCall call;
            call.id = currentToolId_;
            call.name = currentToolName_;

            QJsonDocument inputDoc = QJsonDocument::fromJson(currentToolJson_.toUtf8());
            if (inputDoc.isObject()) {
                call.input = inputDoc.object();
            }

            currentToolCalls_.append(call);
            currentToolId_.clear();
            currentToolName_.clear();
            currentToolJson_.clear();
        }
        currentBlockType_.clear();
    }
    else if (eventType == "message_delta") {
        auto delta = data["delta"].toObject();
        QString stopReason = delta["stop_reason"].toString();

        if (stopReason == "tool_use") {
            Q_EMIT allToolCallsReady(currentToolCalls_);
        }
        else if (stopReason == "end_turn") {
            // Append assistant text to conversation history
            QJsonObject assistantMsg;
            assistantMsg["role"] = "assistant";
            assistantMsg["content"] = currentText_;
            conversationHistory_.append(assistantMsg);

            Q_EMIT turnComplete(currentText_);
        }
    }
    else if (eventType == "error") {
        auto errObj = data["error"].toObject();
        Q_EMIT errorOccurred(errObj["message"].toString("Unknown streaming error"));
    }
}
