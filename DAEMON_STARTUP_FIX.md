# Daemon Startup Fix - Authentication Issue Resolution

## Problem Identified

The GUI first-run dialog was appearing correctly, but after clicking "Start Daemon", authentication was failing with these errors:

```
Connection refused
AuthenticationRequiredError
Server http://127.0.0.1:20998/ failed 2 times, trying next server...
```

## Root Cause

**The daemon was being started WITHOUT specifying the datadir parameter**, which caused:

1. **Datadir mismatch**: Daemon defaulted to `~/.dinero` but may have been configured differently
2. **Cookie file location mismatch**: GUI looked for cookie in one location, daemon wrote it to another
3. **No daemon output capture**: Couldn't see why daemon was failing to start
4. **No error checking**: GUI didn't verify if daemon actually started successfully

## Fixes Implemented

### 1. Added Explicit Datadir Parameter (mainwindow.cpp:1336-1344)

```cpp
// CRITICAL: Use same datadir that RpcClient expects
QString datadir = QStandardPaths::writableLocation(QStandardPaths::HomeLocation) + "/.dinero";

daemonProcess_->setProgram(daemonPath);
QStringList args;
args << "-datadir=" + datadir;  // ⬅️ CRITICAL FIX
args << "-daemon";
daemonProcess_->setArguments(args);
```

### 2. Added Comprehensive Debug Logging (mainwindow.cpp:1329-1374)

```cpp
qDebug() << "✅ Found daemon at:" << daemonPath;
qDebug() << "🗂️ Using datadir:" << datadir;
qDebug() << "🚀 Starting daemon with command:" << daemonPath << args.join(" ");
qDebug() << "✅ Daemon process started successfully, PID:" << daemonProcess_->processId();
```

### 3. Added Daemon Output Capture (mainwindow.cpp:1347-1360)

```cpp
// Capture daemon output for debugging
daemonProcess_->setProcessChannelMode(QProcess::MergedChannels);
connect(daemonProcess_, &QProcess::readyReadStandardOutput, [this]() {
  QString output = daemonProcess_->readAllStandardOutput();
  qDebug() << "DAEMON OUTPUT:" << output;
});

connect(daemonProcess_, &QProcess::errorOccurred, [this](QProcess::ProcessError error) {
  qWarning() << "❌ Daemon process error:" << error;
});
```

### 4. Added Startup Verification (mainwindow.cpp:1365-1372)

```cpp
if (!daemonProcess_->waitForStarted(5000)) {
  qWarning() << "❌ Daemon failed to start!";
  QMessageBox::critical(this, "Startup Failed",
    "Failed to start daemon. Check console for details.");
  lblConnectionStatus_->setText("❌ Daemon start failed");
  return;
}
```

### 5. Extended Cookie Wait Time (mainwindow.cpp:1380)

Changed from 3 seconds to 5 seconds to give daemon more time to initialize and write cookie file.

## Testing Instructions

### Clean Test (Recommended)

1. **Stop any running daemons**:
   ```bash
   pkill -9 dinerod
   ```

2. **Clean datadir** (optional - for fresh test):
   ```bash
   rm -rf ~/.dinero
   ```

3. **Launch GUI and watch console output**:
   ```bash
   cd /Users/haydarevich/Documents/DineroCoin/build-gui
   ./dinero-qt.app/Contents/MacOS/dinero-qt
   ```

4. **Expected behavior**:
   - Welcome dialog appears: "🚀 Welcome to Dinero!"
   - Click "Start Daemon" button
   - Console shows:
     ```
     ✅ Found daemon at: /path/to/dinerod
     🗂️ Using datadir: /Users/haydarevich/.dinero
     🚀 Starting daemon with command: /path/to/dinerod -datadir=/Users/haydarevich/.dinero -daemon
     ✅ Daemon process started successfully, PID: 12345
     DAEMON OUTPUT: [initialization messages]
     ⏰ Attempting to load cookie and connect...
     ```
   - Status bar shows: "⏳ Starting daemon..." → "🔄 Connecting..." → "✅ Connected"
   - No more "Connection refused" or "Unauthorized" errors

### Verify Daemon is Running

```bash
# Check process
ps aux | grep dinerod

# Check cookie file exists
ls -la ~/.dinero/mainnet/.cookie

# Test RPC manually
COOKIE=$(cat ~/.dinero/mainnet/.cookie)
curl -s --user "$COOKIE" \
  -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"getblockcount","params":[]}' \
  http://127.0.0.1:20998
```

## Debug Console Output Reference

### Successful Startup Sequence:
```
✅ Found daemon at: /Users/haydarevich/Documents/DineroCoin/build-gui/dinero-qt.app/Contents/Resources/dinerod
🗂️ Using datadir: /Users/haydarevich/.dinero
🚀 Starting daemon with command: /path/to/dinerod -datadir=/Users/haydarevich/.dinero -daemon
✅ Daemon process started successfully, PID: 67890
DAEMON OUTPUT: Dinero Core Daemon v1.0.0
DAEMON OUTPUT: Initializing database...
DAEMON OUTPUT: Loading blockchain...
⏰ Attempting to load cookie and connect...
Loaded cookie from: /Users/haydarevich/.dinero/mainnet/.cookie
```

### Failed Startup:
```
❌ Daemon failed to start!
❌ Daemon process error: FailedToStart
```

## Architecture Summary

```
┌─────────────────────────────────────────────────┐
│ dinero-qt.app (GUI)                             │
│                                                 │
│  First Run Detection                            │
│       ↓                                         │
│  "Welcome to Dinero" Dialog                     │
│       ↓                                         │
│  [Start Daemon] clicked                         │
│       ↓                                         │
│  QProcess::start(dinerod, ["-datadir=...", "-daemon"])  │
│       ↓                                         │
│  Wait 5 seconds for cookie file                 │
│       ↓                                         │
│  RpcClient::loadCookie()                        │
│       ↓                                         │
│  RPC JSON-RPC 2.0 calls                         │
└─────────────────────────────────────────────────┘
           ↓
┌─────────────────────────────────────────────────┐
│ dinerod (Daemon)                                │
│                                                 │
│  Writes: ~/.dinero/mainnet/.cookie              │
│  Listens: http://127.0.0.1:20998                │
│  Stores blockchain in: ~/.dinero/mainnet/       │
└─────────────────────────────────────────────────┘
```

## Files Modified

1. **gui/src/mainwindow.cpp** (lines 1328-1385)
   - Added datadir parameter to daemon startup
   - Added comprehensive debug logging
   - Added daemon output capture
   - Added startup verification
   - Extended cookie wait time

2. **build-gui/dinero-qt.app** (rebuilt and rebundled)
   - Updated GUI binary with fixes
   - Re-signed with codesign

## Next Steps

If the daemon still fails to start:

1. **Check daemon binary permissions**:
   ```bash
   ls -la /Users/haydarevich/Documents/DineroCoin/build-gui/dinero-qt.app/Contents/Resources/dinerod
   chmod +x /Users/haydarevich/Documents/DineroCoin/build-gui/dinero-qt.app/Contents/Resources/dinerod
   ```

2. **Test daemon manually**:
   ```bash
   /Users/haydarevich/Documents/DineroCoin/build-gui/dinero-qt.app/Contents/Resources/dinerod \
     -datadir=/Users/haydarevich/.dinero \
     -daemon

   # Check output
   tail -f ~/.dinero/mainnet/debug.log
   ```

3. **Check for port conflicts**:
   ```bash
   lsof -i :20998
   ```

4. **Verify all dependencies are bundled**:
   ```bash
   otool -L /Users/haydarevich/Documents/DineroCoin/build-gui/dinero-qt.app/Contents/Resources/dinerod
   ```

## User Experience Improvement

Before fix:
```
❌ Connection refused
❌ Unauthorized
❌ Server http://127.0.0.1:20998/ failed
```

After fix:
```
✅ Welcome dialog with clear explanation
✅ One-click daemon startup
✅ Real-time status updates
✅ Helpful debug output in console
✅ Graceful error messages if startup fails
```

## Related Code References

- First-run dialog: `gui/src/mainwindow.cpp:1264-1395`
- Daemon shutdown: `gui/src/mainwindow.cpp:126-151`
- RPC client cookie loading: `gui/src/rpcclient.cpp` (loadCookie method)
- Bundle script: `bundle-app.sh:64-75` (daemon bundling)
