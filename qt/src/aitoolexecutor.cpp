#include "aitoolexecutor.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QDir>
#include <QUrl>
#include <QUrlQuery>
#include <QDebug>

// ──────────────────────────────────────────────────────────────
// Construction
// ──────────────────────────────────────────────────────────────

AiToolExecutor::AiToolExecutor(const QString& datadirHint, QObject* parent)
    : QObject(parent)
    , nam_(new QNetworkAccessManager(this))
    , rpcUrl_("http://127.0.0.1:20998/")
{
    datadirHint_ = datadirHint;
    loadCredentials(datadirHint_);
}

bool AiToolExecutor::loadCredentials(const QString& datadirHint)
{
    QStringList cookiePaths;
    QStringList serverInfoPaths;

    if (!datadirHint.trimmed().isEmpty()) {
        cookiePaths << QDir(datadirHint).filePath(".cookie");
        serverInfoPaths << QDir(datadirHint).filePath("serverinfo.json");
    }

#ifdef Q_OS_MAC
    cookiePaths << QDir::home().filePath("Library/Application Support/Dinero/.cookie");
    serverInfoPaths << QDir::home().filePath("Library/Application Support/Dinero/serverinfo.json");
#endif

    cookiePaths << QDir::home().filePath(".dinero/.cookie");
    serverInfoPaths << QDir::home().filePath(".dinero/serverinfo.json");

    // Try to discover RPC URL from serverinfo.json
    for (const QString& path : serverInfoPaths) {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            auto doc = QJsonDocument::fromJson(f.readAll());
            if (doc.isObject()) {
                auto rpc = doc.object()["rpc"].toObject();
                if (!rpc.isEmpty()) {
                    QString host = rpc["host"].toString("127.0.0.1");
                    int     port = rpc["port"].toInt(20998);
                    bool    tls  = rpc["tls"].toBool(false);
                    rpcUrl_ = QUrl(QString("%1://%2:%3/")
                                   .arg(tls ? "https" : "http")
                                   .arg(host).arg(port));
                    break;
                }
            }
        }
    }

    for (const QString& path : cookiePaths) {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QString line = QString::fromUtf8(f.readLine()).trimmed();
            if (!line.isEmpty()) {
                cookieToken_ = line;
                return true;
            }
        }
    }
    return false;
}

// ──────────────────────────────────────────────────────────────
// Tool dispatch
// ──────────────────────────────────────────────────────────────

void AiToolExecutor::execute(const QString& name, const QJsonObject& args, Callback cb)
{
    if      (name == "get_din_balance")      toolGetBalance(args, cb);
    else if (name == "get_node_status")      toolGetNodeStatus(args, cb);
    else if (name == "get_receive_address")  toolGetReceiveAddress(args, cb);
    else if (name == "shield_din")           toolShieldDin(args, cb);
    else if (name == "unshield_din")         toolUnshieldDin(args, cb);
    else if (name == "send_din")             toolSendDin(args, cb);
    else if (name == "get_crypto_prices")    toolGetCryptoPrices(args, cb);
    else if (name == "get_market_overview")  toolGetMarketOverview(args, cb);
    else if (name == "get_wdin_info")        toolGetWdinInfo(args, cb);
    else cb(QString(R"({"error":"unknown tool %1"})").arg(name));
}

// ──────────────────────────────────────────────────────────────
// Wallet tools (via local RPC)
// ──────────────────────────────────────────────────────────────

void AiToolExecutor::toolGetBalance(const QJsonObject&, Callback cb)
{
    // Fetch both transparent and confidential balances in parallel
    auto results = std::make_shared<QJsonObject>();
    auto remaining = std::make_shared<int>(2);

    auto tryFinish = [=]() {
        if (--(*remaining) == 0) {
            QJsonObject out;
            out["transparent_din"] = (*results)["transparent"].toDouble();
            out["private_din"]     = (*results)["private"].toDouble();
            out["total_din"]       = out["transparent_din"].toDouble()
                                   + out["private_din"].toDouble();
            cb(QJsonDocument(out).toJson(QJsonDocument::Compact));
        }
    };

    rpcCall("wallet.getbalance", {}, [=](bool ok, const QJsonValue& v) {
        if (ok) (*results)["transparent"] = v.toObject()["balance"].toDouble(v.toDouble());
        tryFinish();
    });

    rpcCall("wallet.getconfidentialbalance", {}, [=](bool ok, const QJsonValue& v) {
        if (ok) (*results)["private"] = v.toObject()["balance"].toDouble(v.toDouble());
        tryFinish();
    });
}

void AiToolExecutor::toolGetNodeStatus(const QJsonObject&, Callback cb)
{
    auto results = std::make_shared<QJsonObject>();
    auto remaining = std::make_shared<int>(2);

    auto tryFinish = [=]() {
        if (--(*remaining) == 0) {
            cb(QJsonDocument(*results).toJson(QJsonDocument::Compact));
        }
    };

    rpcCall("getblockchaininfo", {}, [=](bool ok, const QJsonValue& v) {
        if (ok) {
            auto o = v.toObject();
            (*results)["blocks"]           = o["blocks"].toInt();
            (*results)["headers"]          = o["headers"].toInt();
            (*results)["synced"]           = o["blocks"].toInt() == o["headers"].toInt();
            (*results)["sync_progress"]    = o["verificationprogress"].toDouble();
            (*results)["chain"]            = o["chain"].toString();
            (*results)["difficulty"]       = o["difficulty"].toDouble();
        }
        tryFinish();
    });

    rpcCall("getnetworkinfo", {}, [=](bool ok, const QJsonValue& v) {
        if (ok) {
            auto o = v.toObject();
            (*results)["peers"]            = o["connections"].toInt();
            (*results)["version"]          = o["subversion"].toString();
        }
        tryFinish();
    });
}

void AiToolExecutor::toolGetReceiveAddress(const QJsonObject&, Callback cb)
{
    rpcCall("getnewaddress", {}, [=](bool ok, const QJsonValue& v) {
        QJsonObject out;
        if (ok) out["address"] = v.toString(v.toObject()["address"].toString());
        else    out["error"]   = "Could not generate address";
        cb(QJsonDocument(out).toJson(QJsonDocument::Compact));
    });
}

// ──────────────────────────────────────────────────────────────
// Financial tool safety gate
// ──────────────────────────────────────────────────────────────
//
// The AI panel spawns claude-cli with --dangerously-skip-permissions, which
// means any MCP tool Claude calls executes without a user prompt. Shipping
// fund-moving tools (send / shield / unshield) through that path is a live
// prompt-injection → fund-loss vector: a malicious response from any MCP
// tool result, web page, document, or upstream Claude API payload could
// coerce the model into calling toolSendDin / toolShieldDin / toolUnshieldDin
// with attacker-chosen parameters, and the wallet would broadcast the
// transaction with zero human-in-the-loop confirmation.
//
// Until a proper confirmation modal is wired in (showing amount / recipient /
// fee and requiring an explicit user click before the RPC fires), these three
// tools return an error and instruct the user to use the manual wallet UI.
//
// Read-only tools (balance / status / address / price / market) continue to
// work as before — they cannot move funds.
//
// To re-enable a specific tool later, replace the guard with a synchronous
// confirmation dialog that blocks on QMessageBox::question or a dedicated
// SendConfirmationDialog, and only proceed to rpcCall() on explicit approval.
static const QString kAiFinancialToolsDisabledError = QStringLiteral(
    R"({"error":"This operation has been disabled in the AI assistant for safety. Please use the manual wallet UI to send, shield, or unshield funds. Re-enabling requires adding a confirmation modal — see aitoolexecutor.cpp."})");

void AiToolExecutor::toolShieldDin(const QJsonObject& args, Callback cb)
{
    Q_UNUSED(args)
    cb(kAiFinancialToolsDisabledError);
}

void AiToolExecutor::toolUnshieldDin(const QJsonObject& args, Callback cb)
{
    Q_UNUSED(args)
    cb(kAiFinancialToolsDisabledError);
}

void AiToolExecutor::toolSendDin(const QJsonObject& args, Callback cb)
{
    Q_UNUSED(args)
    cb(kAiFinancialToolsDisabledError);
}

// ──────────────────────────────────────────────────────────────
// Market tools (CoinGecko — free, no API key)
// ──────────────────────────────────────────────────────────────

static QString coinIdFor(const QString& symbol)
{
    // Map common tickers/names to CoinGecko IDs
    static const QHash<QString, QString> map = {
        {"btc","bitcoin"},   {"bitcoin","bitcoin"},
        {"eth","ethereum"},  {"ethereum","ethereum"},
        {"xmr","monero"},    {"monero","monero"},
        {"sol","solana"},    {"solana","solana"},
        {"bnb","binancecoin"},
        {"usdt","tether"},
        {"usdc","usd-coin"},
        {"ltc","litecoin"},
        {"dot","polkadot"},
        {"ada","cardano"},
        {"avax","avalanche-2"},
        {"link","chainlink"},
        {"matic","matic-network"},
        {"doge","dogecoin"},
        {"shib","shiba-inu"},
        {"din","dinero"},    {"dinero","dinero"},
    };
    return map.value(symbol.toLower(), symbol.toLower());
}

void AiToolExecutor::toolGetCryptoPrices(const QJsonObject& args, Callback cb)
{
    QJsonArray coinsArg = args["coins"].toArray();
    if (coinsArg.isEmpty()) {
        // Default: major coins + DIN
        coinsArg = QJsonArray{"btc","eth","xmr","sol","din"};
    }

    QStringList ids;
    for (const auto& v : coinsArg)
        ids << coinIdFor(v.toString());

    QUrl url("https://api.coingecko.com/api/v3/simple/price");
    QUrlQuery q;
    q.addQueryItem("ids", ids.join(","));
    q.addQueryItem("vs_currencies", "usd");
    q.addQueryItem("include_24hr_change", "true");
    q.addQueryItem("include_market_cap", "true");
    url.setQuery(q);

    httpGet(url, [=](bool ok, const QJsonDocument& doc) {
        if (!ok) { cb(R"({"error":"price fetch failed"})"); return; }
        // Reformat into a cleaner structure
        QJsonObject result;
        auto obj = doc.object();
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            auto coin = it.value().toObject();
            QJsonObject entry;
            entry["price_usd"]    = coin["usd"].toDouble();
            entry["change_24h"]   = coin["usd_24h_change"].toDouble();
            entry["market_cap"]   = coin["usd_market_cap"].toDouble();
            result[it.key()] = entry;
        }
        cb(QJsonDocument(result).toJson(QJsonDocument::Compact));
    });
}

void AiToolExecutor::toolGetMarketOverview(const QJsonObject&, Callback cb)
{
    httpGet(QUrl("https://api.coingecko.com/api/v3/global"),
            [=](bool ok, const QJsonDocument& doc) {
        if (!ok) { cb(R"({"error":"market overview fetch failed"})"); return; }
        auto data = doc.object()["data"].toObject();
        QJsonObject result;
        result["total_market_cap_usd"]   = data["total_market_cap"].toObject()["usd"].toDouble();
        result["total_volume_24h_usd"]   = data["total_volume"].toObject()["usd"].toDouble();
        result["btc_dominance_pct"]      = data["market_cap_percentage"].toObject()["btc"].toDouble();
        result["eth_dominance_pct"]      = data["market_cap_percentage"].toObject()["eth"].toDouble();
        result["active_coins"]           = data["active_cryptocurrencies"].toInt();
        result["market_cap_change_24h"]  = data["market_cap_change_percentage_24h_usd"].toDouble();
        cb(QJsonDocument(result).toJson(QJsonDocument::Compact));
    });
}

void AiToolExecutor::toolGetWdinInfo(const QJsonObject&, Callback cb)
{
    // wDIN is on Base chain — query CoinGecko if listed, otherwise fall back to a basic response
    QUrl url("https://api.coingecko.com/api/v3/simple/token_price/base");
    QUrlQuery q;
    q.addQueryItem("contract_addresses", "0xCD91b5C0aaD48E49F992BA690647C244f535C90B");
    q.addQueryItem("vs_currencies", "usd");
    q.addQueryItem("include_24hr_change", "true");
    url.setQuery(q);

    httpGet(url, [=](bool ok, const QJsonDocument& doc) {
        QJsonObject result;
        result["contract"]  = QString("0xCD91b5C0aaD48E49F992BA690647C244f535C90B");
        result["network"]   = "Base (Ethereum L2)";
        result["dex"]       = "Uniswap V3";

        if (ok && !doc.object().isEmpty()) {
            auto tokenData = doc.object().begin().value().toObject();
            result["price_usd"]  = tokenData["usd"].toDouble();
            result["change_24h"] = tokenData["usd_24h_change"].toDouble();
        } else {
            result["note"] = "Price data unavailable — check Uniswap directly";
        }
        cb(QJsonDocument(result).toJson(QJsonDocument::Compact));
    });
}

// ──────────────────────────────────────────────────────────────
// Low-level helpers
// ──────────────────────────────────────────────────────────────

void AiToolExecutor::rpcCall(const QString& method, const QJsonArray& params,
                              std::function<void(bool, const QJsonValue&)> cb)
{
    QJsonObject body;
    body["jsonrpc"] = "2.0";
    body["id"]      = 1;
    body["method"]  = method;
    body["params"]  = params;

    QNetworkRequest req(rpcUrl_);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!cookieToken_.isEmpty()) {
        QByteArray creds = cookieToken_.toUtf8();
        req.setRawHeader("Authorization",
                         "Basic " + creds.toBase64());
    }

    auto* reply = nam_->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [=]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            cb(false, QJsonValue(reply->errorString()));
            return;
        }
        auto doc = QJsonDocument::fromJson(reply->readAll());
        if (doc.isObject() && doc.object().contains("result"))
            cb(true, doc.object()["result"]);
        else
            cb(false, QJsonValue(doc.object()["error"].toObject()["message"].toString()));
    });
}

void AiToolExecutor::httpGet(const QUrl& url,
                              std::function<void(bool, const QJsonDocument&)> cb)
{
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, "DineroWallet/2.0");
    auto* reply = nam_->get(req);
    connect(reply, &QNetworkReply::finished, this, [=]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            cb(false, QJsonDocument());
            return;
        }
        cb(true, QJsonDocument::fromJson(reply->readAll()));
    });
}

// ──────────────────────────────────────────────────────────────
// Tool schema definitions
// ──────────────────────────────────────────────────────────────

// Returns the tools list as Anthropic-format tool objects
QJsonArray AiToolExecutor::anthropicToolDefs()
{
    auto tool = [](const QString& name, const QString& desc, const QJsonObject& props,
                   const QJsonArray& required = {}) {
        return QJsonObject{
            {"name", name}, {"description", desc},
            {"input_schema", QJsonObject{
                {"type","object"}, {"properties", props}, {"required", required}
            }}
        };
    };

    return QJsonArray{
        tool("get_din_balance",
             "Get the user's Dinero wallet balance in both transparent and private lanes.",
             {}),
        tool("get_node_status",
             "Get Dinero node sync status, peer count, block height, and network info.",
             {}),
        tool("get_receive_address",
             "Get a fresh transparent DIN address to receive funds.",
             {}),
        tool("shield_din",
             "Move DIN from the transparent lane to the private/confidential lane (increases privacy).",
             QJsonObject{{"amount_din", QJsonObject{{"type","number"},
                          {"description","Amount of DIN to shield (e.g. 1.5)"}}}},
             QJsonArray{"amount_din"}),
        tool("unshield_din",
             "Move DIN from the private/confidential lane back to transparent.",
             QJsonObject{
                 {"amount_din", QJsonObject{{"type","number"},{"description","Amount to unshield"}}},
                 {"to_address", QJsonObject{{"type","string"},{"description","Transparent din1p... address"}}}
             }, QJsonArray{"amount_din"}),
        tool("send_din",
             "Send DIN to a transparent address.",
             QJsonObject{
                 {"to_address", QJsonObject{{"type","string"},{"description","Recipient din1p... address"}}},
                 {"amount_din", QJsonObject{{"type","number"},{"description","Amount in DIN"}}}
             }, QJsonArray{"to_address","amount_din"}),
        tool("get_crypto_prices",
             "Get live USD prices and 24h change for any cryptocurrencies. Supports btc, eth, xmr, sol, bnb, din, and many others.",
             QJsonObject{{"coins", QJsonObject{{"type","array"},
                          {"items",QJsonObject{{"type","string"}}},
                          {"description","List of coin tickers or names. Examples: btc, eth, xmr, din, sol"}}}},
             QJsonArray{}),
        tool("get_market_overview",
             "Get total crypto market cap, BTC dominance, 24h volume, and market trend.",
             {}),
        tool("get_wdin_info",
             "Get info about wDIN (Wrapped Dinero) — the ERC-20 token on Base blockchain tradable on Uniswap.",
             {}),
    };
}

// OpenAI-compatible format (Groq, Ollama)
QJsonArray AiToolExecutor::openaiToolDefs()
{
    QJsonArray result;
    for (const auto& v : anthropicToolDefs()) {
        auto a = v.toObject();
        QJsonObject fn;
        fn["name"]        = a["name"];
        fn["description"] = a["description"];
        fn["parameters"]  = a["input_schema"];
        result.append(QJsonObject{{"type","function"}, {"function", fn}});
    }
    return result;
}

// Gemini format
QJsonArray AiToolExecutor::geminiToolDefs()
{
    QJsonArray decls;
    for (const auto& v : anthropicToolDefs()) {
        auto a = v.toObject();
        QJsonObject fn;
        fn["name"]        = a["name"];
        fn["description"] = a["description"];
        fn["parameters"]  = a["input_schema"];
        decls.append(fn);
    }
    return QJsonArray{QJsonObject{{"function_declarations", decls}}};
}
