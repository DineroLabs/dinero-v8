# 🧱 Node Setup Checklist

### System Prep

- [x] Ubuntu 22.04+ or macOS 14+
- [x] GCC 11+ / Clang 15+
- [x] 2 GB RAM minimum
- [x] Ports 20999 (P2P), 20998 (RPC), 21001 (WS) open

### Node Installation

- [x] Clone repo and checkout tag `v0.1.0-consensus-lock`
- [x] Build with `cmake --build build --target dinerod -j8`
- [x] Run daemon with `--printtoconsole`
- [x] Verify output shows both genesis + premine PASSED

### Sync Validation

- [x] Run `dinero-cli getblockchaininfo`
- [x] Verify `bestblockhash` = `0000002bd3fa677b...934f`
- [x] Check `getmetrics` returns correct checksum
- [x] Cross-check node heights on both servers

### Security

- [x] Enable ufw firewall rules
- [x] Enable SSL for RPC/WS with Nginx
- [x] Backup `/root/.dinero` and `/root/.dinero/wallets`
- [x] Verify identical binaries via `sha256sum dinerod`

---

✅ **Launch condition:** All above boxes checked = mainnet ready

