# Dinero Coin Network Configuration

## Official Seed Nodes

The Dinero network has the following bootstrap seed nodes for peer discovery:

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

## Connecting to the Network

### Option 1: Configuration File

Create a `dinero.conf` file in your Dinero directory with the following content:

```conf
# Add both seed nodes for redundancy
addnode=172.93.160.131:20999
addnode=173.249.195.59:20999

# P2P port (default)
port=20999

# RPC settings
rpcport=20998
rpcbind=127.0.0.1
```

### Option 2: Command Line

Start your daemon with seed node parameters:

```bash
./dinerod -addnode=172.93.160.131:20999 -addnode=173.249.195.59:20999
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

# Add VA seed node
curl -s -X POST http://127.0.0.1:20998 -u "$COOKIE" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":"test","method":"addnode","params":["173.249.195.59:20999","add"]}'
```

## Verifying Network Connection

Check your peer connections:

```bash
COOKIE=$(cat data/.cookie)
curl -s -X POST http://127.0.0.1:20998 -u "$COOKIE" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":"test","method":"getpeerinfo","params":[]}' | python3 -m json.tool
```

You should see connections to one or both seed nodes.

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
# Allow in System Preferences > Security & Privacy > Firewall > Firewall Options
# Or use built-in firewall rules
```

## Network Status

### Current Network State
- **Active Seed Nodes**: 2 (LA, VA)
- **Consensus Fix**: ✅ Deployed (October 15, 2025)
- **Network Protocol**: v0.1.0
- **Difficulty**: Phase 1 (0x1d3fffff for blocks 1-180,000)

### Monitoring

Monitor network health:
```bash
# Check blockchain height across nodes
for node in 172.93.160.131 173.249.195.59; do
  echo "Node: $node"
  curl -s http://$node:20998 -u "__cookie__:YOUR_COOKIE" \
    -H "Content-Type: application/json" \
    -d '{"jsonrpc":"2.0","id":"test","method":"getblockcount","params":[]}' \
    | python3 -c "import sys,json; print('Height:', json.load(sys.stdin)['result'])"
done
```

## Troubleshooting

### No Peers Found
1. Check firewall allows outbound connections on port 20999
2. Verify seed nodes are reachable: `ping 172.93.160.131` and `ping 173.249.195.59`
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
2. Allow incoming connections (bind=0.0.0.0 in config)
3. Open firewall port 20999
4. Contact maintainers to add your node to this list

---

**Last Updated**: October 15, 2025
**Network Version**: 0.1.0
**Consensus**: Phase 1 (Easy Difficulty)
