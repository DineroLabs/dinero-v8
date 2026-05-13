# Testnet Deployment Analysis (Dinero)

## 1) Executive summary

We will stand up a public Dinero Testnet to validate networking, consensus, wallet flows, and founder-control logic with real nodes. Scope covers: bootstrapping, node configs, security, observability, smoke tests, and a rollback plan.

## 2) Network parameters (proposed)

- **Network name:** dinero-testnet
- **Address HRP:** `tdin` (confirm constant in SelectParams(TESTNET); regtest uses rdin)
- **Default ports**:
  - P2P: 21000 (regtest uses 21001)
  - RPC: 20998 (regtest uses 20996)
- **Difficulty/retarget:** inherit testnet rules as in code (allow easy-mining on low hashrate).
- **Premine/founder control on testnet:** enabled but routed to a faucet (single hot wallet, multisig cold backup). Rationale: simplifies distribution for testers while exercising the control path.

## 3) Node types & minimum specs

- **Seed/Bootstrap nodes (2–3):** public IP / DNS, open P2P 21000/tcp, hardened SSH.
- **Validators/Miners (N):** same binary; mining optional.
- **Developer nodes:** local laptops/VMs; RPC bound to localhost.
- **Baseline spec:** 2 vCPU, 2–4 GB RAM, 20 GB SSD, Linux x86_64 (Ubuntu 22.04+) or macOS (Apple Silicon supported).

## 4) Binaries & build

Build with CMake (as in your logs), static-links for RocksDB/OpenSSL/JsonCpp confirmed.

**Artifacts:**
- `dinerod` (daemon)
- `dinero-cli` (CLI)

macOS bundle audit passes (✅ dinerod clean in your build). For Linux, ship tarball with install.sh that:
- places binaries in `/usr/local/bin`
- creates user `dinero`
- sets up `/var/lib/dinero-testnet` and `/var/log/dinero-testnet`
- installs systemd unit (below)

## 5) Configuration

### 5.1 dinero-testnet.conf (server)

```ini
# ~/.dinero-testnet/dinero.conf  (or /etc/dinero-testnet/dinero.conf)
testnet=1
server=1

# Networking
port=21000
rpcport=20998
rpcbind=127.0.0.1
# If you must allow remote RPC via VPN/jumpbox, add a single allow list (avoid 0.0.0.0/0):
# rpcallowip=10.0.0.0/16

# Seeds (replace with real DNS names)
addnode=seed1.testnet.dinero.org
addnode=seed2.testnet.dinero.org

# Founder control / faucet
# (Code enforces founder-control; set the faucet address here if supported by config,
# else ensure TESTNET params set it at compile-time.)
# founderpremineaddress=tdin1q...faucet

# Logging
logtimestamps=1
debug=1
```

### 5.2 systemd unit (Linux)

```ini
# /etc/systemd/system/dinerod-testnet.service
[Unit]
Description=Dinero Testnet Daemon
After=network-online.target
Wants=network-online.target

[Service]
User=dinero
Group=dinero
Type=simple
ExecStart=/usr/local/bin/dinerod -testnet -datadir=/var/lib/dinero-testnet -server=1 -daemon=0 -rpcbind=127.0.0.1 -port=21000 -rpcport=20998
Restart=on-failure
RestartSec=3
LimitNOFILE=1048576

# Hardening
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=full
ProtectHome=true

[Install]
WantedBy=multi-user.target
```

## 6) Security posture

- **RPC auth:** cookie-based by default (.cookie in datadir). Keep rpcbind=127.0.0.1. If remote RPC is needed, require VPN + mTLS reverse proxy (e.g., Caddy/NGINX) rather than rpcallowip to the internet.
- **P2P:** only port 21000/tcp open to world on seeds/validators. Firewall everything else.
- **Binaries:** distribute checksums + optional cosign attestations.
- **Secrets:** faucet hot wallet on hardened host; cold multisig for reserves.

## 7) Observability

- **Logs:** `${DATADIR}/testnet/debug.log`. Ship logrotate:
  - rotate daily, keep 7, compress, size 50M.
- **Health check (lightweight):**
  ```bash
  dinero-cli -testnet getblockcount
  dinero-cli -testnet getbestblockhash
  ```
- **Exporters:**
  - simple cron-to-metrics or a tiny HTTP wrapper exposing tip height and mempool size.
- **Dashboards:** Grafana panels for tip height, peer count, block interval histogram.

## 8) Launch plan

1. Prepare seeds with final binary & config; open 21000/tcp.
2. Publish DNS seed1/seed2.testnet.dinero.org → seed IPs.
3. Generate testnet genesis (if separate from main/test in code) or confirm SelectParams(TESTNET) uses the intended constants.
4. Start seeds, verify mutual connectivity & that tips advance when mined.
5. Release client tarball + quickstart doc.
6. Announce bootstrap instructions to testers.

## 9) Acceptance criteria

- Nodes can join with only testnet=1 + shipped seeds and fully sync from zero.
- sendtoaddress works end-to-end; tx relays and confirms within expected block interval.
- Founder-control/premine on testnet is spendable by faucet and enforced by validation.
- No ERROR/EXCEPTION lines appear during baseline run (start → mine → tx → shutdown).
- Network maintains ≥2 peers per node and recovers from seed restarts.

## 10) Rollback & recovery

- Keep ZFS/EBS snapshots (or tar backups) of seed datadirs pre-launch.
- If a parameter bug is found early, reset testnet:
  - Announce deprecation height, stop mining, snapshot logs, bump protocol/testnet magic if necessary, relaunch.
- Faucet compromise plan: rotate to new faucet address; broadcast notice; optionally invalidate old outputs via policy (testnet only) if code supports it, otherwise drain and continue.

## 11) Known issues / polish

- **Startup log mismatch:** sometimes prints Latest block: height=0 right after premine. Gate this message on actual tip read to avoid confusion.
- **Cookie path:** ensure we always create the datadir early; your latest runs show "Generated new cookie"/"Loaded existing cookie" ✅
- **Hardcoded loopback peers:** replace 127.0.0.1 dev peers with real testnet seeds under TESTNET params.

## 12) Testnet smoke script (two-node)

Drop-in script adapted from your regtest v2—ports/flags flipped to testnet:

```bash
#!/usr/bin/env bash
set -euo pipefail

A_DIR=/tmp/dinero-testnet-a
B_DIR=/tmp/dinero-testnet-b
A_RPC=20998
B_RPC=23988
A_P2P=21000
B_P2P=23989

RPC() { ./build/bin/dinero-cli -testnet -datadir="$1" "$2" "${@:3}"; }

wait_for_rpc() {
  local dir="$1"
  for _ in {1..100}; do
    if RPC "$dir" getblockcount >/dev/null 2>&1; then return 0; fi
    sleep 0.2
  done
  echo "RPC not ready" >&2; exit 1
}

echo "=== Clean ==="
pkill -f dinerod || true
rm -rf "$A_DIR" "$B_DIR"

echo "=== Start A ==="
./build/bin/dinerod -testnet -datadir="$A_DIR" -server=1 -rpcport=$A_RPC -port=$A_P2P -daemon
wait_for_rpc "$A_DIR"

echo "=== Mine 3 on A ==="
RPC "$A_DIR" setgenerate true 3
sleep 1
RPC "$A_DIR" getblockcount

echo "=== Start B (connect to A) ==="
./build/bin/dinerod -testnet -datadir="$B_DIR" -server=1 -rpcport=$B_RPC -port=$B_P2P -connect=127.0.0.1:$A_P2P -daemon
wait_for_rpc "$B_DIR"
sleep 1

HA=$(RPC "$A_DIR" getblockcount)
HB=$(RPC "$B_DIR" getblockcount)
echo "Heights A/B: $HA / $HB"

ADDR=$(RPC "$B_DIR" getnewaddress)
TXID=$(RPC "$A_DIR" sendtoaddress "$ADDR" 1)
echo "TX: $TXID"

RPC "$A_DIR" setgenerate true 1
sleep 1
CONF=$(RPC "$B_DIR" gettransaction "$TXID" | jq -r '.confirmations // 0')
echo "Confirmations on B: $CONF"

RPC "$A_DIR" stop
RPC "$B_DIR" stop
echo "✅ Testnet smoke OK"
```

## 13) Quick triage tips (if your v2 script stalls)

- Tail the log while the script runs:
  ```bash
  tail -f /tmp/dinero-regtest/regtest/debug.log
  ```
- If Node B height lags, force-connect via `-connect=127.0.0.1:<A_P2P>` (already in the script).
- If jq isn't present on a fresh machine, install it or skip the JSON checks (the script tolerates missing jq inside the if but it's nice-to-have).
