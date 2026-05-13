════════════════════════════════════════════════════════════
  🎯 DINERO GUI → TESTNET CONNECTION - COMPLETE GUIDE
════════════════════════════════════════════════════════════

📊 YOUR TESTNET DEPLOYMENT STATUS:
  ✅ Server 1: 173.249.195.59 (Running)
  ✅ Server 2: 96.9.226.98 (Running)
  ✅ P2P Connection: Established
  ✅ Blockchain: Synced (Height 0, Genesis)
  ✅ RPC: Listening on localhost:20998 (both servers)

🔴 THE PROBLEM:
  RPC ports are bound to 127.0.0.1 (localhost only) for security.
  You cannot connect directly from your Mac to 173.249.195.59:20998

✅ THE SOLUTION: SSH Tunnels + Cookie Authentication

════════════════════════════════════════════════════════════
  🚀 QUICK START (3 STEPS)
════════════════════════════════════════════════════════════

Step 1️⃣: Setup SSH Tunnels
----------------------------
Run: cd /Users/haydarevich/Documents/DineroCoin/gui
     ./setup-tunnels.sh

What this does:
  • Creates SSH tunnel: localhost:20991 → Server 1 RPC
  • Creates SSH tunnel: localhost:20992 → Server 2 RPC
  • Runs in background (stays active)


Step 2️⃣: Fetch Authentication Cookies
--------------------------------------
Run: ./fetch-testnet-cookies.sh

What this does:
  • Downloads .cookie files from both servers via SSH
  • Saves to: ~/.dinero/testnet-cookies/
  • GUI uses these for RPC authentication


Step 3️⃣: Launch the GUI
------------------------
Run: cd /Users/haydarevich/Documents/DineroCoin
     open launch-gui.command

What happens:
  1. GUI tries localhost:20998 (your local daemon)
  2. If fails → tries localhost:20991 (Server 1 tunnel)
  3. If fails → tries localhost:20992 (Server 2 tunnel)
  4. Auto-failover between servers!

════════════════════════════════════════════════════════════
  🔍 VERIFICATION
════════════════════════════════════════════════════════════

✅ Connection successful if you see:
  • Status bar: "✅ Connected: http://127.0.0.1:20991"
  • Green connection indicator
  • Balance and network info loading

❌ Connection failed if you see:
  • "⚠️ Unauthorized (cookie missing/invalid)"
    → Re-run fetch-testnet-cookies.sh
  
  • "⚠️ Connection refused"
    → Tunnels not running, run setup-tunnels.sh

  • "⚠️ Switched to backup server"
    → Normal! This means failover is working

════════════════════════════════════════════════════════════
  🛠️ TROUBLESHOOTING
════════════════════════════════════════════════════════════

Check tunnel status:
  ps aux | grep ssh | grep 20991

Close tunnels:
  ./close-tunnels.sh

Re-establish tunnels:
  ./setup-tunnels.sh

Test connectivity:
  ./test-connection.sh

════════════════════════════════════════════════════════════
  📌 WHAT'S BEEN FIXED IN THE GUI
════════════════════════════════════════════════════════════

✅ Changes made to gui/src/rpcclient.cpp:
  • Added testnet server endpoints (via tunnels)
  • Enhanced cookie loading (supports remote cookies)
  • Automatic server failover (3-tier)
  • Better error messages

✅ New helper scripts created:
  • setup-tunnels.sh - Create SSH tunnels
  • close-tunnels.sh - Stop tunnels
  • fetch-testnet-cookies.sh - Get RPC auth
  • test-connection.sh - Verify connectivity
  • TESTNET-SETUP.sh - This guide

✅ BIP39 validation improved:
  • Word format validation (3-8 lowercase chars)
  • Supports 12 and 24-word seeds
  • Better error messages

════════════════════════════════════════════════════════════
  🎯 FINAL CHECKLIST
════════════════════════════════════════════════════════════

Before launching GUI, ensure:
  ☐ SSH tunnels running (setup-tunnels.sh)
  ☐ Cookies downloaded (fetch-testnet-cookies.sh)
  ☐ GUI rebuilt (already done ✅)

Then launch and verify:
  ☐ Connection status shows green
  ☐ Can generate new addresses
  ☐ Can view network info
  ☐ Balance displays correctly

════════════════════════════════════════════════════════════
  🎊 READY TO GO!
════════════════════════════════════════════════════════════

Your GUI is now configured to connect to your testnet!

Quick start command sequence:
  cd /Users/haydarevich/Documents/DineroCoin/gui
  ./setup-tunnels.sh
  ./fetch-testnet-cookies.sh
  cd ..
  open launch-gui.command

Enjoy your Dinero Wallet! 🚀
