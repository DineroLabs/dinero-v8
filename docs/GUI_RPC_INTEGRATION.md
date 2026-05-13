# Qt6 Desktop GUI - RPC Integration Roadmap

## 🎯 **Immediate GUI Wins (Feature → RPC Mapping)**

| GUI Feature | RPC Endpoint | Benefit |
|-------------|--------------|---------|
| **Always-correct tip & height** | `getbestblockhash`, `getblockcount`, `getblockchaininfo.bestblockhash` | No "N/A" flicker, instant accurate display |
| **Difficulty display** | `getblockchaininfo.{difficulty,difficulty_str}` | Human-readable + machine-precise values |
| **Genesis + block drill-downs** | `getblockheader`, `getblock` (verbosity 1/2) | Works at height 0, no "broken" empty chains |
| **Live status badge** | `/healthz` | Ready/Degraded + network + height display |
| **Ops/dev panel** | `/metrics` | Prometheus counters for diagnostics |
| **Uptime in status bar** | `uptime` | Professional daemon monitoring |
| **Network switch safety** | DB network validation + genesis match | Bulletproof network transitions |

## 🛡️ **Why the GUI Will Feel Rock-Solid**

### **No Placeholders Anywhere**
- UI renders real values on first paint
- No "N/A" or "Loading..." flicker
- Instant professional appearance

### **Genesis Seeded**
- Block/Header views work at height 0
- Empty chains don't look "broken"
- Consistent experience from genesis

### **Mathematical Consistency**
- Chainwork & nTx invariants enforced
- Block cards stay mathematically consistent
- No off-by-one bugs in displays

### **Auto-Heal Protection**
- Old data dirs silently corrected
- Fewer "why is my view wrong?" support tickets
- Self-healing consistency

### **Bitcoin-Style Verbosity**
- Drop-in parity with explorer patterns
- Switchable detail levels
- Familiar UX for Bitcoin users

## 📱 **Screens the Qt App Can Ship Now**

### **Dashboard Cards**
```cpp
// Network badge, Height, Tip (8 chars), Chainwork (8), Difficulty, Uptime
auto info = rpcCall("getblockchaininfo").value("result").toObject();
networkLabel->setText(info["chain"].toString().toUpper());
heightLabel->setText(QString::number(info["blocks"].toInt()));
tipLabel->setText(info["bestblockhash"].toString().left(8));
chainworkLabel->setText(info["chainwork"].toString().left(8));
difficultyLabel->setText(info["difficulty_str"].toString());

auto uptime = rpcCall("uptime").value("result").toInt();
uptimeLabel->setText(formatUptime(uptime));
```

### **Blocks List**
```cpp
// At height 0, show genesis row; later paginate via explorer DB
auto height = rpcCall("getblockcount").value("result").toInt();
for (int h = height; h >= 0 && h > height - 10; h--) {
    auto hash = rpcCall("getblockhash", QJsonArray{h}).value("result").toString();
    auto header = rpcCall("getblockheader", QJsonArray{hash, true}).value("result").toObject();
    addBlockRow(h, hash, header["time"].toInt(), header["nTx"].toInt());
}
```

### **Block Detail**
```cpp
// Header fields with Bitcoin-style verbosity
auto block = rpcCall("getblock", QJsonArray{hash, 1}).value("result").toObject();
versionLabel->setText(QString::number(block["version"].toInt()));
timeLabel->setText(QDateTime::fromSecsSinceEpoch(block["time"].toInt()).toString());
bitsLabel->setText(QString("0x%1").arg(block["bits"].toInt(), 8, 16, QChar('0')));
chainworkLabel->setText(block["chainwork"].toString());
prevLabel->setText(block["previousblockhash"].toString());
merkleLabel->setText(block["merkleroot"].toString());

// Transaction list (verbosity=1 shows txids, empty until coinbase wired)
auto txArray = block["tx"].toArray();
for (const auto& tx : txArray) {
    txListWidget->addItem(tx.toString());
}
```

### **Mempool Card (MVP)**
```cpp
// From getmempoolinfo (counts/sizes), even if 0
auto mempool = rpcCall("getmempoolinfo").value("result").toObject();
mempoolSizeLabel->setText(QString::number(mempool["size"].toInt()));
mempoolBytesLabel->setText(formatBytes(mempool["bytes"].toInt()));
mempoolUsageLabel->setText(formatBytes(mempool["usage"].toInt()));
```

### **Health/Diagnostics**
```cpp
// /healthz + metrics snippet for "Advanced" tab
auto health = httpGet("http://127.0.0.1:20998/healthz");
statusLabel->setText(health["status"] == "ok" ? "Ready" : "Degraded");
networkLabel->setText(health["network"].toString());
heightLabel->setText(QString::number(health["height"].toInt()));

// Auto-heal metrics
auto metrics = httpGet("http://127.0.0.1:20998/metrics");
// Parse Prometheus format for key counters
```

### **Network Switcher**
```cpp
// Toggle regtest/testnet/mainnet with validation
void switchNetwork(NetworkType newNet) {
    try {
        // Stop current daemon gracefully
        daemonManager->stop();
        
        // Validate target network database
        if (!validateNetworkDatabase(newNet)) {
            showToast("Network database validation failed", Toast::Warning);
            return;
        }
        
        // Start daemon for new network
        daemonManager->start(newNet);
        
        // Wait for health check
        if (!waitForHealthy(30000)) {
            showToast("Network switch failed - daemon not responding", Toast::Error);
            return;
        }
        
        // Update UI
        currentNetwork = newNet;
        refreshAllData();
        showToast(QString("Switched to %1").arg(networkName(newNet)), Toast::Success);
        
    } catch (const std::exception& e) {
        showToast(QString("Network switch error: %1").arg(e.what()), Toast::Error);
    }
}
```

## 🔧 **Tiny Glue Code (Qt6 Widgets)**

```cpp
// Minimal JSON-RPC call helper
QJsonObject rpcCall(QString method, QJsonArray params = {}) {
    QNetworkRequest r(QUrl("http://127.0.0.1:20998/"));
    r.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    
    // Load cookie for authentication
    QFile cookie("data/regtest/.cookie"); 
    cookie.open(QIODevice::ReadOnly);
    r.setRawHeader("Authorization", "Basic " + cookie.readAll().toBase64());
    
    // Build JSON-RPC request
    QJsonObject body{
        {"jsonrpc", "2.0"},
        {"id", "gui"},
        {"method", method},
        {"params", params}
    };
    
    // Synchronous request (use async in production)
    QNetworkAccessManager nm; 
    QEventLoop loop;
    auto reply = nm.post(r, QJsonDocument(body).toJson()); 
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit); 
    loop.exec();
    
    return QJsonDocument::fromJson(reply->readAll()).object();
}

// HTTP GET helper for /healthz and /metrics
QJsonObject httpGet(const QString& url) {
    QNetworkAccessManager nm;
    QEventLoop loop;
    auto reply = nm.get(QNetworkRequest(QUrl(url)));
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    
    return QJsonDocument::fromJson(reply->readAll()).object();
}

// Example: populate dashboard
void updateDashboard() {
    auto info = rpcCall("getblockchaininfo").value("result").toObject();
    auto best = rpcCall("getbestblockhash").value("result");
    auto up   = rpcCall("uptime").value("result");
    
    // Update UI elements
    networkBadge->setText(info["chain"].toString().toUpper());
    heightLabel->setText(QString::number(info["blocks"].toInt()));
    tipLabel->setText(best.toString().left(8) + "...");
    difficultyLabel->setText(info["difficulty_str"].toString());
    uptimeLabel->setText(formatDuration(up.toInt()));
    
    // Status indicator
    statusIndicator->setColor(info["initialblockdownload"].toBool() ? 
                             QColor(255, 165, 0) : QColor(40, 167, 69)); // Orange : Green
}
```

## 🚀 **Dev Velocity Boosters**

### **Unified Architecture**
- **Unified port + cookie auth** → Trivial wiring from Qt (no special cases)
- **Lowercase hex everywhere** → Stable string compares, fewer UI parsers
- **Consistent JSON responses** → Predictable parsing patterns

### **Built-in Reliability**
- **/metrics endpoint** → Cheap perf counters for "Dev Tools" pane
- **CI/auto-heal system** → GUI tests won't flake due to stale local DBs
- **Network validation** → Bulletproof network switching

### **Bitcoin Compatibility**
- **Standard verbosity levels** → Drop-in explorer patterns
- **Familiar RPC interface** → Existing Bitcoin tooling works
- **Production-grade error handling** → Clear error messages

## 📋 **Short "Next" List (Fast Wins)**

### **Immediate (< 1 day each)**
1. **Status bar** fed by `/healthz` + `uptime`
2. **Block Header dialog** using `getblockheader(hash, true)`
3. **Dashboard difficulty** using `difficulty_str` (keep numeric for tooltips)
4. **Refresh timer** (1s on regtest) to re-pull `getblockcount`/`getbestblockhash`

### **Near-term (< 1 week each)**
1. **Mempool monitoring** via `getmempoolinfo`
2. **Network switcher** with validation toasts
3. **Block list** with pagination
4. **Health diagnostics** panel

### **Future (when ready)**
1. **Raw-hex modes** → "Copy raw block/header" buttons
2. **Transaction details** when coinbase is wired
3. **Advanced metrics** dashboard
4. **Mining controls** when mining RPCs are available

## 🎯 **Result: Production-Grade Desktop App**

The backend work transforms the desktop app from "demo" into production-feeling:

- **✅ Instant** - Real data on first paint
- **✅ Correct** - Mathematical consistency enforced
- **✅ Bitcoin-familiar** - Standard RPC patterns
- **✅ Self-healing** - Built-in safety nets
- **✅ Trustworthy** - UI stays reliable over time

**Every RPC endpoint maps directly to a GUI feature that users will love!** 🚀
