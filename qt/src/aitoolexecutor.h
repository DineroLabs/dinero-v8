#pragma once

#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>
#include <QString>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

// Self-contained tool executor for AI backends.
// Loads RPC credentials from the Dinero datadir independently.
// Handles both wallet RPC calls and public market data (CoinGecko).
class AiToolExecutor : public QObject {
    Q_OBJECT
public:
    using Callback = std::function<void(const QString& resultJson)>;

    explicit AiToolExecutor(const QString& datadirHint, QObject* parent = nullptr);

    void execute(const QString& toolName, const QJsonObject& args, Callback cb);

    // Tool schema arrays for each API format
    static QJsonArray anthropicToolDefs();
    static QJsonArray openaiToolDefs();   // Groq, Ollama, OpenAI-compatible
    static QJsonArray geminiToolDefs();

private:
    void toolGetBalance(const QJsonObject& args, Callback cb);
    void toolGetNodeStatus(const QJsonObject& args, Callback cb);
    void toolGetReceiveAddress(const QJsonObject& args, Callback cb);
    void toolShieldDin(const QJsonObject& args, Callback cb);
    void toolUnshieldDin(const QJsonObject& args, Callback cb);
    void toolSendDin(const QJsonObject& args, Callback cb);
    void toolGetCryptoPrices(const QJsonObject& args, Callback cb);
    void toolGetMarketOverview(const QJsonObject& args, Callback cb);
    void toolGetWdinInfo(const QJsonObject& args, Callback cb);

    // Async RPC call — cb receives raw QJsonValue result or error string
    void rpcCall(const QString& method, const QJsonArray& params,
                 std::function<void(bool ok, const QJsonValue& result)> cb);
    // Async HTTP GET
    void httpGet(const QUrl& url,
                 std::function<void(bool ok, const QJsonDocument& doc)> cb);

    bool loadCredentials(const QString& datadirHint);

    QNetworkAccessManager* nam_;
    QUrl    rpcUrl_;
    QString datadirHint_;
    QString cookieToken_;
};
