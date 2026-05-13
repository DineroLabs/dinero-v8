#include <QtCore>
#include <QCoreApplication>
#include <QStringList>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonJson::Value>
#include <QUrl>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <cstdio>
#include <regex>
#include <algorithm>
#include <filesystem>
#include "rpc_api.h"
#include "ws_client.h"
#include "url_parser.h"

namespace fs = std::filesystem;

static void printJson(const QJsonJson::Value& v) {
    QJsonDocument d;
    if (v.isObject()) {
        d = QJsonDocument(v.toObject());
    } else if (v.isArray()) {
        d = QJsonDocument(v.toArray());
    } else {
        // For primitive values, wrap in an object
        d = QJsonDocument(QJsonObject{{"value", v}});
    }
    printf("%s\n", d.toJson(QJsonDocument::Indented).constData());
}

// OS-specific datadir resolution
static std::string getOSDefaultDataDir() {
#ifdef __APPLE__
    const char* home = getenv("HOME");
    if (home) {
        return std::string(home) + "/Library/Application Support/Dinero";
    }
    return "/tmp/dinero";
#elif defined(_WIN32)
    const char* appdata = getenv("APPDATA");
    if (appdata) {
        return std::string(appdata) + "\\Dinero";
    }
    return "C:\\Dinero";
#else
    const char* home = getenv("HOME");
    if (home) {
        return std::string(home) + "/.dinero";
    }
    return "/tmp/dinero";
#endif
}

// Network detection from arguments
static std::string detectNetwork(const std::vector<std::string>& args) {
    for (const std::string& arg : args) {
        if (arg == "-regtest" || arg == "--regtest") return "regtest";
        if (arg == "-testnet" || arg == "--testnet") return "testnet";
    }
    return "mainnet";
}

// Simple path join utility
static std::string joinPath(const std::string& base, const std::string& sub) {
    if (base.empty()) return sub;
    if (sub.empty()) return base;
    
    char sep = '/';
#ifdef _WIN32
    sep = '\\';
#endif
    
    if (base.back() == sep) {
        return base + sub;
    }
    return base + sep + sub;
}

// Resolve datadir with proper precedence
static std::string resolveDataDir(const std::vector<std::string>& args, const std::string& explicitDatadir) {
    if (!explicitDatadir.empty()) {
        return explicitDatadir;
    }
    
    std::string network = detectNetwork(args);
    std::string osDefault = getOSDefaultDataDir();
    
    return joinPath(osDefault, network);
}

// parseUrl implementation moved to url_parser.cpp

// Enhanced NodeInfo structure
struct NodeInfo {
    QString rpcUrl;
    QString wsUrl;
    QString cookiePath;
    QString network;
    QString datadir;
    bool valid = false;
    bool fromNodeinfo = false;
};

// Load nodeinfo.json with proper path resolution
static NodeInfo loadNodeInfo(const QString& datadir, const QString& explicitNodeinfo = QString()) {
    NodeInfo info;
    info.datadir = datadir;
    
    QString nodeinfoPath = explicitNodeinfo.isEmpty() ? 
        (datadir + "/nodeinfo.json") : explicitNodeinfo;
    
    QFile file(nodeinfoPath);
    if (!file.open(QIODevice::ReadOnly)) {
        // Fallback to default cookie path
        info.cookiePath = datadir + "/.cookie";
        info.valid = QFile::exists(info.cookiePath);
        return info;
    }
    
    QByteArray data = file.readAll();
    QJsonDocument doc = QJsonDocument::fromJson(data);
    
    if (!doc.isObject()) {
        info.cookiePath = datadir + "/.cookie";
        info.valid = QFile::exists(info.cookiePath);
        return info;
    }
    
    QJsonObject obj = doc.object();
    info.fromNodeinfo = true;
    
    info.valid = true;
    info.network = obj["network"].toString();
    
    // Extract RPC URL
    if (obj.isMember("rpc") && obj["rpc"].isObject()) {
        QJsonObject rpc = obj["rpc"].toObject();
        if (rpc.isMember("url")) {
            info.rpcUrl = rpc["url"].toString();
        }
    }
    
    // Extract WebSocket URL
    if (obj.isMember("ws") && obj["ws"].isObject()) {
        QJsonObject ws = obj["ws"].toObject();
        if (ws.isMember("url")) {
            info.wsUrl = ws["url"].toString();
        }
    }
    
    // Resolve paths relative to nodeinfo.json directory
    QString nodeinfoDir = QFileInfo(nodeinfoPath).absolutePath();
    
    if (obj.isMember("cookie")) {
        QString cookiePath = obj["cookie"].toString();
        if (QFileInfo(cookiePath).isRelative()) {
            // For daemon-generated relative paths, resolve relative to nodeinfo.json's parent directory
            // This handles cases where daemon writes "test-ephemeral/.cookie" but means "./.cookie"
            if (cookiePath.isMember('/')) {
                // Extract just the filename part for daemon-generated paths
                QString filename = QFileInfo(cookiePath).fileName();
                info.cookiePath = QDir(nodeinfoDir).absoluteFilePath(filename);
            } else {
                // Normal relative path resolution
                info.cookiePath = QDir(nodeinfoDir).absoluteFilePath(cookiePath);
            }
        } else {
            info.cookiePath = cookiePath;
        }
    }
    
    if (obj.isMember("datadir")) {
        QString datadirPath = obj["datadir"].toString();
        if (QFileInfo(datadirPath).isRelative()) {
            // Resolve relative to nodeinfo.json directory
            info.datadir = QDir(nodeinfoDir).absoluteFilePath(datadirPath);
        } else {
            info.datadir = datadirPath;
        }
    }
    
    info.valid = !info.rpcUrl.isEmpty() && QFile::exists(info.cookiePath);
    return info;
}

static void printNodeInfo(const NodeInfo& info) {
    printf("NodeInfo Discovery:\n");
    printf("  RPC URL:     %s\n", info.rpcUrl.isEmpty() ? "(none)" : qPrintable(info.rpcUrl));
    printf("  WS URL:      %s\n", info.wsUrl.isEmpty() ? "(none)" : qPrintable(info.wsUrl));
    printf("  Cookie:      %s\n", info.cookiePath.isEmpty() ? "(none)" : qPrintable(info.cookiePath));
    printf("  Network:     %s\n", info.network.isEmpty() ? "(auto)" : qPrintable(info.network));
    printf("  Datadir:     %s\n", qPrintable(info.datadir));
    printf("  Source:      %s\n", info.fromNodeinfo ? "nodeinfo.json" : "defaults");
    printf("  Status:      %s\n", info.valid ? "Valid" : "Invalid");
}

// Doctor command for comprehensive diagnostics
static int runDoctorCommand(const NodeInfo& nodeInfo, const QUrl& httpUrl, const QString& cookiePath) {
    printf("Dinero CLI Doctor - Comprehensive Diagnostics\n");
    printf("================================================\n\n");
    
    int exitCode = 0;
    
    // 1. Resolved endpoint
    QString host = httpUrl.host();
    int port = httpUrl.port();
    if (port == -1) port = 20998;
    
    printf("🔗 Connection Endpoint:\n");
    printf("  Target:      %s:%d\n", qPrintable(host), port);
    printf("  Source:      %s\n", nodeInfo.valid ? "nodeinfo" : "defaults");
    printf("  URL:         %s\n", qPrintable(httpUrl.toString()));
    
    // 2. Cookie validation
    printf("\n🍪 Authentication:\n");
    printf("  Cookie Path: %s\n", qPrintable(cookiePath));
    
    QFileInfo cookieInfo(cookiePath);
    if (!cookieInfo.exists()) {
        printf("  Status:      ❌ MISSING\n");
        printf("  Issue:       Cookie file not found\n");
        exitCode = 4;
    } else {
        QFile::Permissions perms = cookieInfo.permissions();
        bool readable = perms & QFile::ReadOwner;
        bool worldReadable = (perms & QFile::ReadGroup) || (perms & QFile::ReadOther);
        
        printf("  Status:      %s\n", readable ? "✅ Found" : "❌ Unreadable");
        printf("  Permissions: %s\n", worldReadable ? "❌ Too permissive" : "✅ Owner-only");
        printf("  Size:        %lld bytes\n", cookieInfo.size());
        
        if (!readable) {
            printf("  Issue:       Cannot read cookie file\n");
            exitCode = 4;
        } else if (worldReadable) {
            printf("  Issue:       Cookie readable by others (security risk)\n");
            exitCode = 4;
        }
    }
    
    // 3. Network and nodeinfo
    printf("\n🌐 Configuration:\n");
    printf("  Network:     %s\n", nodeInfo.network.isEmpty() ? "mainnet" : qPrintable(nodeInfo.network));
    printf("  Datadir:     %s\n", qPrintable(nodeInfo.datadir));
    printf("  NodeInfo:    %s\n", nodeInfo.fromNodeinfo ? "✅ Discovered" : "⚠️  Using defaults");
    
    if (nodeInfo.fromNodeinfo) {
        QString nodeinfoPath = nodeInfo.datadir + "/nodeinfo.json";
        QFileInfo nodeinfoInfo(nodeinfoPath);
        printf("  NodeInfo Path: %s\n", qPrintable(nodeinfoPath));
        printf("  NodeInfo Age:  %s\n", nodeinfoInfo.exists() ? 
               qPrintable(nodeinfoInfo.lastModified().toString()) : "Missing");
    }
    
    // 4. RPC connectivity test
    printf("\n🚀 RPC Connectivity:\n");
    printf("  Testing connection to %s:%d...\n", qPrintable(host), port);
    
    // Simple connection test using Qt networking
    QByteArray auth = readCookieBasicAuth(cookiePath);
    if (auth.isEmpty() && cookieInfo.exists()) {
        printf("  Auth:        ❌ Failed to read cookie\n");
        exitCode = 4;
    } else {
        RpcClientLite client(httpUrl, auth);
        RpcReply reply = client.call("getbestblockhash", QJsonArray{});
        
        if (reply.success) {
            printf("  Status:      ✅ Connected\n");
            printf("  Latency:     %dms\n", reply.latencyMs);
            printf("  Response:    %s\n", qPrintable(reply.result.toString().left(16) + "..."));
        } else {
            printf("  Status:      ❌ Failed\n");
            printf("  Error:       %s (code %d)\n", qPrintable(reply.message), reply.code);
            
            if (reply.code == 401) {
                exitCode = 4; // Auth error
            } else if (reply.code == -1) {
                exitCode = 3; // Connection error
            } else {
                exitCode = 5; // RPC error
            }
        }
    }
    
    // 5. WebSocket reachability (if available)
    printf("\n🔌 WebSocket Reachability:\n");
    if (!nodeInfo.wsUrl.isEmpty()) {
        printf("  WS URL:      %s\n", qPrintable(nodeInfo.wsUrl));
        // TODO: Add WebSocket connectivity test
        printf("  Status:      ⚠️  Not tested (WebSocket test not implemented)\n");
    } else {
        printf("  Status:      ⚠️  No WebSocket URL configured\n");
    }
    
    // Summary
    printf("\n📋 Summary:\n");
    if (exitCode == 0) {
        printf("  Overall:     ✅ All systems operational\n");
        printf("  Recommendation: CLI is ready for use\n");
    } else {
        printf("  Overall:     ❌ Issues detected (exit code %d)\n", exitCode);
        printf("  Recommendation: Fix the issues above before using CLI\n");
    }
    
    return exitCode;
}

static void printUsage() {
    fprintf(stderr,
        "Usage: dinero-cli [options] <command> [params]\n"
        "\n"
        "Transport Options:\n"
        "  --transport {http,ws,auto}  Transport mode (default: auto)\n"
        "  --rpc-url URL               RPC endpoint URL (default: http://127.0.0.1:20998/)\n"
        "  --http-url URL              Alias for --rpc-url\n"
        "  --ws-url URL                WebSocket RPC endpoint (default: ws://127.0.0.1:21001/ws)\n"
        "  --cookie PATH               Cookie file path (default: auto-detect from datadir)\n"
        "  --datadir PATH              Data directory (auto-discovers URLs from nodeinfo.json)\n"
        "  --nodeinfo PATH             Explicit nodeinfo.json path\n"
        "  --print-nodeinfo            Print discovered configuration and exit\n"
        "  -v, --verbose               Show connection details\n"
        "\n"
        "Environment Variables:\n"
        "  DINERO_RPC_URL              Default RPC endpoint URL\n"
        "  DINERO_COOKIE               Default cookie file path\n"
        "  DINERO_NODEINFO             Default nodeinfo.json path\n"
        "  DINERO_DATADIR              Default data directory\n"
        "\n"
        "Precedence: flags > env vars > nodeinfo > defaults\n"
        "\n"
        "Diagnostic Commands:\n"
        "  doctor                      Run comprehensive diagnostics\n"
        "\n"
        "Port Reference:\n"
        "  HTTP RPC: 20998\n"
        "  P2P:      20999\n"
        "  WS RPC:   21001  (JSON-RPC over WebSocket, path=/ws, Basic auth via .cookie)\n"
        "\n"
        "Network Info:\n"
        "  Block Time: 8.67 minutes (520 seconds)\n"
        "  Retarget: ~8.67 hours (60 blocks)\n"
        "  CPU-Friendly Phase: 3 years (18M DIN at 99 DIN/block)\n"
        "\n"
        "Wallet Commands:\n"
        "  wallet.info                    Get wallet information\n"
        "  wallet.create <name>           Create a new wallet\n"
        "  wallet.load <name>             Load an existing wallet\n"
        "  wallet.unload <name>           Unload a wallet\n"
        "  wallet.getnewaddress           Generate a new address\n"
        "  wallet.listaddresses           List all addresses\n"
        "\n"
        "Mining Commands:\n"
        "  mining.info                    Get mining information\n"
        "  mining.start [threads]         Start mining (default: 1 thread)\n"
        "  mining.stop                    Stop mining\n"
        "  mining.setpayoutaddress <addr> Set mining payout address\n"
        "\n"
        "Blockchain Commands:\n"
        "  getblockchaininfo              Get blockchain information\n"
        "  getblockcount                  Get current block count\n"
        "  getbestblockhash               Get best block hash\n"
        "  getblock <hash> [verbosity]    Get block by hash (verbosity: 0-2, default: 2)\n"
    );
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QStringList args = QCoreApplication::arguments();

    // Environment variable support with precedence: flags > env > nodeinfo > defaults
    QString envRpcUrl = qEnvironmentVariable("DINERO_RPC_URL");
    QString envCookie = qEnvironmentVariable("DINERO_COOKIE");
    QString envNodeinfo = qEnvironmentVariable("DINERO_NODEINFO");
    QString envDatadir = qEnvironmentVariable("DINERO_DATADIR");

    // Default values (lowest precedence)
    QString transport = "auto";

    // Detect network from args to set correct default port
    std::vector<std::string> stdArgs;
    for (int i = 0; i < argc; ++i) {
        stdArgs.push_back(argv[i]);
    }
    std::string network = detectNetwork(stdArgs);

    // Set default RPC port based on network
    // Mainnet: 20998, Testnet: 20998, Regtest: 20996
    int defaultRpcPort = 20998;  // mainnet
    int defaultWsPort = 21001;
    if (network == "regtest") {
        defaultRpcPort = 20996;
        defaultWsPort = 18881;
    } else if (network == "testnet") {
        defaultRpcPort = 20998;
        defaultWsPort = 18081;
    }

    QString defaultRpcUrl = QString("http://127.0.0.1:%1/").arg(defaultRpcPort);
    QString defaultWsUrl = QString("ws://127.0.0.1:%1/ws").arg(defaultWsPort);

    QUrl httpUrl(envRpcUrl.isEmpty() ? defaultRpcUrl : envRpcUrl);
    QUrl wsUrl(defaultWsUrl);
    QString explicitCookie = envCookie;
    QString explicitDatadir = envDatadir;
    QString explicitNodeinfo = envNodeinfo;
    bool printNodeInfoOnly = false;

    // Parse command line arguments
    QStringList commands;
    for (int i = 1; i < args.size(); ++i) {
        QString arg = args[i];
        
        // Handle --flag=value format
        if (arg.isMember('=')) {
            QStringList parts = arg.split('=', Qt::KeepEmptyParts);
            if (parts.size() >= 2) {
                QString flag = parts[0];
                QString value = parts.mid(1).join('='); // Handle values with = in them
                
                if (flag == "--transport") {
                    transport = value;
                    continue;
                } else if (flag == "--http-url") {
                    httpUrl = QUrl(value);
                    continue;
                } else if (flag == "--ws-url") {
                    wsUrl = QUrl(value);
                    continue;
                } else if (flag == "--cookie" || flag == "-cookie") {
                    explicitCookie = value;
                    continue;
                } else if (flag == "--datadir" || flag == "-datadir") {
                    explicitDatadir = value;
                    continue;
                } else if (flag == "--nodeinfo" || flag == "-nodeinfo") {
                    explicitNodeinfo = value;
                    continue;
                } else if (flag == "--rpc" || flag == "--rpc-url") {
                    httpUrl = QUrl(value);
                    continue;
                }
            }
        }
        
        // Handle --flag value format
        if (arg == "--transport" && i + 1 < args.size()) {
            transport = args[++i];
        } else if (arg == "--http-url" && i + 1 < args.size()) {
            httpUrl = QUrl(args[++i]);
        } else if (arg == "--ws-url" && i + 1 < args.size()) {
            wsUrl = QUrl(args[++i]);
        } else if ((arg == "--cookie" || arg == "-cookie") && i + 1 < args.size()) {
            explicitCookie = args[++i];
        } else if ((arg == "--datadir" || arg == "-datadir") && i + 1 < args.size()) {
            explicitDatadir = args[++i];
        } else if ((arg == "--nodeinfo" || arg == "-nodeinfo") && i + 1 < args.size()) {
            explicitNodeinfo = args[++i];
        } else if (arg == "--print-nodeinfo") {
            printNodeInfoOnly = true;
        } else if ((arg == "--rpc" || arg == "--rpc-url") && i + 1 < args.size()) {
            // Legacy compatibility and --rpc-url support
            httpUrl = QUrl(args[++i]);
        } else if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        } else {
            // Collect remaining arguments as command and parameters
            commands = args.mid(i);
            break;
        }
    }

    // Resolve datadir with proper OS-specific defaults
    QString resolvedDatadir = resolveDataDir(args, explicitDatadir);
    
    // Load configuration with proper precedence
    NodeInfo nodeInfo = loadNodeInfo(resolvedDatadir, explicitNodeinfo);
    
    if (printNodeInfoOnly) {
        printNodeInfo(nodeInfo);
        return nodeInfo.valid ? 0 : 1;
    }
    
    // Apply discovered configuration if valid and not explicitly overridden
    QString cookie = explicitCookie.isEmpty() ? nodeInfo.cookiePath : explicitCookie;
    
    // Verbose connection logging for debugging
    bool verbose = false;
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == "-v" || args[i] == "--verbose") {
            verbose = true;
            break;
        }
    }
    
    // Track if URLs were explicitly provided
    bool httpUrlExplicit = false;
    bool wsUrlExplicit = false;
    
    // Check if URLs were explicitly set (not defaults)
    for (int i = 1; i < args.size(); ++i) {
        QString arg = args[i];
        if (arg.startsWith("--http-url") || arg.startsWith("--rpc-url") || arg == "--rpc") {
            httpUrlExplicit = true;
        }
        if (arg.startsWith("--ws-url")) {
            wsUrlExplicit = true;
        }
    }
    
    QString connectionSource = "defaults";
    if (nodeInfo.valid && !nodeInfo.rpcUrl.isEmpty()) {
        // Only override URLs if not explicitly provided
        if (!httpUrlExplicit) {
            httpUrl = QUrl(nodeInfo.rpcUrl);
            connectionSource = "nodeinfo";
        }
        if (!wsUrlExplicit && !nodeInfo.wsUrl.isEmpty()) {
            wsUrl = QUrl(nodeInfo.wsUrl);
        }
    }
    
    if (httpUrlExplicit) {
        connectionSource = "explicit";
    }
    
    // Verbose connection logging
    if (verbose) {
        QString host = httpUrl.host();
        int port = httpUrl.port();
        if (port == -1) port = 20998; // Default port if not specified
        fprintf(stderr, "dialing %s:%d (source=%s)\n", 
                qPrintable(host), port, qPrintable(connectionSource));
    }

    // HTTPS validation - fail fast with clear error
    if (httpUrl.scheme() == "https") {
        fprintf(stderr, "Error: HTTPS not supported. Use http:// or configure a TLS reverse proxy.\n");
        fprintf(stderr, "Example: Use 'http://127.0.0.1:20998/' instead of 'https://...'\n");
        return 2;
    }

    // Handle doctor command before other processing
    if (!commands.isEmpty() && commands[0] == "doctor") {
        return runDoctorCommand(nodeInfo, httpUrl, cookie);
    }

    if (commands.isEmpty()) {
        printUsage();
        return 1;
    }

    // Determine transport mode
    bool useWebSocket = false;
    if (transport == "ws") {
        useWebSocket = true;
    } else if (transport == "auto") {
        // Try WebSocket first, fall back to HTTP
        Auth auth = Auth::fromCookie(cookie);
        if (auth.isValid() && WsClient::isAvailable(wsUrl.toString().toStdString())) {
            useWebSocket = true;
        }
    }

    RpcReply r;
    
    if (useWebSocket) {
        // WebSocket transport
        Auth auth = Auth::fromCookie(cookie);
        if (!auth.isValid()) {
            fprintf(stderr, "Failed to read cookie at %s\n", qPrintable(cookie));
            return 2;
        }

        // Build JSON-RPC request
        QJsonObject request;
        request["jsonrpc"] = "2.0";
        request["method"] = commands[0];
        request["id"] = 1;
        
        if (commands.size() > 1) {
            QJsonArray params;
            fprintf(stderr, "[CLI DEBUG] Building params, commands.size=%d\n", commands.size());
            for (int i = 1; i < commands.size(); ++i) {
                QString arg = commands[i];
                fprintf(stderr, "[CLI DEBUG] Processing arg[%d]: %s\n", i, arg.toStdString().c_str());

                // Try to parse as JSON (object or array)
                if ((arg.startsWith("{") && arg.endsWith("}")) ||
                    (arg.startsWith("[") && arg.endsWith("]"))) {
                    QJsonParseError parseError;
                    QJsonDocument argDoc = QJsonDocument::fromJson(arg.toUtf8(), &parseError);

                    fprintf(stderr, "[CLI DEBUG] Attempting to parse JSON: %s\n", arg.toStdString().c_str());
                    fprintf(stderr, "[CLI DEBUG] Parse error: %s\n", parseError.errorString().toStdString().c_str());

                    if (parseError.error == QJsonParseError::NoError) {
                        // Successfully parsed JSON
                        fprintf(stderr, "[CLI DEBUG] Successfully parsed! isObject=%d isArray=%d\n",
                                argDoc.isObject(), argDoc.isArray());
                        if (argDoc.isObject()) {
                            params.append(argDoc.object());
                        } else if (argDoc.isArray()) {
                            params.append(argDoc.array());
                        } else {
                            params.append(arg);  // Fallback to string
                        }
                    } else {
                        // Not valid JSON, treat as string
                        params.append(arg);
                    }
                } else {
                    // Not JSON-like, treat as string (or try to parse as number/bool)
                    bool isNumber;
                    double num = arg.toDouble(&isNumber);
                    if (isNumber) {
                        params.append(num);
                    } else if (arg == "true") {
                        params.append(true);
                    } else if (arg == "false") {
                        params.append(false);
                    } else {
                        params.append(arg);
                    }
                }
            }
            request["params"] = params;
        }

        QJsonDocument doc(request);
        std::string json_request = doc.toJson(QJsonDocument::Compact).toStdString();

        try {
            std::string response = WsClient::call(wsUrl.toString().toStdString(), auth.header(), json_request);
            
            // Parse response
            QJsonDocument responseDoc = QJsonDocument::fromJson(QByteArray::fromStdString(response));
            QJsonObject responseObj = responseDoc.object();
            
            if (responseObj.isMember("error") && !responseObj["error"].isNull()) {
                QJsonObject error = responseObj["error"].toObject();
                r.code = error["code"].toInt();
                r.message = error["message"].toString();
                r.ok = false;
            } else {
                r.result = responseObj["result"];
                r.ok = true;
            }
        } catch (const std::exception& e) {
            fprintf(stderr, "WebSocket error: %s\n", e.what());
            return 3;
        }
    } else {
        // HTTP transport (existing code)
        const QByteArray auth = readCookieBasicAuth(cookie);
        if (auth.isEmpty()) {
            fprintf(stderr, "Failed to read cookie at %s\n", qPrintable(cookie));
            return 2;
        }

        RpcClientLite rpc(httpUrl, auth);
        Api api{rpc};
        api.detect();

        const QString cmd = commands[0];

        auto fail = [](const RpcReply& r) {
            fprintf(stderr, "error(%d): %s\n", r.code, qPrintable(r.message));
            return r.ok ? 0 : 3;
        };

        // Wallet commands
        if (cmd == "wallet.info") {
            r = api.walletInfo();
        } else if (cmd == "wallet.create" && commands.size() >= 2) {
            r = api.walletCreate(commands[1]);
        } else if (cmd == "wallet.load" && commands.size() >= 2) {
            r = api.walletLoad(commands[1]);
        } else if (cmd == "wallet.unload" && commands.size() >= 2) {
            r = api.walletUnload(commands[1]);
        } else if (cmd == "wallet.getnewaddress") {
            r = api.walletGetNewAddress();
        } else if (cmd == "wallet.listaddresses") {
            r = api.walletListAddresses();
        
        // Mining commands
        } else if (cmd == "mining.info") {
            r = api.miningInfo();
        } else if (cmd == "mining.start") {
            int threads = (commands.size() >= 2) ? commands[1].toInt() : 1;
            r = api.miningStart(threads);
        } else if (cmd == "mining.stop") {
            r = api.miningStop();
        } else if (cmd == "mining.setpayoutaddress" && commands.size() >= 2) {
            r = api.miningSetPayout(commands[1]);
        
        // Blockchain commands
        } else if (cmd == "getblockchaininfo") {
            r = api.getBlockchainInfo();
        } else if (cmd == "getblockcount") {
            r = api.getBlockCount();
        } else if (cmd == "getbestblockhash") {
            r = api.getBestBlockHash();
        } else if (cmd == "getblock" && commands.size() >= 2) {
            int verbosity = (commands.size() >= 3) ? commands[2].toInt() : 2;
            r = api.getBlock(commands[1], verbosity);
        } else {
            fprintf(stderr, "Unknown or malformed command: %s\n", qPrintable(cmd));
            printUsage();
            return 1;
        }
    }

    auto fail = [](const RpcReply& r) {
        fprintf(stderr, "error(%d): %s\n", r.code, qPrintable(r.message));
        return r.ok ? 0 : 3;
    };

    if (!r.ok) return fail(r);
    printJson(r.result);
    return 0;
}
