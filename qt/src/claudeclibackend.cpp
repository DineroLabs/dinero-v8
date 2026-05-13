#include "claudeclibackend.h"

#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonArray>
#include <QFile>
#include <QDir>
#include <QUuid>
#include <QDebug>

ClaudeCliBackend::ClaudeCliBackend(const Config& cfg, QObject* parent)
    : AiBackend(parent)
{
    claudePath_ = cfg.claudePath.isEmpty() ? findClaudeBinary() : cfg.claudePath;
}

ClaudeCliBackend::~ClaudeCliBackend()
{
    cancel();
}

// ──────────────────────────────────────────────────────────────
// Public interface
// ──────────────────────────────────────────────────────────────

void ClaudeCliBackend::sendMessage(const QString& userText)
{
    if (running_) cancel();
    currentText_.clear();

    if (!process_) {
        process_ = new QProcess(this);
        process_->setProcessChannelMode(QProcess::SeparateChannels);
        connect(process_, &QProcess::readyReadStandardOutput,
                this, &ClaudeCliBackend::onReadyReadStdout);
        connect(process_, &QProcess::readyReadStandardError,
                this, &ClaudeCliBackend::onReadyReadStderr);
        connect(process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                this, &ClaudeCliBackend::onProcessFinished);
        connect(process_, &QProcess::errorOccurred,
                this, &ClaudeCliBackend::onProcessError);
    }

    if (sessionId_.isEmpty())
        sessionId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);

    QStringList args;
    args << "-p" << userText;
    args << "--output-format" << "stream-json";
    args << "--verbose";
    args << "--max-turns" << QString::number(maxTurns_);
    args << "--session-id" << sessionId_;
    args << "--continue";
    args << "--dangerously-skip-permissions";

    if (!systemPrompt_.isEmpty())
        args << "--append-system-prompt" << systemPrompt_;

    lineBuffer_.clear();
    running_ = true;
    Q_EMIT streamingStarted();

    process_->start(claudePath_, args);

    if (!process_->waitForStarted(5000)) {
        running_ = false;
        Q_EMIT errorOccurred("Failed to start Claude CLI: " + process_->errorString());
        Q_EMIT streamingFinished();
    }
}

void ClaudeCliBackend::cancel()
{
    if (process_ && process_->state() != QProcess::NotRunning) {
        process_->kill();
        process_->waitForFinished(2000);
    }
    running_ = false;
}

void ClaudeCliBackend::clearSession()
{
    sessionId_.clear();
}

bool ClaudeCliBackend::isReady() const
{
    return !claudePath_.isEmpty() && QFile::exists(claudePath_);
}

// ──────────────────────────────────────────────────────────────
// Process I/O
// ──────────────────────────────────────────────────────────────

void ClaudeCliBackend::onReadyReadStdout()
{
    if (!process_) return;
    lineBuffer_ += process_->readAllStandardOutput();
    while (true) {
        int idx = lineBuffer_.indexOf('\n');
        if (idx < 0) break;
        QByteArray line = lineBuffer_.left(idx).trimmed();
        lineBuffer_ = lineBuffer_.mid(idx + 1);
        if (!line.isEmpty()) processLine(line);
    }
}

void ClaudeCliBackend::onReadyReadStderr()
{
    if (!process_) return;
    QByteArray err = process_->readAllStandardError();
    if (!err.isEmpty()) qDebug() << "Claude CLI stderr:" << err;
}

void ClaudeCliBackend::onProcessFinished(int exitCode, QProcess::ExitStatus)
{
    running_ = false;
    if (!lineBuffer_.trimmed().isEmpty()) processLine(lineBuffer_.trimmed());
    lineBuffer_.clear();
    if (exitCode != 0 && currentText_.isEmpty())
        Q_EMIT errorOccurred(QString("Claude CLI exited with code %1").arg(exitCode));
    Q_EMIT streamingFinished();
}

void ClaudeCliBackend::onProcessError(QProcess::ProcessError error)
{
    running_ = false;
    QString msg;
    switch (error) {
    case QProcess::FailedToStart: msg = "Claude CLI not found. Check path in AI settings."; break;
    case QProcess::Crashed:       msg = "Claude CLI crashed."; break;
    default:                      msg = "Claude CLI error: " + process_->errorString(); break;
    }
    Q_EMIT errorOccurred(msg);
    Q_EMIT streamingFinished();
}

// ──────────────────────────────────────────────────────────────
// NDJSON parsing (stream-json format)
// ──────────────────────────────────────────────────────────────

void ClaudeCliBackend::processLine(const QByteArray& line)
{
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(line, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return;

    QJsonObject obj = doc.object();
    QString type = obj["type"].toString();

    if      (type == "stream_event") handleStreamEvent(obj);
    else if (type == "assistant")    handleAssistantMessage(obj);
    else if (type == "result")       handleResult(obj);
    else if (type == "system") {
        QString sid = obj["session_id"].toString();
        if (!sid.isEmpty()) sessionId_ = sid;
    }
}

void ClaudeCliBackend::handleStreamEvent(const QJsonObject& obj)
{
    QJsonObject event = obj["event"].toObject();
    if (event["type"].toString() == "content_block_delta") {
        QJsonObject delta = event["delta"].toObject();
        if (delta["type"].toString() == "text_delta") {
            QString text = delta["text"].toString();
            currentText_ += text;
            Q_EMIT textDelta(text);
        }
    }
}

void ClaudeCliBackend::handleAssistantMessage(const QJsonObject& obj)
{
    for (const auto& block : obj["message"].toObject()["content"].toArray()) {
        QJsonObject b = block.toObject();
        QString btype = b["type"].toString();
        if (btype == "text") {
            QString text = b["text"].toString();
            if (currentText_.isEmpty() && !text.isEmpty()) {
                currentText_ = text;
                Q_EMIT textDelta(text);
            }
        } else if (btype == "tool_use") {
            QString toolName = b["name"].toString();
            QJsonObject input = b["input"].toObject();
            QString summary;
            if (toolName == "Bash")
                summary = "$ " + input["command"].toString().left(80);
            else if (toolName.startsWith("mcp__ops__"))
                summary = toolName.mid(10);
            else
                summary = toolName;
            Q_EMIT toolActivity(toolName, summary);
        }
    }
}

void ClaudeCliBackend::handleResult(const QJsonObject& obj)
{
    QString sid = obj["session_id"].toString();
    if (!sid.isEmpty()) sessionId_ = sid;

    double cost = obj["total_cost_usd"].toDouble();
    if (cost > 0) Q_EMIT costUpdate(cost);

    if (obj["subtype"].toString() == "success") {
        QString finalText = currentText_.isEmpty()
                          ? obj["result"].toString()
                          : currentText_;
        Q_EMIT turnComplete(finalText);
    } else {
        QString err = obj["result"].toString();
        Q_EMIT errorOccurred(err.isEmpty()
            ? QString("Claude error: %1").arg(obj["subtype"].toString())
            : err);
    }
}

// ──────────────────────────────────────────────────────────────
// Binary discovery
// ──────────────────────────────────────────────────────────────

QString ClaudeCliBackend::findClaudeBinary() const
{
    QString path = QStandardPaths::findExecutable("claude");
    if (!path.isEmpty()) return path;

    QStringList candidates = {
        "/opt/homebrew/bin/claude",
        "/usr/local/bin/claude",
        QDir::homePath() + "/.npm-global/bin/claude",
        QDir::homePath() + "/.local/bin/claude",
        "/usr/bin/claude",
    };
    for (const auto& p : candidates)
        if (QFile::exists(p)) return p;

    return {};
}
