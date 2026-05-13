#include "claudeprocess.h"
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonArray>
#include <QFile>
#include <QDir>
#include <QUuid>
#include <QDebug>

ClaudeProcess::ClaudeProcess(QObject* parent)
    : QObject(parent)
{
    claudePath_ = findClaudeBinary();
}

ClaudeProcess::~ClaudeProcess()
{
    cancel();
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void ClaudeProcess::sendMessage(const QString& userText)
{
    if (running_) {
        cancel();
    }

    currentText_.clear();

    if (!process_) {
        process_ = new QProcess(this);
        process_->setProcessChannelMode(QProcess::SeparateChannels);
        connect(process_, &QProcess::readyReadStandardOutput,
                this, &ClaudeProcess::onReadyReadStdout);
        connect(process_, &QProcess::readyReadStandardError,
                this, &ClaudeProcess::onReadyReadStderr);
        connect(process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                this, &ClaudeProcess::onProcessFinished);
        connect(process_, &QProcess::errorOccurred,
                this, &ClaudeProcess::onProcessError);
    }

    // Generate session ID on first message
    if (sessionId_.isEmpty())
        sessionId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);

    QStringList args;
    args << "-p" << userText;
    args << "--output-format" << "stream-json";
    args << "--verbose";
    args << "--max-turns" << QString::number(maxTurns_);
    args << "--session-id" << sessionId_;
    args << "--continue";

    if (!systemPrompt_.isEmpty()) {
        args << "--append-system-prompt" << systemPrompt_;
    }

    // Use dontAsk mode — Claude handles tools autonomously
    // The MCP servers (ops, etc.) are already configured in ~/.claude/
    args << "--dangerously-skip-permissions";

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

void ClaudeProcess::cancel()
{
    if (process_ && process_->state() != QProcess::NotRunning) {
        process_->kill();
        process_->waitForFinished(2000);
    }
    running_ = false;
}

void ClaudeProcess::clearSession()
{
    sessionId_.clear();
}

bool ClaudeProcess::isClaudeInstalled() const
{
    return !claudePath_.isEmpty() && QFile::exists(claudePath_);
}

QString ClaudeProcess::claudeVersion() const
{
    if (!isClaudeInstalled()) return {};

    QProcess proc;
    proc.start(claudePath_, {"--version"});
    if (!proc.waitForFinished(3000)) return {};
    return QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
}

bool ClaudeProcess::isRunning() const
{
    return running_;
}

void ClaudeProcess::setClaudePath(const QString& path)
{
    claudePath_ = path;
}

void ClaudeProcess::setSystemPrompt(const QString& prompt)
{
    systemPrompt_ = prompt;
}

void ClaudeProcess::setMaxTurns(int turns)
{
    maxTurns_ = turns;
}

// ---------------------------------------------------------------------------
// Process I/O
// ---------------------------------------------------------------------------

void ClaudeProcess::onReadyReadStdout()
{
    if (!process_) return;
    lineBuffer_ += process_->readAllStandardOutput();

    // Process complete lines (NDJSON — one JSON per \n)
    while (true) {
        int idx = lineBuffer_.indexOf('\n');
        if (idx < 0) break;

        QByteArray line = lineBuffer_.left(idx).trimmed();
        lineBuffer_ = lineBuffer_.mid(idx + 1);

        if (!line.isEmpty())
            processLine(line);
    }
}

void ClaudeProcess::onReadyReadStderr()
{
    if (!process_) return;
    QByteArray err = process_->readAllStandardError();
    if (!err.isEmpty())
        qDebug() << "Claude stderr:" << err;
}

void ClaudeProcess::onProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    Q_UNUSED(status)
    running_ = false;

    // Process any remaining data in the buffer
    if (!lineBuffer_.trimmed().isEmpty())
        processLine(lineBuffer_.trimmed());
    lineBuffer_.clear();

    if (exitCode != 0 && currentText_.isEmpty()) {
        Q_EMIT errorOccurred(QString("Claude exited with code %1").arg(exitCode));
    }
    Q_EMIT streamingFinished();
}

void ClaudeProcess::onProcessError(QProcess::ProcessError error)
{
    running_ = false;
    QString msg;
    switch (error) {
    case QProcess::FailedToStart:
        msg = "Claude CLI failed to start. Is it installed?";
        break;
    case QProcess::Crashed:
        msg = "Claude CLI crashed.";
        break;
    case QProcess::Timedout:
        msg = "Claude CLI timed out.";
        break;
    default:
        msg = "Claude CLI error: " + process_->errorString();
        break;
    }
    Q_EMIT errorOccurred(msg);
    Q_EMIT streamingFinished();
}

// ---------------------------------------------------------------------------
// NDJSON line parsing
// ---------------------------------------------------------------------------

void ClaudeProcess::processLine(const QByteArray& line)
{
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(line, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return;

    QJsonObject obj = doc.object();
    QString type = obj["type"].toString();

    if (type == "stream_event")
        handleStreamEvent(obj);
    else if (type == "assistant")
        handleAssistantMessage(obj);
    else if (type == "result")
        handleResult(obj);
    else if (type == "system")
        handleSystemMessage(obj);
    // Ignore: rate_limit_event, control_request (auto-handled by --dangerously-skip-permissions)
}

void ClaudeProcess::handleStreamEvent(const QJsonObject& obj)
{
    QJsonObject event = obj["event"].toObject();
    QString eventType = event["type"].toString();

    if (eventType == "content_block_delta") {
        QJsonObject delta = event["delta"].toObject();
        QString deltaType = delta["type"].toString();

        if (deltaType == "text_delta") {
            QString text = delta["text"].toString();
            currentText_ += text;
            Q_EMIT textDelta(text);
        }
    }
}

void ClaudeProcess::handleAssistantMessage(const QJsonObject& obj)
{
    QJsonObject message = obj["message"].toObject();
    QJsonArray content = message["content"].toArray();

    for (const auto& block : content) {
        QJsonObject b = block.toObject();
        QString blockType = b["type"].toString();

        if (blockType == "text") {
            QString text = b["text"].toString();
            // If we haven't received stream_events (short response),
            // emit the full text as a single delta
            if (currentText_.isEmpty() && !text.isEmpty()) {
                currentText_ = text;
                Q_EMIT textDelta(text);
            }
        }
        else if (blockType == "tool_use") {
            QString toolName = b["name"].toString();
            QJsonObject input = b["input"].toObject();
            // Show what tool is being used
            QString inputSummary;
            if (toolName == "Bash")
                inputSummary = input["command"].toString().left(80);
            else if (toolName == "Read" || toolName == "Write" || toolName == "Edit")
                inputSummary = input["file_path"].toString();
            else if (toolName == "Grep")
                inputSummary = input["pattern"].toString();
            else if (toolName.startsWith("mcp__ops__"))
                inputSummary = toolName.mid(10); // strip mcp__ops__ prefix
            else
                inputSummary = toolName;

            Q_EMIT toolActivity(toolName, inputSummary);
        }
    }
}

void ClaudeProcess::handleResult(const QJsonObject& obj)
{
    QString subtype = obj["subtype"].toString();
    QString resultText = obj["result"].toString();

    // Capture session_id for continuity
    QString sid = obj["session_id"].toString();
    if (!sid.isEmpty())
        sessionId_ = sid;

    // Cost tracking
    double cost = obj["total_cost_usd"].toDouble();
    if (cost > 0)
        Q_EMIT costUpdate(cost);

    if (subtype == "success") {
        // Use currentText_ if we got streaming, otherwise use result field
        QString finalText = currentText_.isEmpty() ? resultText : currentText_;
        Q_EMIT turnComplete(finalText);
    }
    else {
        // error_during_execution, error_max_turns, error_max_budget_usd
        QString error = resultText.isEmpty()
            ? QString("Claude error: %1").arg(subtype)
            : resultText;
        Q_EMIT errorOccurred(error);
    }
}

void ClaudeProcess::handleSystemMessage(const QJsonObject& obj)
{
    QString subtype = obj["subtype"].toString();

    if (subtype == "init") {
        // Capture session_id from init
        QString sid = obj["session_id"].toString();
        if (!sid.isEmpty())
            sessionId_ = sid;
    }
    // Other system messages (status, etc.) are informational — ignore for now
}

// ---------------------------------------------------------------------------
// Binary discovery
// ---------------------------------------------------------------------------

QString ClaudeProcess::findClaudeBinary() const
{
    // Check common paths
    QString path = QStandardPaths::findExecutable("claude");
    if (!path.isEmpty()) return path;

    // Homebrew on macOS
    QStringList candidates = {
        "/opt/homebrew/bin/claude",
        "/usr/local/bin/claude",
        QDir::homePath() + "/.npm-global/bin/claude",
        QDir::homePath() + "/.local/bin/claude",
    };
    for (const auto& p : candidates) {
        if (QFile::exists(p)) return p;
    }

    return {};
}
