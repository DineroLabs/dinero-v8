# DineroCoin v1.0 Configuration Reference

**Status:** Phase Z.2 - Configuration Guarantees
**Date:** 2025-12-31
**Objective:** Document all configuration options with stability guarantees

---

## Philosophy

**"Explicit is better than implicit. Stability is better than surprises."**

Every configuration option is documented with:
- Default value
- Type (string, int, bool)
- Valid range
- Stability level (STABLE, EXPERIMENTAL, DEPRECATED)
- Breaking change policy

**Critical Rule:** Configuration changes require:
1. Backward compatibility OR migration path
2. Version bump (config_version field)
3. Documented in CONFIG_MIGRATION.md
4. Announced in release notes

---

## Configuration Format

### Config Version

**Field:** `config_version`
**Type:** Integer
**Default:** `1`
**Status:** STABLE (v1.0)

Version of the configuration format. Used for migration and validation.

```conf
# dinero.conf
config_version=1
```

**Upgrade Policy:**
- v1.0.x: config_version=1 (frozen)
- v1.1.x: config_version=2 (if breaking changes)
- Migration handled automatically by daemon

---

## Configuration Sections

Configuration is organized into namespaces (dotted keys):
- `wallet.*` - Wallet and data storage
- `rpc.*` - RPC server settings
- `p2p.*` - Peer-to-peer networking
- `network.*` - Network selection (mainnet/testnet/regtest)
- `mining.*` - Mining configuration
- `stratum.*` - Stratum mining server (optional)
- `lightning.*` - Lightning Network (optional)
- `log.*` - Logging configuration

---

## Wallet & Data Storage

### wallet.datadir

**Flat Alias:** `datadir`, `walletdir`, `walletpath`
**Type:** String (directory path)
**Default:** `~/.dinero`
**Status:** STABLE
**Valid:** Any writable directory path

Data directory for blockchain, wallet, and configuration files.

**Example:**
```conf
datadir=/var/lib/dinerod
```

**Command Line:**
```bash
dinerod --datadir=/var/lib/dinerod
```

**Directory Structure:**
```
~/.dinero/
├── blocks/          # Blockchain data (RocksDB)
├── chainstate/      # UTXO set (RocksDB)
├── wallet.dat       # Encrypted wallet
├── peers.dat        # Persistent peer database
├── dinero.conf      # Configuration file
└── debug.log        # Debug log
```

**Migration Notes:**
- Changing datadir requires moving all files
- Use symlinks for splitting storage
- No automatic migration (manual copy required)

---

### wallet.txindex

**Type:** Boolean
**Default:** `false`
**Status:** STABLE
**Valid:** `true`, `false`

Enable full transaction index (required for blockchain explorers).

**Example:**
```conf
txindex=true
```

**WARNING:** Enabling txindex requires full reindex (`-reindex` flag).

---

### wallet.prune

**Type:** Integer (MB)
**Default:** `0` (disabled)
**Status:** STABLE
**Valid:** `0` (disabled) or `550+` MB

Enable block pruning (reduces disk usage).

**Example:**
```conf
prune=550  # Keep only 550 MB of blocks
```

**Constraints:**
- Cannot enable if `txindex=true`
- Minimum: 550 MB (required for header sync)
- Disabling requires full reindex

---

## RPC Server

### rpc.port

**Flat Alias:** `rpcport`
**Type:** Integer
**Default:** `20998`
**Status:** STABLE
**Valid:** `1024-65535`

RPC server listen port.

**Example:**
```conf
rpcport=20998
```

**Ports:**
- Mainnet: 20998 (default)
- Testnet: 20998 (default)
- Regtest: 20996 (default)

---

### rpc.bind

**Flat Alias:** `rpcbind`
**Type:** String (IP address)
**Default:** `127.0.0.1` (localhost only)
**Status:** STABLE
**Valid:** Any valid IP address

RPC server bind address.

**Example:**
```conf
rpcbind=0.0.0.0  # Listen on all interfaces (DANGER: requires rpcallowip)
```

**Security Warning:**
- `0.0.0.0` exposes RPC to network
- Always use `rpcallowip` whitelist
- Always use `rpcuser` and `rpcpassword`
- Consider firewall rules

---

### rpc.user

**Flat Alias:** `rpcuser`
**Type:** String
**Default:** (none - RPC disabled if not set)
**Status:** STABLE
**Valid:** Any non-empty string

RPC authentication username.

**Example:**
```conf
rpcuser=dinero
```

**Security:**
- REQUIRED for RPC access
- Use strong, unique username
- Do NOT use "admin", "root", "dinero"

---

### rpc.password

**Flat Alias:** `rpcpassword`
**Type:** String
**Default:** (none - RPC disabled if not set)
**Status:** STABLE
**Valid:** Any non-empty string

RPC authentication password.

**Example:**
```conf
rpcpassword=strong_random_password_here
```

**Security:**
- REQUIRED for RPC access
- Use strong, random password (32+ characters)
- Rotate regularly
- Do NOT commit to version control

**Generate Strong Password:**
```bash
openssl rand -hex 32
```

---

### rpc.allowip

**Flat Alias:** `rpcallowip`
**Type:** String (IP/CIDR)
**Default:** `127.0.0.1` (localhost only)
**Status:** STABLE
**Valid:** IP address or CIDR notation

Whitelist of IP addresses allowed to access RPC.

**Example:**
```conf
rpcallowip=127.0.0.1
rpcallowip=192.168.1.0/24  # Allow local subnet
```

**Multi-Value:**
```conf
# Multiple IPs
rpcallowip=127.0.0.1
rpcallowip=10.0.0.5
```

**Security:**
- Always use with `rpcbind=0.0.0.0`
- Restrict to minimum necessary IPs
- Use CIDR for subnets

---

## P2P Networking

### p2p.port

**Flat Alias:** `port`, `p2pport`
**Type:** Integer
**Default:** `20999`
**Status:** STABLE
**Valid:** `1024-65535`

P2P network listen port.

**Example:**
```conf
port=20999
```

**Ports:**
- Mainnet: 20999 (default)
- Testnet: 21000 (default)
- Regtest: 21001 (default)

---

### p2p.bind

**Flat Alias:** `bind`
**Type:** String (IP address)
**Default:** `0.0.0.0` (all interfaces)
**Status:** STABLE
**Valid:** Any valid IP address

P2P network bind address.

**Example:**
```conf
bind=0.0.0.0  # Listen on all interfaces (default)
```

---

### p2p.listen

**Flat Alias:** `listen`
**Type:** Boolean
**Default:** `true`
**Status:** STABLE
**Valid:** `true`, `false`

Accept incoming P2P connections.

**Example:**
```conf
listen=true
```

**Use Cases:**
- `listen=true` - Public node (accepts inbound)
- `listen=false` - Private node (outbound only)

---

### p2p.addnode

**Flat Alias:** `addnode`
**Type:** String (hostname:port or IP:port)
**Default:** (none)
**Status:** STABLE
**Valid:** Valid hostname or IP with port
**Multi-Value:** Yes

Add a node to connect to.

**Example:**
```conf
addnode=seed1.dinero.com:20999
addnode=seed2.dinero.com:20999
addnode=192.168.1.100:20999
```

**Behavior:**
- Daemon will attempt to connect to these nodes
- Does NOT disable DNS seed discovery
- Useful for private networks

---

### p2p.connect

**Flat Alias:** `connect`
**Type:** String (hostname:port or IP:port)
**Default:** (none)
**Status:** STABLE
**Valid:** Valid hostname or IP with port
**Multi-Value:** Yes

Connect ONLY to specified nodes (disables seed discovery).

**Example:**
```conf
connect=192.168.1.100:20999
connect=192.168.1.101:20999
```

**Behavior:**
- Daemon connects ONLY to these nodes
- Disables DNS seed discovery
- Disables automatic peer discovery
- Use for isolated/private networks

---

### p2p.maxconnections

**Flat Alias:** `maxconnections`
**Type:** Integer
**Default:** `125`
**Status:** STABLE
**Valid:** `8-10000`

Maximum number of total connections (inbound + outbound).

**Example:**
```conf
maxconnections=125
```

**Constraints:**
- Minimum: 8 (required for network health)
- Maximum: 10000 (theoretical limit)
- Recommended: 125 (default)

**Resource Impact:**
- Each connection: ~1 MB memory
- Each connection: ~1 file descriptor

---

### p2p.whitelist

**Flat Alias:** `whitelist`
**Type:** String (IP/CIDR)
**Default:** (none)
**Status:** STABLE
**Valid:** IP address or CIDR notation
**Multi-Value:** Yes

Whitelist peers (never banned, higher connection priority).

**Example:**
```conf
whitelist=192.168.1.0/24  # Whitelist local subnet
whitelist=10.0.0.5        # Whitelist specific IP
```

**Use Cases:**
- Trusted nodes (mining pool, exchange)
- Local network peers
- Disaster recovery nodes

---

## Network Selection

### network.testnet

**Flat Alias:** `testnet`
**Type:** Boolean
**Default:** `false`
**Status:** STABLE
**Valid:** `true`, `false`

Enable testnet mode.

**Example:**
```conf
testnet=true
```

**Mutually Exclusive:**
- Cannot enable `testnet` and `regtest` simultaneously
- Cannot enable `testnet` and `mainnet` simultaneously

**Ports:**
- Testnet P2P: 21000
- Testnet RPC: 20998

---

### network.regtest

**Flat Alias:** `regtest`
**Type:** Boolean
**Default:** `false`
**Status:** STABLE
**Valid:** `true`, `false`

Enable regression test mode (local testing).

**Example:**
```conf
regtest=true
```

**Behavior:**
- No proof-of-work requirement
- Instant block generation (`dinero-cli generate 1`)
- Isolated network (no peers)

**Use Cases:**
- Unit testing
- Integration testing
- Development

---

### network.mainnet

**Flat Alias:** `mainnet`
**Type:** Boolean
**Default:** `true`
**Status:** STABLE
**Valid:** `true`, `false`

Enable mainnet mode (default, explicit).

**Example:**
```conf
mainnet=true
```

**Note:** Explicit `mainnet=true` is optional (default behavior).

---

## Mining

### mining.enabled

**Flat Alias:** `gen`
**Type:** Boolean
**Default:** `false`
**Status:** STABLE
**Valid:** `true`, `false`

Enable built-in CPU mining.

**Example:**
```conf
gen=true
```

**WARNING:** CPU mining is inefficient. Use for testing only.

---

### mining.threads

**Flat Alias:** `genproclimit`
**Type:** Integer
**Default:** `1`
**Status:** STABLE
**Valid:** `1-256`

Number of mining threads.

**Example:**
```conf
genproclimit=4
```

**Constraints:**
- Maximum: Number of CPU cores
- Recommended: Leave 1-2 cores for node operation

---

### mining.address

**Flat Alias:** `miningaddress`
**Type:** String (DIN address)
**Default:** (none - required if mining enabled)
**Status:** STABLE
**Valid:** Valid DIN address

Address to receive mining rewards.

**Example:**
```conf
miningaddress=DIN1A2B3C4D5E6F7G8H9I0J1K2L3M4N5O6P7Q8R
```

**Validation:**
- Must be valid DIN address
- Must match network (mainnet/testnet/regtest)
- Required if `gen=true`

---

## Stratum Mining Server

### stratum.enabled

**Flat Alias:** `stratum`
**Type:** Boolean
**Default:** `true`
**Status:** STABLE
**Valid:** `true`, `false`

Enable Stratum V1 mining server.

**Example:**
```conf
stratum=true
```

**Use Cases:**
- Mining pool operator
- GPU mining
- ASIC mining

---

### stratum.port

**Flat Alias:** `stratumport`
**Type:** Integer
**Default:** `3333`
**Status:** STABLE
**Valid:** `1024-65535`

Stratum server listen port.

**Example:**
```conf
stratumport=3333
```

**Ports:**
- Standard: 3333 (conventional)
- SSL: 3334 (if using stratum-ssl)

---

### stratum.maxconnections

**Flat Alias:** `stratummaxconnections`
**Type:** Integer
**Default:** `100`
**Status:** STABLE
**Valid:** `1-10000`

Maximum number of Stratum miner connections.

**Example:**
```conf
stratummaxconnections=100
```

**Resource Impact:**
- Each connection: ~500 KB memory
- Each connection: ~1 file descriptor

---

### stratum.ssl

**Flat Alias:** `stratum-ssl`
**Type:** Boolean
**Default:** `false`
**Status:** EXPERIMENTAL
**Valid:** `true`, `false`

Enable SSL/TLS for Stratum connections.

**Example:**
```conf
stratum-ssl=true
stratum-ssl-cert=/path/to/cert.pem
stratum-ssl-key=/path/to/key.pem
```

**Requirements:**
- `stratum-ssl-cert` - Path to SSL certificate
- `stratum-ssl-key` - Path to SSL private key

**Certificate Generation:**
```bash
openssl req -x509 -newkey rsa:4096 \
  -keyout stratum_key.pem \
  -out stratum_cert.pem \
  -days 365 -nodes
```

---

### stratum.ssl.cert

**Flat Alias:** `stratum-ssl-cert`
**Type:** String (file path)
**Default:** `<datadir>/stratum_cert.pem`
**Status:** EXPERIMENTAL
**Valid:** Path to valid SSL certificate

SSL certificate path for Stratum SSL.

---

### stratum.ssl.key

**Flat Alias:** `stratum-ssl-key`
**Type:** String (file path)
**Default:** `<datadir>/stratum_key.pem`
**Status:** EXPERIMENTAL
**Valid:** Path to valid SSL private key

SSL private key path for Stratum SSL.

---

## Logging

### log.level

**Type:** String
**Default:** `info`
**Status:** STABLE
**Valid:** `trace`, `debug`, `info`, `warn`, `error`, `fatal`

Log verbosity level.

**Example:**
```conf
loglevel=debug
```

**Levels:**
- `trace` - Extremely verbose (not recommended)
- `debug` - Detailed debugging
- `info` - Normal operation (default)
- `warn` - Warnings only
- `error` - Errors only
- `fatal` - Fatal errors only

---

### log.path

**Flat Alias:** `logpath`
**Type:** String (file path)
**Default:** `<datadir>/debug.log`
**Status:** STABLE
**Valid:** Any writable file path

Log file path.

**Example:**
```conf
logpath=/var/log/dinerod/debug.log
```

---

### log.printtoconsole

**Type:** Boolean
**Default:** `false`
**Status:** STABLE
**Valid:** `true`, `false`

Print logs to console (stdout/stderr).

**Example:**
```conf
printtoconsole=true
```

**Use Cases:**
- Systemd journald logging
- Docker containers
- Development

---

## Stability Levels

**STABLE** (v1.0)
- Guaranteed backward compatibility
- Breaking changes require major version bump
- Safe for production

**EXPERIMENTAL** (v1.0)
- May change in minor versions
- May be removed in minor versions
- Use with caution

**DEPRECATED** (v1.0)
- Will be removed in v1.1
- Use alternative (documented)
- Migration path provided

**INTERNAL** (not documented)
- Not for user configuration
- May change without notice

---

## Configuration Validation

### Required Fields

For production operation:
- `rpcuser` - RPC authentication username
- `rpcpassword` - RPC authentication password

For mining:
- `mining.address` - Mining reward address (if `gen=true`)

### Mutually Exclusive

Cannot enable simultaneously:
- `testnet` and `regtest`
- `testnet` and `mainnet`
- `regtest` and `mainnet`
- `txindex` and `prune`

### Constraints

Port ranges:
- All ports: `1024-65535`

Connection limits:
- `p2p.maxconnections`: `8-10000`

Pruning:
- `wallet.prune`: `0` (disabled) or `550+` MB

---

## Breaking Change Policy

**What Constitutes a Breaking Change:**

1. **Config Key Renamed**
   - Old: `rpcport`
   - New: `rpc.port`
   - **Policy:** Maintain backward compatibility via aliases (indefinitely)

2. **Default Value Changed**
   - Old default: `stratum=false`
   - New default: `stratum=true`
   - **Policy:** Announce in release notes, document migration

3. **Valid Range Changed**
   - Old range: `p2p.maxconnections=1-1000`
   - New range: `p2p.maxconnections=8-10000`
   - **Policy:** Reject invalid configs with clear error message

4. **Option Removed**
   - Deprecated option removed after 1 major version
   - **Policy:** Deprecation warning → removal in next major version

**What Is NOT a Breaking Change:**

1. **New Config Key Added**
   - Safe (defaults ensure backward compatibility)

2. **Option Marked Deprecated**
   - Still works (with warning)

---

## Configuration File Format

**Location:** `<datadir>/dinero.conf`

**Format:** INI-style (key=value pairs)

**Example:**
```conf
# DineroCoin v1.0 Configuration
config_version=1

# Data directory
datadir=/var/lib/dinerod

# RPC server
rpcuser=dinero
rpcpassword=strong_random_password_here
rpcport=20998
rpcbind=127.0.0.1
rpcallowip=127.0.0.1

# P2P network
port=20999
listen=true
maxconnections=125

# Add seed nodes
addnode=seed1.dinero.com:20999
addnode=seed2.dinero.com:20999

# Network selection
mainnet=true

# Mining (disabled by default)
gen=false

# Stratum server (enabled by default)
stratum=true
stratumport=3333
stratummaxconnections=100
```

**Comments:**
- Lines starting with `#` are comments
- Inline comments not supported

**Multi-Value Keys:**
- `addnode`, `connect`, `whitelist`, `rpcallowip`
- Specify multiple times

---

## Command-Line Override

Command-line arguments override config file.

**Example:**
```bash
# Config file: rpcport=20998
# Override:
dinerod --rpcport=21000

# Effective value: 21000
```

**Precedence:**
1. Command-line arguments (highest)
2. Config file
3. Defaults (lowest)

---

## Configuration Migration

See [CONFIG_MIGRATION.md](CONFIG_MIGRATION.md) for version-specific migration guides.

**Version History:**
- v1.0.x: config_version=1 (current)
- v1.1.x: config_version=2 (future)

---

## Security Best Practices

### RPC Security

✅ **DO:**
- Use strong `rpcuser` and `rpcpassword`
- Bind to `127.0.0.1` for local-only access
- Use `rpcallowip` whitelist if binding to `0.0.0.0`
- Rotate passwords regularly

❌ **DON'T:**
- Use default/weak passwords
- Expose RPC to internet without firewall
- Commit passwords to version control
- Reuse passwords across nodes

### Stratum Security

✅ **DO:**
- Use SSL/TLS (`stratum-ssl=true`)
- Restrict `stratummaxconnections`
- Monitor for abuse

❌ **DON'T:**
- Expose Stratum to internet without SSL
- Allow unlimited connections

### File Permissions

```bash
chmod 600 ~/.dinero/dinero.conf  # Config file (contains passwords)
chmod 700 ~/.dinero              # Data directory
```

---

## Troubleshooting

### RPC Not Accessible

**Symptom:** `dinero-cli` fails with "connection refused"

**Checklist:**
- [ ] Is `rpcuser` set?
- [ ] Is `rpcpassword` set?
- [ ] Is `rpcport` correct?
- [ ] Is `rpcbind` correct?
- [ ] Is firewall blocking port?

### P2P No Connections

**Symptom:** `dinero-cli getpeerinfo` shows 0 peers

**Checklist:**
- [ ] Is `listen=true`?
- [ ] Is `port` correct?
- [ ] Is firewall blocking port?
- [ ] Are seed nodes reachable?
- [ ] Is internet connection working?

### Mining Not Working

**Symptom:** Mining enabled but no blocks found

**Checklist:**
- [ ] Is `gen=true`?
- [ ] Is `miningaddress` set?
- [ ] Is `miningaddress` valid?
- [ ] Is node synced to tip?

---

## Audit Trail

Phase Z.2 establishes **configuration guarantees**:

1. **Phase D** - Consensus frozen ✅
2. **Phase E** - Safety infrastructure ✅
3. **Phase Z.1** - Reproducible builds ✅
4. **Phase Z.2** - Configuration guarantees ← **YOU ARE HERE** 🔨

**Next:** Phase Z.3 (RPC Compatibility Contract)

---

**Phase Z.2: Configuration Reference - Complete**

**Configuration guarantee:**
- ✅ All options documented
- ✅ Defaults documented
- ✅ Stability levels defined
- ✅ Breaking change policy established
- ✅ Security best practices documented

**Operators have complete configuration visibility.**
