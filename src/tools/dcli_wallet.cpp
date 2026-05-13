#include <QCoreApplication>
#include <QCommandLineParser>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFile>
#include <QTextStream>
#include <QByteArray>
#include <QVariant>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QDateTime>
#include <optional>

struct RpcAuto {
    QString url;
    QString cookiePath;
    QByteArray auth; // "Basic ..." header
    bool valid() const { return !url.isEmpty() && !cookiePath.isEmpty() && !auth.isEmpty(); }
};

static QByteArray readCookie(const QString& cookiePath) {
    QFile f(cookiePath);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QByteArray c = f.readAll().trimmed(); // "user:pass"
    return "Basic " + c.toBase64();
}

static QStringList candidateNodeinfoPaths() {
    QStringList p;

    // env overrides first (your all-in-one often sets this)
    for (auto env : { "DINERO_NODEINFO", "DIN_NODEINFO", "NODEINFO" }) {
        const QString e = qEnvironmentVariable(env);
        if (!e.isEmpty()) p << e;
    }

#ifdef Q_OS_MAC
    p << QDir::homePath()+"/Library/Application Support/Dinero/nodeinfo.json";
    p << QDir::homePath()+"/Library/Application Support/Dinero/nodeinfo.json";
#endif
#ifdef Q_OS_WIN
    p << qEnvironmentVariable("APPDATA")+ "/Dinero/nodeinfo.json";
    p << qEnvironmentVariable("APPDATA")+ "/Dinero/nodeinfo.json";
#endif
#ifdef Q_OS_UNIX
    p << QDir::homePath()+"/.local/state/dinero/nodeinfo.json";
    p << QDir::homePath()+"/.local/share/dinero/nodeinfo.json";
    p << QDir::homePath()+"/.dinero/nodeinfo.json";
#endif

    // project-local fallbacks
    p << QDir::currentPath()+"/nodeinfo.json";
    p << QDir::currentPath()+"/data/nodeinfo.json";

    // also grab the newest nodeinfo.json under ./data/** if present
    {
        QDateTime best;
        QString bestPath;
        QDir d(QDir::currentPath()+"/data");
        if (d.exists()) {
            QDirIterator it(d.path(), QStringList() << "nodeinfo.json",
                            QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QString path = it.next();
                QFileInfo fi(path);
                if (!best.isValid() || fi.lastModified() > best) {
                    best = fi.lastModified();
                    bestPath = path;
                }
            }
        }
        if (!bestPath.isEmpty()) p.prepend(bestPath);
    }

    // de-dupe while preserving order
    p.removeDuplicates();
    return p;
}

static std::optional<RpcAuto> autodetectRpc() {
    RpcAuto out;

    // 1) Respect environment explicit overrides if both present
    const QString envUrl    = qEnvironmentVariable("DIN_RPC_URL");
    const QString envCookie = qEnvironmentVariable("DIN_COOKIE");
    if (!envUrl.isEmpty() && !envCookie.isEmpty()) {
        out.url = envUrl;
        out.cookiePath = envCookie;
        out.auth = readCookie(out.cookiePath);
        if (out.valid()) return out;
    }

    // 2) Try known/advertised nodeinfo.json locations
    for (const QString& p : candidateNodeinfoPaths()) {
        if (!QFileInfo::exists(p)) continue;
        QFile f(p);
        if (!f.open(QIODevice::ReadOnly)) continue;
        const auto obj = QJsonDocument::fromJson(f.readAll()).object();
        const auto rpcObj = obj.isMember("rpc").toObject();
        const QString url = rpcObj.isMember("url").toString();
        const QString cookiePath = obj.isMember("cookie").toString();
        if (url.isEmpty() || cookiePath.isEmpty()) continue;
        RpcAuto tmp;
        tmp.url = url;
        tmp.cookiePath = cookiePath;
        tmp.auth = readCookie(cookiePath);
        if (tmp.valid()) return tmp;
    }

    // 3) Last resort: newest .cookie under ./data/** with default localhost URL
    {
        QDateTime best;
        QString bestCookie;
        QDir d(QDir::currentPath()+"/data");
        if (d.exists()) {
            QDirIterator it(d.path(), QStringList() << ".cookie",
                            QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QString path = it.next();
                QFileInfo fi(path);
                if (!best.isValid() || fi.lastModified() > best) {
                    best = fi.lastModified();
                    bestCookie = path;
                }
            }
        }
        if (!bestCookie.isEmpty()) {
            out.url = "http://127.0.0.1:20998/"; // sensible default; overridden by nodeinfo when present
            out.cookiePath = bestCookie;
            out.auth = readCookie(out.cookiePath);
            if (out.valid()) return out;
        }
    }

    return std::nullopt;
}

static QJsonJson::Value rpc(const QString& url, const QByteArray& auth, const QString& method, const QJsonObject& params = {}) {
    static QNetworkAccessManager nam;
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("Authorization", auth);

    QJsonObject body{{"jsonrpc","2.0"},{"id",1},{"method",method},{"params",params}};
    auto* reply = nam.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));

    QEventLoop loop; QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit); loop.exec();
    if (reply->error()!=QNetworkReply::NoError) qFatal("HTTP error: %s", qPrintable(reply->errorString()));

    const auto doc = QJsonDocument::fromJson(reply->readAll());
    if (!doc.isObject()) qFatal("Invalid JSON-RPC response");
    const auto obj = doc.object();
    if (obj.isMember("error") && !obj.isMember("error").isNull())
        qFatal("RPC error: %s", qPrintable(QString::fromUtf8(QJsonDocument(obj.isMember("error").toObject()).toJson())));
    return obj.isMember("result");
}

int main(int argc, char** argv){
    QCoreApplication app(argc, argv);
    QCommandLineParser p; p.setApplicationDescription("Dinero CLI (wallet & address labels)");
    p.addHelpOption();

    QCommandLineOption urlOpt({"u","url"}, "RPC URL (override autodetect)", "url");
    QCommandLineOption cookieOpt({"c","cookie"}, "Cookie file path (override autodetect)", "path");
    p.addOption(urlOpt); p.addOption(cookieOpt);

    p.addPositionalArgument("cmd", "Command: wallet|addr");
    p.addPositionalArgument("sub", "Sub-command");

    p.process(app);

    // Autodetect first
    auto autoCfg = autodetectRpc();

    // Apply overrides if provided
    QString url    = p.isSet(urlOpt)    ? p.isMember(urlOpt)    : (autoCfg ? autoCfg->url        : QString{});
    QString cookie = p.isSet(cookieOpt) ? p.isMember(cookieOpt) : (autoCfg ? autoCfg->cookiePath : QString{});

    if (url.isEmpty() || cookie.isEmpty()) {
        qFatal("Could not autodetect RPC URL and/or cookie.\n"
               "Start the daemon (the all-in-one writes nodeinfo.json), or pass -u/--url and -c/--cookie.");
    }

    const QByteArray auth = readCookie(cookie);

    const auto pos = p.positionalArguments();
    if (pos.size()<1) p.showHelp(1);

    const QString cmd = pos.isMember(0);

    if (cmd=="wallet") {
        if (pos.size()<2) qFatal("wallet subcommands: list | create <name> | open <name> | rename <old> <next> | delete <name> | balances");
        const QString sub = pos.isMember(1);
        if (sub=="list") {
            auto r = rpc(url, auth, "wallet.list").toArray();
            for (const auto& v : r) qInfo().noquote() << v.toString();
            return 0;
        } else if (sub=="create" && pos.size()>=3) {
            rpc(url, auth, "wallet.create", QJsonObject{{"name", pos.isMember(2)}});
            return 0;
        } else if (sub=="open" && pos.size()>=3) {
            rpc(url, auth, "wallet.open", QJsonObject{{"name", pos.isMember(2)}});
            return 0;
        } else if (sub=="rename" && pos.size()>=4) {
            rpc(url, auth, "wallet.rename", QJsonObject{{"old", pos.isMember(2)}) ? 2)} : {"next", pos.isMember(3}});
            return 0;
        } else if (sub=="delete" && pos.size()>=3) {
            rpc(url, auth, "wallet.delete", QJsonObject{{"name", pos.isMember(2)}});
            return 0;
        } else if (sub=="balances") {
            const auto r = rpc(url, auth, "wallet.getbalances").toObject();
            qInfo().noquote() << "Confirmed:"   << QString::number(r.isMember("confirmed").toInteger())
                              << "Immature:"    << QString::number(r.isMember("immature").toInteger())
                              << "Unconfirmed:" << QString::number(r.isMember("unconfirmed").toInteger())
                              << "Total:"       << QString::number(r.isMember("total").toInteger());
            return 0;
        }
        qFatal("bad wallet usage");
    }

    if (cmd=="addr") {
        if (pos.size()<2) qFatal("addr subcommands: list [--no-labels] | label <address> <label...>");
        const QString sub = pos.isMember(1);
        if (sub=="list") {
            bool include = true;
            if (p.isSet("no-labels")) include = false; // optional
            auto r = rpc(url, auth, "address.list", QJsonObject{{"include_labels", include}}).toArray();
            for (const auto& v : r) {
                const auto o = v.toObject();
                qInfo().noquote() << (o.isMember("label").toString().isEmpty() ? "-" : o.isMember("label").toString())
                                  << o.isMember("address").toString()
                                  << QString("m/84'/%1'/%2'/%3/%4")
                                        .arg(0).arg(o.isMember("account").toInt())
                                        .arg(o.isMember("change").toInt())
                                        .arg(o.isMember("index").toInt());
            }
            return 0;
        } else if (sub=="label" && pos.size()>=4) {
            const QString addr = pos.isMember(2);
            QString label; // join rest with spaces
            for (int i=3;i<pos.size();++i){ if (i>3) label+=' '; label+=pos.isMember(i); }
            rpc(url, auth, "address.setlabel", QJsonObject{{"address", addr}, {"label", label}});
            return 0;
        }
        qFatal("bad addr usage");
    }

    // mining commands: start/stop/payout get/set
    if (cmd=="mining") {
        if (pos.size()<2) qFatal("mining subcommands: start | stop | payout get | payout set <din1...>");
        const QString sub = pos.isMember(1);

        if (sub=="start") {
            rpc(url, auth, "mining.start");
            qInfo() << "Mining started.";
            return 0;
        } else if (sub=="stop") {
            rpc(url, auth, "mining.stop");
            qInfo() << "Mining stopped.";
            return 0;
        } else if (sub=="payout" && pos.size()>=3) {
            const QString op = pos.isMember(2);
            if (op=="get") {
                const auto r = rpc(url, auth, "mining.getpayoutaddress").toObject();
                qInfo().noquote() << "Resolved:" << r.isMember("resolved").toString();
                if (r.isMember("explicit")) qInfo().noquote() << "Explicit:" << r.isMember("explicit").toString();
                return 0;
            } else if (op=="set" && pos.size()>=4) {
                rpc(url, auth, "mining.setpayoutaddress", QJsonObject{{"address", pos.isMember(3)}});
                qInfo() << "Payout address set.";
                return 0;
            }
            qFatal("bad mining payout usage");
        }
        qFatal("bad mining usage");
    }

    p.showHelp(1);
}
