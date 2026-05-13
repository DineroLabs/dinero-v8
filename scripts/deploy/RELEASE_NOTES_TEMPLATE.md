# Dinero v1.0.0-mainnet - Official Launch 🚀

**Release Date:** [LAUNCH_DATE]  
**Network:** Mainnet  
**Status:** 🔥 **LIVE AND MINING** 🔥

## 🎉 Welcome to Dinero!

After months of development and testing, **Dinero cryptocurrency is now live on mainnet!** 

Dinero is the first **CPU-friendly cryptocurrency** designed to give everyone the thrill of mining without requiring expensive hardware. Our unique **phase-based algorithm** ensures fair distribution and sustainable growth.

## 🔒 Network Parameters (IMMUTABLE)

### Genesis Block
- **Genesis Hash:** `[GENESIS_HASH]`
- **Genesis Time:** `[GENESIS_TIME]` ([GENESIS_DATE])
- **Genesis Bits:** `[GENESIS_BITS]`
- **Genesis Nonce:** `[GENESIS_NONCE]`
- **Merkle Root:** `[MERKLE_ROOT]`

### Chain Identity  
- **Chain Identity Commitment (CIC):** `[CIC_HASH]`
- **Network Magic:** `0xD14E5201`
- **Address HRP:** `din` (Bech32)

### Network Ports
- **P2P Port:** `40999`
- **RPC Port:** `20999` (localhost only)
- **Health/Metrics:** `22001` (localhost only)

## 💰 Economics & Supply

### Currency Units
- **Ticker:** DIN
- **Decimals:** 6
- **Smallest Unit:** una (plural: una)
- **Ratio:** 1 DIN = 1,000,000 una

### Unit Examples
- 100.000000 DIN = 100,000,000 una
- 0.000001 DIN = 1 una
- Fees are quoted in una/kvB

### Total Supply
- **Maximum Supply:** 99,000,000 DIN
- **Developer Fund:** 2,000,000 DIN (2.02% premine)
- **Mining Rewards:** 97,000,000 DIN

### Mining Algorithm Phases
1. **Phase 1** (0-2M DIN): Developer fund premine
2. **Phase 2** (2M-20M DIN): CPU-friendly mining at 100 DIN/block
3. **Phase 3** (20M+ DIN): Bitcoin-level difficulty with halving every 210,000 blocks

### Block Parameters
- **Block Time:** 60 seconds (1 minute)
- **Difficulty Retarget:** Every 60 blocks (1 hour)
- **Coinbase Maturity:** 100 blocks (~1.67 hours)

## 🏗️ Developer Fund

### Multisig Treasury
- **Address:** `[DEV_FUND_ADDRESS]`
- **Type:** 2-of-3 Multisig P2WSH
- **Amount:** 2,000,000 DIN
- **Purpose:** Protocol development, security audits, ecosystem growth

### Transparency
- All treasury transactions are public on the blockchain
- Quarterly reports on fund usage will be published
- Community governance for major expenditures

## 🌐 Network Infrastructure

### DNS Seeds
- `seed.dinero-coin.com`
- `dnsseed.dinero-coin.com`

### Official Seed Nodes
- `seed1.dinero-coin.com:40999` (US-East)
- `seed2.dinero-coin.com:40999` (EU-West)  
- `seed3.dinero-coin.com:40999` (Asia-Pacific)

## 📦 Download & Installation

### Quick Install (Recommended)
```bash
# Download and verify
wget https://github.com/dinerocoin/dinero/releases/download/v1.0.0-mainnet/dinero-v1.0.0-mainnet-[PLATFORM]-release.tar.gz
wget https://github.com/dinerocoin/dinero/releases/download/v1.0.0-mainnet/SHA256SUMS.txt
wget https://github.com/dinerocoin/dinero/releases/download/v1.0.0-mainnet/SHA256SUMS.txt.asc

# Verify signatures
gpg --verify SHA256SUMS.txt.asc SHA256SUMS.txt
sha256sum --check SHA256SUMS.txt

# Extract and deploy
tar -xzf dinero-v1.0.0-mainnet-[PLATFORM]-release.tar.gz
cd dinero-v1.0.0-mainnet-[PLATFORM]-release
sudo ./scripts/deploy_mainnet.sh --full
```

### Available Packages
- **Linux x86_64:** `dinero-v1.0.0-mainnet-linux-x86_64-release.tar.gz`
- **Linux ARM64:** `dinero-v1.0.0-mainnet-linux-aarch64-release.tar.gz`
- **macOS Intel:** `dinero-v1.0.0-mainnet-darwin-x86_64-release.tar.gz`
- **macOS Apple Silicon:** `dinero-v1.0.0-mainnet-darwin-arm64-release.tar.gz`

### Build Variants
- **Release:** Production optimized
- **Release-ASAN:** With AddressSanitizer for debugging
- **Debug-ASAN:** Full debugging with sanitizers

## ⛏️ Start Mining

### Solo Mining (Recommended for CPU miners)
```bash
# Generate mining address
dinero-cli getnewaddress

# Start mining
./scripts/deploy/launch_mining.sh --address [YOUR_ADDRESS] --threads 4

# Monitor mining
tail -f ~/.dinero/mining/mining.log
```

### Mining Pools
Mining pools will be available soon. Check our website for updates.

## 🔧 Node Operations

### Run a Full Node
```bash
# Start node
sudo systemctl start dinerod

# Check status
dinero-cli getblockcount
dinero-cli getconnectioncount
curl http://127.0.0.1:22001/healthz
```

### RPC Commands
```bash
# Network status
dinero-cli getblockcount
dinero-cli getchaintips
dinero-cli getconnectioncount

# Mining info
dinero-cli getblocktemplate
dinero-cli getmininginfo

# Wallet operations
dinero-cli getnewaddress
dinero-cli getbalance
dinero-cli sendtoaddress [ADDRESS] [AMOUNT]
```

## 📊 Monitoring

### Prometheus Metrics
- Endpoint: `http://127.0.0.1:22001/metrics`
- Dashboard: Import `dinero_dashboard.json` into Grafana
- Alerts: Use `dinero_alerts.yml` with Alertmanager

### Health Checks
```bash
# HTTP health
curl http://127.0.0.1:22001/healthz

# Comprehensive check
./scripts/deploy/post_launch_checks.sh

# Continuous monitoring
./scripts/deploy/post_launch_checks.sh --continuous
```

## 🔐 Security

### Consensus Protection
- **Deep Reorg Protection:** Automatic safe-mode after 30+ block reorgs
- **Chain Identity Commitment:** Prevents parameter tampering
- **Signed Releases:** All binaries signed with GPG

### Operational Security
- RPC bound to localhost only
- Cookie-based authentication
- Systemd sandboxing
- Firewall configuration included

### Responsible Disclosure
Security vulnerabilities should be reported to: **security@dinero-coin.com**

## 🚀 What's Next

### Immediate (Week 1)
- [ ] Public testnet launch
- [ ] Basic block explorer
- [ ] Mining pool software
- [ ] Exchange integration docs

### Short-term (Month 1)
- [ ] Mobile wallet (iOS/Android)
- [ ] Hardware wallet support
- [ ] Payment processor integration
- [ ] Developer APIs

### Long-term (Quarter 1)
- [ ] Smart contract research
- [ ] Lightning Network integration
- [ ] Cross-chain bridges
- [ ] DeFi protocols

## 🤝 Community & Support

### Official Channels
- **Website:** https://dinero-coin.com
- **Documentation:** https://docs.dinero-coin.com
- **GitHub:** https://github.com/dinerocoin/dinero
- **Discord:** https://discord.gg/dinerocoin
- **Twitter:** https://twitter.com/dinerocoin

### Developer Resources
- **RPC Documentation:** https://docs.dinero-coin.com/rpc
- **API Reference:** https://docs.dinero-coin.com/api
- **Mining Guide:** https://docs.dinero-coin.com/mining
- **Node Setup:** https://docs.dinero-coin.com/nodes

### Contributing
We welcome contributions! See our [Contributing Guide](CONTRIBUTING.md) for details.

## 🔍 Technical Details

### Cryptographic Specifications
- **Hash Algorithm:** SHA-256 (double)
- **Address Format:** Bech32 with `din` HRP
- **Signature Scheme:** secp256k1 ECDSA
- **Merkle Trees:** Binary trees with SHA-256

### P2P Protocol
- **Network Magic:** `0xD14E5201`
- **Protocol Version:** `70001`
- **Default Port:** `40999`
- **Message Format:** Bitcoin-compatible

### Consensus Rules
- **Block Version:** `1`
- **Max Block Size:** 1MB
- **Max Transaction Size:** 100KB
- **Script Opcodes:** Bitcoin-compatible subset

## 📜 License

Dinero is released under the MIT License. See [LICENSE](LICENSE) for details.

## 🎯 Checksums

### Release Packages
```
[SHA256_CHECKSUMS_WILL_BE_HERE]
```

### GPG Signature
This release is signed with GPG key: `[GPG_KEY_ID]`

```
[GPG_SIGNATURE_WILL_BE_HERE]
```

---

## 🎉 Welcome to the Future of CPU-Friendly Mining!

**Dinero is now live!** Join thousands of miners earning DIN with just their CPU. No expensive ASICs, no energy waste - just fair, accessible cryptocurrency for everyone.

**Start mining today and be part of the revolution!** ⛏️💎

---

*This release represents months of development, testing, and community feedback. Thank you to everyone who made Dinero possible!*

**The Dinero Development Team**  
*[LAUNCH_DATE]*
