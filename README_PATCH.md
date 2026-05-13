```diff
diff --git a/README.md b/README.md
index 0000000..1111111 100644
--- a/README.md
+++ b/README.md
@@ -1,6 +1,67 @@
 # DineroCoin

+## What's new (v0.4.0)
+This release introduces a **wallet-scoped RPC** architecture and **wallet-aware mining**:
+
+- Wallet-scoped URLs: call RPCs under `/wallet/<name>`
+- Mining RPCs: `mining.getaddress`, `mining.setaddress`
+- Wallet RPCs: `wallet.create`, `wallet.load`, `wallet.getnewaddress`, `wallet.validateaddress`, `wallet.listaddresses`
+- Meta RPCs: `rpc.capabilities`, `rpc.listmethods`, `rpc.help`, `rpc.health`
+- SQLite persistence with network-scoped mining address
+- Single dispatcher (legacy RPC v2 removed)
+
+> Mining rewards are guaranteed spendable: the mining address must be owned by the active/scoped wallet and its HRP must match the network.
+
 ## Build
 <existing build instructions remain here>

+## Quickstart (Regtest)
+Start the daemon, authenticate with the cookie, and exercise the wallet-aware mining RPCs.
+
+```bash
+# 1) Start daemon (regtest)
+DATADIR="$HOME/Library/Application Support/DineroCoin/Dinero All-in-One/data/regtest"
+./build/bin/dinerod --regtest --datadir="$DATADIR" --rpcport=18443 --wsport=18444
+
+# 2) Auth cookie
+AUTH="$(tr -d '\r\n' < "$DATADIR/.cookie")"
+
+# 3) Health & capabilities
+curl -s -u "$AUTH" -H 'content-type: application/json' \
+  --data '{"jsonrpc":"2.0","id":1,"method":"rpc.health"}' \
+  http://127.0.0.1:18443/
+
+curl -s -u "$AUTH" -H 'content-type: application/json' \
+  --data '{"jsonrpc":"2.0","id":2,"method":"rpc.capabilities"}' \
+  http://127.0.0.1:18443/
+```
+
+### Wallet lifecycle (scoped URL)
+```bash
+RPC="http://127.0.0.1:18443/wallet/demo"
+
+# Create + load
+curl -s -u "$AUTH" -H 'content-type: application/json' \
+  --data '{"jsonrpc":"2.0","id":3,"method":"wallet.create","params":{"name":"demo"}}' \
+  http://127.0.0.1:18443/
+
+curl -s -u "$AUTH" -H 'content-type: application/json' \
+  --data '{"jsonrpc":"2.0","id":4,"method":"wallet.load","params":{"name":"demo"}}' \
+  "$RPC"
+
+# Derive address (BIP84 path on regtest, HRP rdin)
+ADDR=$(curl -s -u "$AUTH" -H 'content-type: application/json' \
+  --data '{"jsonrpc":"2.0","id":5,"method":"wallet.getnewaddress"}' "$RPC" | jq -r .result.address)
+echo "$ADDR"
+
+# Set mining address (must be wallet-owned)
+curl -s -u "$AUTH" -H 'content-type: application/json' \
+  --data "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"mining.setaddress\",\"params\":[\"$ADDR\"]}" \
+  http://127.0.0.1:18443/
+
+# Read mining address
+curl -s -u "$AUTH" -H 'content-type: application/json' \
+  --data '{"jsonrpc":"2.0","id":7,"method":"mining.getaddress"}' \
+  http://127.0.0.1:18443/
+```
+
+## API Docs
+- Meta: `docs/rpc/meta.md` (`rpc.capabilities`, `rpc.listmethods`, `rpc.help`, `rpc.health`)
+- Wallet: `docs/rpc/wallet.md`
+- Mining: `docs/rpc/mining.md`
+
 ## Testing
 <keep existing section; add the following line if not present>
 
+End-to-end wallet-aware mining tests:
+```bash
+scripts/ci/run_e2e_tests.sh
+```
```
