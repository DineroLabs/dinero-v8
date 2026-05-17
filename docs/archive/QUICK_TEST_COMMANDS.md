# ⚡ Quick Test Commands

Copy-paste these commands for quick testing:

---

## 🧪 **Test 1: Server Failover**

### Stop Server 1 (trigger failover):
```bash
ssh -i .server-key root@96.9.226.98 'systemctl stop dinerod'
```
**Watch GUI status bar** → Should switch to Server 2 within 2 seconds

### Restart Server 1:
```bash
ssh -i .server-key root@96.9.226.98 'systemctl start dinerod'
```

---

## 🧪 **Test 2: Monitor P2P Activity**

### Server 1 logs (live):
```bash
ssh -i .server-key root@96.9.226.98 'journalctl -u dinerod -f | grep -i peer'
```

### Server 2 logs (live):
```bash
ssh -i .server2-key root@173.249.195.59 'journalctl -u dinerod -f | grep -i peer'
```

### Both servers connection status:
```bash
ssh -i .server-key root@96.9.226.98 'journalctl -u dinerod -n 5 | grep -i "peer connected"'
ssh -i .server2-key root@173.249.195.59 'journalctl -u dinerod -n 5 | grep -i "peer connected"'
```

---

## 🧪 **Test 3: Verify Async Outbox**

### Check binary for async symbols:
```bash
ssh -i .server-key root@96.9.226.98 'strings /usr/local/bin/dinerod | grep -E "outbox|broadcast_async"'
```

### Monitor outbox activity:
```bash
ssh -i .server-key root@96.9.226.98 'journalctl -u dinerod -f | grep -i outbox'
```

---

## 🧪 **Test 4: Server Health Check**

### Check both servers:
```bash
echo "Server 1:" && ssh -i .server-key root@96.9.226.98 'systemctl status dinerod | head -5'
echo ""
echo "Server 2:" && ssh -i .server2-key root@173.249.195.59 'systemctl status dinerod | head -5'
```

### Get block heights:
```bash
echo "Server 1:" && ssh -i .server-key root@96.9.226.98 'journalctl -u dinerod -n 20 | grep -i "height\|block" | tail -3'
echo ""
echo "Server 2:" && ssh -i .server2-key root@173.249.195.59 'journalctl -u dinerod -n 20 | grep -i "height\|block" | tail -3'
```

---

## 🧪 **Test 5: Mine a Block (for transaction history test)**

In GUI:
1. Go to 📥 Receive tab
2. Click "🔓 Unlock Wallet"
3. Click "🆕 New Address"
4. Go to ⛏️ Mining tab
5. Click "Use Wallet Address"
6. Set threads to 4
7. Click "▶️ Start Mining"
8. Watch Mining Statistics panel

---

## 🛠️ **Troubleshooting Commands**

### Restart both servers:
```bash
ssh -i .server-key root@96.9.226.98 'systemctl restart dinerod' &
ssh -i .server2-key root@173.249.195.59 'systemctl restart dinerod' &
wait
echo "Both servers restarted"
```

### View recent errors:
```bash
ssh -i .server-key root@96.9.226.98 'journalctl -u dinerod -p err -n 20'
```

### Check firewall:
```bash
ssh -i .server-key root@96.9.226.98 'ufw status | grep 2099'
```

### Kill and restart GUI:
```bash
pkill dinero-qt && sleep 1 && ./gui/build/dinero-qt &
```

---

## 📊 **Monitoring Commands**

### Watch all P2P activity (both servers):
```bash
# Terminal 1
ssh -i .server-key root@96.9.226.98 'journalctl -u dinerod -f'

# Terminal 2
ssh -i .server2-key root@173.249.195.59 'journalctl -u dinerod -f'
```

### CPU/Memory usage:
```bash
ssh -i .server-key root@96.9.226.98 'top -bn1 | grep dinerod'
ssh -i .server2-key root@173.249.195.59 'top -bn1 | grep dinerod'
```

### Connection count:
```bash
ssh -i .server-key root@96.9.226.98 'netstat -an | grep :20999 | wc -l'
```

---

## ✅ **Success Indicators**

### Good signs:
```
✓ GUI shows "✅ Connected: http://..."
✓ Logs show "Peer connected: X.X.X.X:20999"
✓ Mining stats update every second
✓ Transaction history loads
✓ No "send failed" or "timeout" errors
```

### Bad signs:
```
✗ GUI shows red "⚠️ Connection failed"
✗ Logs show "Failed to connect to peer"
✗ Mining stats frozen
✗ Transaction history empty (after mining blocks)
✗ Frequent "send timeout" errors
```

---

## 🎯 **Quick Verification Script**

Run this to verify everything is working:

```bash
#!/bin/bash
echo "1. Servers running..."
ssh -i .server-key root@96.9.226.98 'systemctl is-active dinerod' && echo "  ✅ Server 1" || echo "  ❌ Server 1"
ssh -i .server2-key root@173.249.195.59 'systemctl is-active dinerod' && echo "  ✅ Server 2" || echo "  ❌ Server 2"

echo "2. P2P connected..."
ssh -i .server-key root@96.9.226.98 'journalctl -u dinerod -n 10 | grep -q "Peer connected"' && echo "  ✅ Server 1" || echo "  ⚠️  Server 1"
ssh -i .server2-key root@173.249.195.59 'journalctl -u dinerod -n 10 | grep -q "Peer connected"' && echo "  ✅ Server 2" || echo "  ⚠️  Server 2"

echo "3. Async outbox active..."
ssh -i .server-key root@96.9.226.98 'strings /usr/local/bin/dinerod | grep -q outbox_loop' && echo "  ✅ Server 1" || echo "  ❌ Server 1"
ssh -i .server2-key root@173.249.195.59 'strings /usr/local/bin/dinerod | grep -q outbox_loop' && echo "  ✅ Server 2" || echo "  ❌ Server 2"

echo "4. GUI running..."
pgrep dinero-qt > /dev/null && echo "  ✅ GUI active" || echo "  ❌ GUI not running"

echo ""
echo "All checks complete!"
```

Save as `quick-verify.sh` and run: `chmod +x quick-verify.sh && ./quick-verify.sh`

