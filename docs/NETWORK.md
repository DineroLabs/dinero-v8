# Dinero Network Configuration

## P2P vs RPC

Dinero's P2P network is direct node-to-node traffic. It does not depend
on a website. DNS seeds are only a convenience for first contact.

- P2P port: `20999` on mainnet. This can be open to the internet.
- RPC port: `20998` on mainnet. Keep this private, normally bound to
  `127.0.0.1`.
- Offline mode: leave `p2p.offline` unset or false for normal nodes.

Every `dinerod` / `dinero-qt` instance with networking enabled can
connect to peers, learn more peers via `getaddr`, and persist known
peers in `peers.dat`.

## v8 Bootstrap Nodes

v8 has fixed seed IPs compiled into the binary and DNS seeds as an
extra bootstrap path. These manual `addnode` lines are a useful fallback
if DNS is unavailable or a first-start wallet needs help finding peers.

### LA Server (Los Angeles)
- **IP Address**: `172.93.160.131`
- **P2P Port**: `20999`
- **Location**: Los Angeles, USA
- **Add to config**: `addnode=172.93.160.131:20999`

### VA Server (Virginia)
- **IP Address**: `173.249.195.59`
- **P2P Port**: `20999`
- **Location**: Virginia, USA
- **Add to config**: `addnode=173.249.195.59:20999`

### MO Server (Missouri)
- **IP Address**: `72.18.214.120`
- **P2P Port**: `20999`
- **Location**: Missouri, USA
- **Add to config**: `addnode=72.18.214.120:20999`

### Canada Server
- **IP Address**: `96.9.226.98`
- **P2P Port**: `20999`
- **Location**: Canada
- **Add to config**: `addnode=96.9.226.98:20999`

## Connecting to the Network

### Option 1: Configuration File

Create a `dinero.conf` file in your Dinero directory with the following content:

```conf
# Enable P2P networking.
listen=1
port=20999

# Manual bootstrap fallback nodes.
addnode=172.93.160.131:20999
addnode=173.249.195.59:20999
addnode=72.18.214.120:20999
addnode=96.9.226.98:20999

# Keep RPC private.
rpcport=20998
rpcbind=127.0.0.1
rpcallowip=127.0.0.1
```

### Option 2: Command Line

Start your daemon with seed node parameters:

```bash
./dinerod -listen=1 -port=20999 \
  -addnode=172.93.160.131:20999 \
  -addnode=173.249.195.59:20999 \
  -addnode=72.18.214.120:20999 \
  -addnode=96.9.226.98:20999
```

### Option 3: Runtime RPC Commands

Connect to seed nodes while your daemon is running:

```bash
# Get your RPC cookie
COOKIE=$(cat data/.cookie)

# Add LA seed node
curl -s -X POST http://127.0.0.1:20998 -u "$COOKIE" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":"test","method":"addnode","params":["172.93.160.131:20999","add"]}'

# Add the remaining fallback nodes the same way:
# 173.249.195.59:20999, 72.18.214.120:20999, 96.9.226.98:20999
```

## Verifying Network Connection

Check your peer connections:

```bash
COOKIE=$(cat data/.cookie)
curl -s -X POST http://127.0.0.1:20998 -u "$COOKIE" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":"test","method":"getpeerinfo","params":[]}' | python3 -m json.tool
```

You should see outbound peers, and public nodes should also see inbound
connections after TCP `20999` is reachable through the firewall/router.

## Firewall Requirements

If you want your node to accept incoming connections and help the network:

### Linux (ufw)
```bash
sudo ufw allow 20999/tcp comment "Dinero P2P"
```

### Linux (iptables)
```bash
sudo iptables -A INPUT -p tcp --dport 20999 -j ACCEPT
```

### macOS
```bash
# System Settings > Network > Firewall > Options
# Allow inbound connections for dinerod / dinero-qt.
```

### Windows
Open Windows Defender Firewall and allow inbound TCP `20999` for
`dinerod.exe` / `dinero-qt.exe`. Keep RPC `20998` local/private.

### Automatic Router Port Mapping

Dinero can try to open inbound TCP `20999` automatically on compatible
home routers:

```ini
listen=1
port=20999
portmap=auto
```

Compatibility aliases are also accepted:

```ini
upnp=1
natpmp=1
```

`portmap=auto` tries UPnP first, then NAT-PMP. This is best-effort:
outbound P2P still works if the router does not support port mapping,
UPnP/NAT-PMP is disabled, the OS firewall blocks the app, or the ISP
uses CGNAT. Release builders should package `miniupnpc` and `libnatpmp`
from `depends/` once Dinero moves to fully reproducible Bitcoin-style
dependency builds.

## Network Status

### Current Network State
- **Active v8 fleet bootstrap nodes**: 4 (LA, VA, MO, Canada)
- **P2P port**: `20999`
- **RPC port**: `20998` local/private
- **Network model**: direct P2P mesh with fixed seeds, DNS seeds,
  `peers.dat`, and `getaddr` peer discovery

### Monitoring

Monitor your local node only. Public fleet RPC is intentionally not
internet-exposed.

```bash
# Check local height through local/private RPC.
COOKIE=$(cat data/.cookie)
curl -s -X POST http://127.0.0.1:20998 -u "$COOKIE" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":"local","method":"getblockcount","params":[]}' \
  | python3 -c "import sys,json; print('Height:', json.load(sys.stdin)['result'])"
```

## Troubleshooting

### No Peers Found
1. Check firewall allows outbound connections on port 20999
2. Verify seed nodes are reachable: `nc -vz 172.93.160.131 20999`
3. Check daemon logs for connection errors
4. Manually add nodes via RPC `addnode` command

### Connection Refused
1. Ensure daemon is running: `ps aux | grep dinerod`
2. Check RPC port is listening: `lsof -i :20998` (Unix) or `netstat -an | grep 20998`
3. Verify cookie file exists: `cat data/.cookie`

### Slow Synchronization
1. Check network bandwidth: `iftop` or `nethogs`
2. Verify peer connections: Use `getpeerinfo` RPC
3. Check disk I/O: `iotop` or Activity Monitor

## Contributing Seed Nodes

If you want to run a public seed node:

1. Ensure stable uptime (>99%)
2. Enable P2P listening with `listen=1`
3. Open firewall port 20999
4. Contact maintainers to add your node to this list

---

**Last Updated**: May 14, 2026
**Release Line**: v8
