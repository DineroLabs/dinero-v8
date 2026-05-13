# 🚀 Dinero Health Dashboard

A real-time monitoring and control interface for the Dinero cryptocurrency mining system.

## 🎯 Features

### 📊 Real-Time Monitoring
- **Blockchain Height**: Current block height with live updates
- **Hashrate**: Mining performance in H/s, kH/s, or MH/s
- **Difficulty**: Current mining difficulty in hex format
- **Target**: Mining target with canonical format validation
- **Mining Phase**: Current phase (Genesis, Developer Fund, CPU-Friendly, Halving)
- **Mining Status**: Active/Inactive mining state

### 🏥 Health Indicators
- **Daemon Health**: ✅ Responding / ❌ Not Responding
- **Target Format**: ✅ Canonical / ⚠️ Non-canonical
- **Hashrate Health**: ✅ Healthy (>100k H/s) / ⚠️ Low (<100k H/s)
- **Mining Health**: ✅ Active / ❌ Inactive

### ⛏️ Mining Controls
- **Start Mining**: Begin mining with 2 threads
- **Stop Mining**: Stop all mining operations
- **Refresh**: Manual status update

### 📝 Activity Log
- Real-time updates and status changes
- Error messages and network issues
- Timestamped log entries

## 🚀 Quick Start

### Prerequisites
- Dinero daemon running on `http://127.0.0.1:20998`
- Valid RPC cookie at `/Users/haydarevich/Documents/DineroCoin/data/.cookie`

### Launch Methods

#### Method 1: Direct Launch
```bash
./build/bin/dinero-health-dashboard
```

#### Method 2: Smart Launcher (Recommended)
```bash
./scripts/launch_health_dashboard.sh
```

#### Method 3: Command Line Operations
```bash
# Health check
./scripts/dinero_ops.sh health

# Start mining
./scripts/dinero_ops.sh start 4

# Stop mining
./scripts/dinero_ops.sh stop

# Watch real-time status
./scripts/dinero_ops.sh watch
```

## 🎨 User Interface

### Main Dashboard
- **Clean, modern design** with color-coded status indicators
- **Real-time updates** every 2 seconds
- **Responsive layout** that adapts to window size
- **Professional styling** with consistent color scheme

### Status Colors
- 🟢 **Green**: Healthy, Active, Canonical
- 🟡 **Orange**: Warning, Low performance
- 🔴 **Red**: Error, Inactive, Not responding

### Menu Bar
- **File**: Exit application
- **View**: Refresh status
- **Help**: Help and About information

## 🔧 Technical Details

### Architecture
- **Qt6 Framework**: Cross-platform GUI
- **JSON-RPC**: Communication with Dinero daemon
- **QNetworkAccessManager**: HTTP requests
- **QTimer**: Automatic updates
- **QJsonDocument**: JSON parsing

### Update Frequency
- **Automatic**: Every 2 seconds
- **Manual**: Click Refresh button or press F5
- **On-demand**: Start/Stop mining actions

### Error Handling
- **Network errors**: Graceful degradation with error messages
- **JSON parsing**: Robust error handling
- **Daemon connectivity**: Clear status indicators
- **Cookie authentication**: Automatic validation

## 📊 Monitoring Metrics

### Key Performance Indicators
1. **Hashrate**: Mining performance (target: >100k H/s)
2. **Block Height**: Blockchain growth rate
3. **Target Format**: Canonical Bitcoin format validation
4. **Mining Status**: Active mining confirmation
5. **Daemon Health**: Node connectivity

### Health Thresholds
- **Hashrate**: >100k H/s = Healthy, <100k H/s = Warning
- **Target**: `00002710...` = Canonical, other = Warning
- **Mining**: Active = Healthy, Inactive = Warning
- **Daemon**: Responding = Healthy, Not responding = Error

## 🛠️ Troubleshooting

### Common Issues

#### Dashboard Shows "Not Responding"
```bash
# Check if daemon is running
./scripts/dinero_ops.sh health

# Start daemon if needed
./build/bin/dinerod -datadir=/Users/haydarevich/Documents/DineroCoin/data -rpcport=20998 -port=20999
```

#### Cookie Authentication Failed
```bash
# Check cookie file exists
ls -la /Users/haydarevich/Documents/DineroCoin/data/.cookie

# Verify cookie format
cat /Users/haydarevich/Documents/DineroCoin/data/.cookie
```

#### Mining Not Starting
```bash
# Check mining status
./scripts/dinero_ops.sh info

# Verify daemon is mining-enabled
curl -s --user "$(tr -d '\r\n' < /Users/haydarevich/Documents/DineroCoin/data/.cookie)" \
  -H "content-type: application/json" \
  --data '{"jsonrpc":"2.0","id":1,"method":"getmininginfo","params":[]}' \
  "http://127.0.0.1:20998" | jq .
```

### Debug Mode
Enable verbose logging by setting environment variable:
```bash
export QT_LOGGING_RULES="*=true"
./build/bin/dinero-health-dashboard
```

## 🎯 Production Usage

### For Miners
- **Monitor performance** in real-time
- **Control mining** with one-click start/stop
- **Track progress** with block height and hashrate
- **Identify issues** with health indicators

### For Developers
- **Debug mining** with detailed status
- **Monitor network** health and connectivity
- **Validate algorithm** with canonical format checks
- **Test operations** with manual controls

### For Operations
- **Health monitoring** with automated checks
- **Performance tracking** with key metrics
- **Error detection** with clear indicators
- **Status reporting** with real-time updates

## 🔮 Future Enhancements

### Planned Features
- **Historical charts** for hashrate and difficulty
- **Block interval analysis** with statistics
- **Network peer monitoring** and connection status
- **Wallet balance** and transaction history
- **Mining pool** integration and statistics
- **Alert system** for critical issues
- **Export functionality** for logs and data

### Advanced Monitoring
- **Performance metrics** with detailed analytics
- **Resource usage** monitoring (CPU, memory)
- **Network latency** and connection quality
- **Block validation** and consensus status
- **Transaction processing** and mempool status

## 📚 Related Tools

### Command Line Tools
- `./scripts/dinero_ops.sh` - Command line operations
- `./scripts/health_dashboard.sh` - Terminal-based monitoring
- `./build/bin/test_production_smoke` - Production validation

### Core Components
- `./build/bin/dinerod` - Dinero daemon
- `./build/bin/dinero-qt6` - Full wallet GUI
- `./build/bin/dinero-cli` - Command line interface

## 🎉 Success Metrics

The Dinero Health Dashboard provides:
- ✅ **Real-time monitoring** of all critical systems
- ✅ **One-click mining control** for easy operation
- ✅ **Health validation** with canonical format checks
- ✅ **Professional interface** for production use
- ✅ **Comprehensive logging** for debugging
- ✅ **Error handling** with graceful degradation
- ✅ **Cross-platform compatibility** with Qt6
- ✅ **Production-ready** with robust architecture

**The Dinero cryptocurrency mining system is now fully operational with professional-grade monitoring and control!** 🚀⛏️
