# Dinero Mining Guide

## Architecture: Daemon + External Miner

Dinero uses a **clean separation** between consensus and mining:

```
┌──────────────┐         RPC          ┌───────────────┐
│   dinerod    │ ←──getblocktemplate──│ dinero-miner  │
│  (daemon)    │ ───submitblock─────→ │   (worker)    │
└──────────────┘                      └───────────────┘
     Node                                  Hashing
     Consensus                             No risk
     Wallet                                Optimizable
```

**Benefits:**
- ✅ Daemon stays lean and stable
- ✅ Mine from any machine
- ✅ No wallet unlock needed
- ✅ Easy to update/optimize
- ✅ GUI just spawns miner process

## Quick Start

### 1. Start the Daemon

```bash
./dinerod -datadir=./data
```

Daemon will:
- Create genesis block
- Start RPC on `127.0.0.1:20998`
- Generate `.cookie` file for auth

### 2. Get a Mining Address

```bash
ADDR=$(curl -s -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"getnewaddress"}' \
  http://127.0.0.1:20998/ | jq -r .result)

echo "Mining to: $ADDR"
```

### 3. Start Mining

```bash
./dinero-miner \
  --address "$ADDR" \
  --threads 4 \
  --datadir ./data
```

**Auto-detected:**
- Thread count (uses all CPU cores)
- Cookie file (from datadir)
- RPC endpoint (localhost:20998)

## Mining Options

```bash
dinero-miner [options]

Options:
  --rpc <url>       RPC endpoint (default: http://127.0.0.1:20998/)
  --address <addr>  Mining payout address (din1...) [REQUIRED]
  --threads <n>     Number of threads (default: auto-detect)
  --cookie <path>   Path to .cookie file (default: auto-detect)
  --datadir <path>  Data directory for cookie auto-detection
```

## Examples

### Solo Mining (Default)

```bash
# Mine to your wallet
./dinero-miner --address din1q...

# Custom thread count
./dinero-miner --address din1q... --threads 8

# Remote daemon
./dinero-miner \
  --address din1q... \
  --rpc http://192.168.1.100:20998/ \
  --cookie /path/to/.cookie
```

### Mining Stats

Miner outputs:
```
⛏️  2.45 MH/s | Total: 1234 MH | Blocks: 5
```

Check daemon:
```bash
curl -s -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"getmininginfo"}' \
  http://127.0.0.1:20998/ | jq
```

## GUI Integration

### Start Mining (Qt/GUI)

```cpp
// In your GUI code:
QProcess *miner = new QProcess(this);

QStringList args;
args << "--address" << ui->payoutAddressEdit->text();
args << "--threads" << QString::number(ui->threadSpinBox->value());
args << "--datadir" << dataDir;

// Connect signals
connect(miner, &QProcess::readyReadStandardOutput, this, [=]() {
    QString output = miner->readAllStandardOutput();
    // Parse for hashrate: "⛏️  2.45 MH/s"
    ui->hashrateLabel->setText(parseHashrate(output));
});

connect(miner, &QProcess::errorOccurred, this, [=](QProcess::ProcessError error) {
    QMessageBox::warning(this, "Mining Error", 
        "Failed to start miner: " + miner->errorString());
});

// Start
miner->start("./dinero-miner", args);
ui->startMiningButton->setEnabled(false);
ui->stopMiningButton->setEnabled(true);
```

### Stop Mining

```cpp
void MainWindow::onStopMining() {
    if (miner && miner->state() == QProcess::Running) {
        miner->terminate();
        miner->waitForFinished(3000);
        if (miner->state() == QProcess::Running) {
            miner->kill(); // Force if needed
        }
    }
    ui->startMiningButton->setEnabled(true);
    ui->stopMiningButton->setEnabled(false);
}
```

## Security Best Practices

### ✅ Do This

- **Keep RPC on localhost** - Never expose to internet
- **Use cookie authentication** - Auto-generated, secure
- **Mine to a real address** - From `getnewaddress`
- **Monitor miner process** - Check it's actually running

### ❌ Don't Do This

- **Don't expose RPC publicly** - Port 20998 is localhost-only
- **Don't hardcode addresses** - Use RPC to generate
- **Don't keep wallet unlocked** - Mining doesn't need it
- **Don't trust unverified miners** - Build from source

## Performance Tips

### Thread Count

```bash
# Auto-detect (recommended)
--threads 0   # Uses all cores

# Manual tuning
--threads 4   # Leave some cores for system

# Check what you have:
sysctl -n hw.ncpu  # macOS
nproc              # Linux
```

### CPU Affinity (Linux)

```bash
# Pin to specific cores
taskset -c 0-7 ./dinero-miner --address din1q...
```

### Priority (macOS/Linux)

```bash
# Lower priority (nice to system)
nice -n 10 ./dinero-miner --address din1q...

# Higher priority (use with caution)
sudo nice -n -5 ./dinero-miner --address din1q...
```

## Troubleshooting

### "Failed to get block template"

**Problem:** Can't connect to daemon

**Solutions:**
1. Check daemon is running: `ps aux | grep dinerod`
2. Check RPC is listening: `netstat -an | grep 20998`
3. Verify cookie exists: `ls -la data/.cookie`
4. Use `-dev` mode (no auth): `./dinerod -dev`

### "Address must start with 'din1'"

**Problem:** Invalid payout address

**Solution:**
```bash
# Get a valid address
curl -s -d '{"method":"getnewaddress"}' http://127.0.0.1:20998/
```

### Low Hashrate

**Problem:** Only getting kH/s instead of MH/s

**Causes:**
- Wrong difficulty (Phase 1 is easy)
- CPU throttling (thermal)
- Too many threads (context switching)

**Solutions:**
- Reduce threads: `--threads $(($(nproc)-2))`
- Check temps: `sensors` (Linux) or Activity Monitor (macOS)
- Use Phase 1 difficulty: easier targets

## Mining Economics

### Phase 1 (Blocks 1-200,000)
- **Reward:** 100 DIN per block
- **Difficulty:** 0x2100ffff (CPU-friendly)
- **Target:** ~20M DIN total

### Phase 2 (Blocks 200,001+)
- **Initial Reward:** 50 DIN
- **Halving:** Every 790,000 blocks
- **Difficulty:** 0x1d00ffff (Bitcoin-level)
- **Target:** ~79M DIN total

### Check Current Phase

```bash
curl -s -d '{"method":"geteconomics"}' http://127.0.0.1:20998/ | jq
```

## Future Enhancements

### Planned Features
- [ ] Block template caching
- [ ] Actual block mining (currently placeholder)
- [ ] Stratum protocol support
- [ ] GPU miner version
- [ ] Pool mining mode

### Want to Contribute?

The miner is designed to be simple and hackable:
- `tools/dinero_miner.cpp` - Main miner code
- `tools/genesis_miner.cpp` - Proven hashing logic
- Pull requests welcome!

---

## Summary: Why External Miner?

**This is the RIGHT architecture:**
1. **Separation of concerns** - Mining ≠ Consensus
2. **Security** - Miner has no consensus code
3. **Flexibility** - Mine from anywhere
4. **Simplicity** - GUI just spawns process
5. **Maintainability** - Update independently

**Industry standard:**
- Bitcoin: bitcoind + external miners
- Ethereum: geth + external miners
- Monero: monerod + xmrig

**Dinero follows best practices.** ✅
