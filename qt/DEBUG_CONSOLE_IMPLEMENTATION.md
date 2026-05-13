# Debug Console - Live Log Viewer Implementation

## Overview
This document describes the Live Log Viewer implementation for Dinero-Qt GUI, providing real-time visibility into daemon, miner, and GUI activities.

## ✅ Completed Implementation

### 1. Debug Console Widget (`debugconsole.h` + `debugconsole.cpp`)

**Features Implemented:**
- Three-tab interface (Daemon | Miner | GUI Logs)
- Color-coded log levels:
  - DEBUG: Gray
  - INFO: Green
  - WARNING: Orange
  - ERROR: Red
- Per-tab controls:
  - Log level filtering (DEBUG/INFO/WARNING/ERROR)
  - Pause/Resume auto-scroll
  - Clear logs button
  - Export to file
- Auto-scrolling with pause capability
- 10,000 line buffer per tab (prevents memory issues)
- Monospace font for readability
- Timestamp formatting `[HH:MM:SS]`

### 2. API for Logging

**Public Methods:**
```cpp
// Log to specific source
void logMessage(LogSource source, LogLevel level, const QString& message);

// Convenience methods
void logDaemonOutput(const QString& output);   // Auto-detects log level
void logMinerOutput(const QString& output);     // Auto-detects log level
void logGuiEvent(LogLevel level, const QString& event);
```

**Log Level Detection:**
- Automatically parses output for keywords (ERROR, WARN, DEBUG)
- Falls back to INFO if no keywords detected

### 3. Export Functionality
- Exports plain text logs to file
- Default filename: `dinero_{daemon|miner|gui}_logs_YYYYMMDD_HHMMSS.txt`
- Includes all filtered logs visible in current tab

## 🔧 Remaining Integration Steps

### Step 1: Add to CMakeLists.txt

Edit `/Users/haydarevich/Documents/DineroCoin/gui/CMakeLists.txt`:

```cmake
# Find line with GUI_SOURCES and add:
set(GUI_SOURCES
  src/main.cpp
  src/rpcclient.cpp
  src/rpcclient.h
  src/mainwindow.cpp
  src/mainwindow.h
  src/walletwizard.cpp
  src/walletwizard.h
  src/QrUtil.cpp
  src/QrUtil.h
  src/websocketclient.cpp
  src/websocketclient.h
  src/debugconsole.cpp      # ADD THIS
  src/debugconsole.h        # ADD THIS
)
```

### Step 2: Add to MainWindow Header

Edit `/Users/haydarevich/Documents/DineroCoin/gui/src/mainwindow.h`:

```cpp
// Add near top with other includes:
#include "debugconsole.h"

// Add in private section near other member variables:
private:
  DebugConsole* debugConsole_;  // Live log viewer window
```

### Step 3: Initialize in MainWindow Constructor

Edit `/Users/haydarevich/Documents/DineroCoin/gui/src/mainwindow.cpp` constructor:

```cpp
MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      rpc_(new RpcClient(this)),
      ws_(new WebSocketClient("ws://127.0.0.1:20997", this)),
      // ... other initializations ...
      debugConsole_(nullptr)  // ADD THIS
{
  setupUI();

  // Initialize debug console (hidden by default)
  debugConsole_ = new DebugConsole();
  debugConsole_->hide();

  // Log initial startup
  debugConsole_->logGuiEvent(DebugConsole::LogLevel::INFO,
                              "Dinero-Qt started");

  // ... rest of constructor ...
}
```

### Step 4: Add Menu Item

Edit `setupUI()` in `/Users/haydarevich/Documents/DineroCoin/gui/src/mainwindow.cpp`:

```cpp
// Find where menuBar is created and add:
QMenu* windowMenu = menuBar()->addMenu("&Window");

QAction* showDebugConsoleAction = new QAction("&Debug Console", this);
showDebugConsoleAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D));
showDebugConsoleAction->setStatusTip("Show live logs from daemon, miner, and GUI");

connect(showDebugConsoleAction, &QAction::triggered, this, [this]() {
  if (debugConsole_) {
    debugConsole_->show();
    debugConsole_->raise();
    debugConsole_->activateWindow();
  }
});

windowMenu->addAction(showDebugConsoleAction);
```

### Step 5: Capture Daemon Output

Edit `startDaemon()` in mainwindow.cpp to capture stdout/stderr:

```cpp
// Find where daemonProcess_ is started and modify:
daemonProcess_->setProcessChannelMode(QProcess::MergedChannels);  // Combine stdout+stderr

// Add signal handler for daemon output:
connect(daemonProcess_, &QProcess::readyReadStandardOutput, this, [this]() {
  if (daemonProcess_ && debugConsole_) {
    QString output = QString::fromUtf8(daemonProcess_->readAllStandardOutput());

    // Send each line to debug console
    QStringList lines = output.split('\n', Qt::SkipEmptyParts);
    for (const QString& line : lines) {
      debugConsole_->logDaemonOutput(line);
    }
  }
});
```

### Step 6: Connect Miner Output

Edit `startMining()` in mainwindow.cpp:

```cpp
// Find where miningProcess_ output is captured and add:
connect(miningProcess_, &QProcess::readyReadStandardOutput, this, [this]() {
  if (!miningProcess_ || !txtMiningOutput_) {
    return;
  }
  QString output = QString::fromUtf8(miningProcess_->readAllStandardOutput());

  // Show in Mining tab (existing functionality)
  txtMiningOutput_->append(output);

  // ALSO send to debug console
  if (debugConsole_) {
    QStringList lines = output.split('\n', Qt::SkipEmptyParts);
    for (const QString& line : lines) {
      debugConsole_->logMinerOutput(line);
    }
  }
});
```

### Step 7: Add GUI Event Logging

Sprinkle throughout mainwindow.cpp wherever significant events occur:

```cpp
// Examples:
debugConsole_->logGuiEvent(DebugConsole::LogLevel::INFO, "Wallet created successfully");
debugConsole_->logGuiEvent(DebugConsole::LogLevel::WARNING, "Failed to connect to daemon");
debugConsole_->logGuiEvent(DebugConsole::LogLevel::ERROR, "RPC call failed: " + error);
debugConsole_->logGuiEvent(DebugConsole::LogLevel::DEBUG, "Refreshing wallet balance");
```

## 📝 Usage Instructions (For Users)

### Opening Debug Console
- **Menu**: Window → Debug Console
- **Keyboard**: `Ctrl+Shift+D`

### Tabs
1. **Daemon Logs**: Real-time output from `dinerod` subprocess
   - P2P connections, block validation, sync progress

2. **Miner Logs**: Real-time output from `dinero-miner` subprocess
   - Hashrate, nonce attempts, blocks found

3. **GUI Logs**: Application events
   - Wallet operations, RPC calls, errors

### Controls (per tab)
- **Log Level Filter**: Choose minimum severity to display
- **Pause Scroll**: Freeze auto-scroll to review logs
- **Clear**: Wipe current tab's log history
- **Export**: Save logs to text file

## 🎨 Example Log Output

```
[10:39:38] [INFO ] Daemon started - Dinero v0.1.0
[10:39:39] [INFO ] Loading blockchain from ~/.dinero
[10:39:40] [INFO ] Blockchain height: 1,234 blocks
[10:39:40] [INFO ] Connecting to seed node 172.93.160.131:20999...
[10:39:41] [INFO ] Connected to peer 172.93.160.131:20999
[10:39:42] [INFO ] Received block 1,235 from peer
[10:39:42] [DEBUG] Validating block 1,235...
[10:39:42] [INFO ] Block 1,235 accepted
[10:39:50] [INFO ] Sync status: 45.2% (1,250/2,765 blocks)
[10:39:55] [WARN ] Peer 173.249.195.59:20999 timeout, reconnecting...
[10:40:01] [ERROR] Failed to validate block 1,256: invalid merkle root
```

## 🔍 Troubleshooting

**No daemon logs showing:**
- Verify daemon is actually running: Check "Overview" tab connection status
- Ensure `daemonProcess_` stdout/stderr capture is connected (Step 5)

**Miner logs missing:**
- Check that miner is running (Mining tab should show output)
- Verify miner output connection is added (Step 6)

**GUI logs sparse:**
- Normal - only significant events are logged
- Add more `debugConsole_->logGuiEvent()` calls as needed

## 🚀 Future Enhancements

- [ ] Search/filter by text
- [ ] Save filters/preferences
- [ ] Tail -f style auto-refresh from daemon's debug.log file
- [ ] Network packet inspector
- [ ] RPC call tracer with request/response pairs

---

**Status**: Core implementation complete, integration pending
**Next Step**: Follow integration steps above to wire into MainWindow
