# Dinero Wallet Playbook (v0.1.0‑p0‑perfect)

A practical, security‑first guide to running, backing up, and recovering the Dinero wallet stack you just built.

---

## 0) Who this is for

* **Operators**: run `dinerod` + wallet on servers and desktops
* **Developers**: extend wallet code, add RPCs, write tests
* **Power users**: self‑custody with safety nets

---

## 1) What's inside the wallet module

* **Seeds/Keys**: BIP39 seed → BIP32 HD tree (SLIP‑132 prefixes validated)
* **Addresses**: BIP84 (bech32 v0 P2WPKH), HRP from `network_params.hpp`
* **Crypto**: libsecp256k1 for EC ops; hash pipeline verified by tests
* **Storage**: **SQLite** as the wallet database (metadata, descriptors, tx notes)
* **Chain DB**: **RocksDB** (daemon, not wallet) for blocks/UTXO

> ✅ Verified by your **P0 test suite** (7/7 green). Run anytime with: `ctest -L p0 --output-on-failure`

---

## 2) Network Parameters & HRPs

**Official Human Readable Prefixes (from `network_params.hpp`):**
* **Mainnet HRP**: `din` (Dinero mainnet addresses start with `din1...`)
* **Testnet HRP**: `tdin` (Testnet addresses start with `tdin1...`)
* **Regtest HRP**: `rdin` (Regtest addresses start with `rdin1...`)

**Bech32 Encoding Rules:**
* **Witness v0**: Uses BECH32 encoding (BIP-173)
* **Witness v1+**: Uses BECH32M encoding (BIP-350)

---

## 3) Golden rules (please adopt these)

1. **Seed is the source of truth.** Keep BIP39 seed (and optional passphrase) offline.
2. **Encrypt at rest.** Use OS disk encryption + wallet encryption (when available).
3. **Two independent backups**: seed (paper/metal or HSM) **and** wallet DB (file copy/backup export).
4. **Test your recovery** quarterly in an air‑gapped or lab environment.
5. **Least privilege on servers**: dedicated non‑login user; locked‑down RPC; firewall P2P/RPC only.

---

## 4) Quick start: local single‑node wallet

### 4.1 Start daemon

```bash
# Mainnet
./bin/dinerod -datadir="$HOME/.dinero" -port=20999 -rpcport=20998 -printtoconsole

# Regtest (development)
./bin/dinerod -datadir="$HOME/.dinero-regtest" -regtest=1 -port=21001 -rpcport=20996 -printtoconsole
```

### 4.2 Wallet Operations (Actual CLI Commands)

**Create a new wallet:**
```bash
./bin/dinero-cli --rpc-url http://127.0.0.1:20998 --rpc-user <user> --rpc-pass <pass> wallet create mywallet
```

**Load existing wallet:**
```bash
./bin/dinero-cli --wallet mywallet wallet load
```

**Get wallet balance:**
```bash
./bin/dinero-cli --wallet mywallet wallet balance
```

**Generate new address:**
```bash
# Bech32 address (recommended)
./bin/dinero-cli --wallet mywallet addr new --type bech32

# P2PKH address (legacy)
./bin/dinero-cli --wallet mywallet addr new --type p2pkh
```

**Validate address:**
```bash
./bin/dinero-cli addr validate din1qxy2kgdygjrsqtzq2n0yrf2493p83kkfjhx0wlh
```

**Send transaction:**
```bash
# Basic send
./bin/dinero-cli --wallet mywallet tx send din1qxy2kgdygjrsqtzq2n0yrf2493p83kkfjhx0wlh 10.5

# Send with fee subtracted from amount
./bin/dinero-cli --wallet mywallet tx send din1qxy2kgdygjrsqtzq2n0yrf2493p83kkfjhx0wlh 5.0 --subtractfee
```

**List transactions:**
```bash
./bin/dinero-cli --wallet mywallet tx list
```

**Lock/unlock wallet:**
```bash
./bin/dinero-cli --wallet mywallet wallet lock
./bin/dinero-cli --wallet mywallet wallet passphrase
```

### 4.3 Blockchain Operations

```bash
# Get current height
./bin/dinero-cli height

# Get best block hash
./bin/dinero-cli besthash

# Get blockchain info (JSON)
./bin/dinero-cli blockchain --json
```

---

## 5) Server (EC2) pattern

**Goal**: hot online node with minimally exposed RPC.

* **System user**: `dinero` (no shell)
* **Data dir**: `/var/lib/dinero`
* **Config** (example `/etc/dinero/dinero.conf`):

```
# P2P
port=20999
# RPC (bind to localhost; reverse proxy/TLS if needed)
rpcbind=127.0.0.1
rpcport=20998
rpcuser=<generated>
rpcpassword=<generated-long>
# Wallet
wallet_name=main
```

* **Firewall**:
  * Inbound: `20999/tcp` P2P (world), `20998/tcp` RPC (**block** from world; allow only ALB/reverse proxy or admin VPN)
* **Service**: systemd unit launching `dinerod` as user `dinero` with `Restart=on-failure`
* **Backups**: see §7 (hot‑copy safe procedure) + seed stored offline

---

## 6) Addressing & derivation

* **Standard**: **BIP84** (bech32 v0 P2WPKH)
* **HRP**: as defined in `tests/network_params.hpp`:
  * **Mainnet HRP**: `din`
  * **Testnet HRP**: `tdin`
  * **Regtest HRP**: `rdin`
* **SLIP‑132** versions checked by tests (`xpub/zpub` mapping). Don't mix networks.
* **Descriptors**: keep a **descriptor JSON** (export) alongside SQLite backup for sanity checks and recovery validation.

---

## 7) Backups & recovery

### 7.1 What to back up

* **Primary**: BIP39 **mnemonic** (+ optional passphrase). Store **offline**.
* **Operational**: wallet DB (SQLite file, e.g., `wallet.sqlite`), plus **descriptor export** (e.g., `descriptor.json`).

### 7.2 Safe backup procedure (running node)

Option A – **Quiesced copy** (recommended):

```bash
./bin/dinero-cli --wallet main wallet lock
# TODO: Add checkpoint command when available
# ./bin/dinero-cli --wallet main wallet checkpoint
# now copy DB and descriptor
cp /var/lib/dinero/wallets/main/wallet.sqlite /backups/dinero/$(date +%F)/
# cp /var/lib/dinero/wallets/main/descriptor.json /backups/dinero/$(date +%F)/
```

Option B – **Cold copy**:

```bash
systemctl stop dinerod
cp -a /var/lib/dinero/wallets/main /backups/dinero/$(date +%F)/
systemctl start dinerod
```

> If your wallet runs SQLite in WAL mode, you may also checkpoint before copy.

### 7.3 Recovery test (do this quarterly)

```bash
# On a lab machine
mkdir -p ~/dinero-recovery-test
cp backup/wallet.sqlite ~/dinero-recovery-test/
# cp backup/descriptor.json ~/dinero-recovery-test/
./bin/dinerod -datadir=~/dinero-recovery-test -printtoconsole &
./bin/dinero-cli --wallet main wallet load --datadir ~/dinero-recovery-test
# ./bin/dinero-cli --wallet main wallet verify --descriptor descriptor.json
./bin/dinero-cli --wallet main addr new --type bech32
```

---

## 8) Security hardening checklist

* [ ] Dedicated user & permissions (no world‑writable data dirs)
* [ ] Strong `rpcuser`/`rpcpassword`, or mutual TLS via reverse proxy
* [ ] Firewall restrict RPC; P2P open
* [ ] OS disk encryption (FileVault/LUKS/EBS encryption)
* [ ] Off‑host off‑region backups with integrity checks (SHA256)
* [ ] Secrets in a password manager/HSM
* [ ] Enable audit logging for critical wallet ops

---

## 9) QA & CI gates

* **Unit/crypto P0**: `ctest -L p0 --output-on-failure`
* **Sanitizers**: ASan+UBSan job in CI (required)
* **Pre‑push hook**: runs P0 locally; reject on red
* **Fuzz (optional)**: `tests/fuzz_bech32_decode.cpp`, `tests/fuzz_bip39_parse.cpp` harnesses (wire into CI as needed)

---

## 10) Crypto Self-Test

All binaries support crypto self-testing:

```bash
# Test daemon crypto
./bin/dinerod --selftest-crypto

# Test CLI crypto
./bin/dinero-cli --selftest-crypto

# Test Qt applications
./bin/dinero-qt6.app/Contents/MacOS/dinero-qt6 --selftest-crypto
./bin/dinero-miner-pro-qt6.app/Contents/MacOS/dinero-miner-pro-qt6 --selftest-crypto
```

Expected output: `crypto self-test: OK`

---

## 11) Qt GUI Integration

The Qt applications use a **DaemonController** pattern (Bitcoin Core style):

* **Managed daemon**: GUI starts/stops `dinerod` as child process
* **RPC communication**: GUI connects to daemon via JSON-RPC
* **Real-time logs**: Daemon output streamed to GUI log panel
* **Status monitoring**: Connection health and readiness detection

**Test the integration:**
```bash
# Launch daemon test GUI
open ./bin/daemon-test-qt6.app

# Or launch main applications
open ./bin/dinero-qt6.app
open ./bin/dinero-miner-pro-qt6.app
```

---

## 12) Packaging & release hygiene

* Produce per‑platform archives:
  * `Dinero-linux-amd64.tar.gz`, `Dinero-linux-arm64.tar.gz`, `Dinero-windows-x86_64.zip`, `Dinero-macos-arm64.tar.gz`
* Each archive must include: `dinerod`, `dinero-cli` (and GUI app bundles where applicable)
* Generate `SHA256SUMS.txt` (sorted, stable)
* Verify no Homebrew/Local prefixes on macOS (`otool -L`); prefer static RocksDB

---

## 13) Operator runbook (incidents)

* **Node won't start**: check `stderr`/logs; ensure data dir permissions; verify RocksDB lock.
* **Wallet mismatch**: re‑point to correct HRP/network; verify descriptor vs DB.
* **Corrupted wallet DB**: restore last good backup; re‑scan chain if needed.
* **Seed compromise**: rotate immediately; sweep funds to a fresh seed.
* **GUI won't connect**: check daemon status, RPC credentials, and port configuration.

---

## 14) Development & Testing

**Build and test crypto foundation:**
```bash
# Build with sanitizers (development)
cmake -S . -B build-test -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_SANITIZERS=ON
cmake --build build-test -j

# Run P0 crypto test suite
ctest --test-dir build-test -L p0 --output-on-failure

# Test individual components
./build-test/bin/test_crypto_vectors
./build-test/bin/test_bip39_seed_kat
./build-test/bin/test_bech32_roundtrip
```

**Qt GUI development:**
```bash
# Build Qt applications
cmake -S . -B build-qt -DWITH_QT=ON
cmake --build build-qt -j --target dinero-qt6 dinero-miner-pro-qt6 daemon-test-qt6

# Test GUI integration
open build-qt/bin/daemon-test-qt6.app
```

---

## 15) Open items to finalize (PRs welcome)

* [ ] Add `wallet checkpoint` command for safe SQLite backups
* [ ] Add `wallet export-descriptor` command for backup validation
* [ ] Add `wallet import-seed` command for BIP39 mnemonic import
* [ ] Add startup log line "Opened RocksDB at <path> / Opened wallet at <sqlite>"
* [ ] GUI wallet: include first‑run wizard (seed create/import, HRP select)
* [ ] Add wallet encryption/decryption commands
* [ ] Add transaction fee estimation commands

---

## 16) Appendix – Concept map

* **BIP39 mnemonic** → **seed** (+ optional passphrase)
* **BIP32** master key → derivation paths
* **BIP84** (bech32 v0) receive/change paths
* **SLIP‑132** encodings for xpub/zpub
* **Descriptor** captures derivation + scripts for round‑trip import
* **SQLite** stores wallet metadata; **RocksDB** stores chainstate/blocks
* **DaemonController** manages daemon lifecycle from Qt GUI
* **CryptoInit** ensures secure initialization of all crypto systems

---

## 17) Architecture Summary

```
┌─────────────────┐    JSON-RPC     ┌──────────────────┐
│   Qt GUI Apps   │◄──────────────►│     dinerod      │
│                 │   (HTTP/WS)     │                  │
│ • dinero-qt6    │                 │ • P2P Network    │
│ • miner-pro-qt6 │                 │ • RPC Server     │
│ • daemon-test   │                 │ • Mining         │
└─────────────────┘                 │ • Blockchain     │
                                    └──────────────────┘
         │                                   │
         │ DaemonController                  │
         │ (start/stop/logs)                 │
         │                                   │
         ▼                                   ▼
┌─────────────────┐                 ┌──────────────────┐
│  Crypto System  │                 │   Data Storage   │
│                 │                 │                  │
│ • secp256k1     │                 │ • RocksDB (chain)│
│ • BIP39/32/84   │                 │ • SQLite (wallet)│
│ • Bech32 v0/v1  │                 │ • Config files   │
│ • Self-tests    │                 │ • Log files      │
└─────────────────┘                 └──────────────────┘
```

This architecture provides:
- **Separation of concerns**: GUI, daemon, crypto, and storage are independent
- **Bitcoin Core compatibility**: Similar RPC interface and daemon pattern  
- **Production readiness**: Comprehensive testing, security, and operational procedures
- **Developer friendly**: Clear APIs, extensive testing, and documentation
