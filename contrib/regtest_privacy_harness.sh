#!/usr/bin/env bash
set -euo pipefail

CLI="${CLI:-dinero-cli}"   # path to your CLI
NODE="${NODE:-127.0.0.1:20998}"
WALLET_A="alice"
WALLET_B="bob"
COORD_URL="${COORD_URL:-http://127.0.0.1:8080/cj}"

echo "== Spin up wallets =="
$CLI -rpcwallet="" createwallet "$WALLET_A" || true
$CLI -rpcwallet="" createwallet "$WALLET_B" || true

echo "== Fund Alice on regtest =="
ADDR_A=$($CLI -rpcwallet="$WALLET_A" getnewaddress)
$CLI generatetoaddress 101 "$ADDR_A" >/dev/null

echo "== Silent Payments address (Bob) =="
SP_ADDR=$($CLI -rpcwallet="$WALLET_B" walletgetnewspaddress)
echo "Bob SP address: $SP_ADDR"

echo "== Send to Silent Payment (Alice → Bob) =="
TXID_SP=$($CLI -rpcwallet="$WALLET_A" sendtosilent "$SP_ADDR" 0.10000000  # 0.1 DIN
                                '{"feerate":5,"subtractfeefromamount":true}' | jq -r .txid)
echo "TXID (SP): $TXID_SP"
$CLI generate 1 >/dev/null

echo "== Bob scans for SP outputs =="
$CLI -rpcwallet="$WALLET_B" silentpaymentscan
BAL_B=$($CLI -rpcwallet="$WALLET_B" getbalance)
echo "Bob balance after SP: $BAL_B"

echo "== PayJoin (Alice paying Bob invoice) =="
INV_ADDR=$($CLI -rpcwallet="$WALLET_B" getnewaddress)
AMT=0.01000000
PJ_URI=$($CLI -rpcwallet="$WALLET_B" payjoinprepare "$INV_ADDR" $AMT | jq -r .bip78)
PSBT0=$($CLI -rpcwallet="$WALLET_A" walletcreatefundedpsbt [] "[{\"$INV_ADDR\": $AMT}]" 0 "{\"replaceable\":true}" | jq -r .psbt)
# Sender negotiates PayJoin with receiver
PSBT1=$($CLI -rpcwallet="$WALLET_A" payjoinclient "$PJ_URI" "$PSBT0" | jq -r .psbt)
# Sender signs and finalizes
PSBT2=$($CLI -rpcwallet="$WALLET_A" walletprocesspsbt "$PSBT1" | jq -r .psbt)
RAW=$($CLI -rpcwallet="$WALLET_A" finalizepsbt "$PSBT2" | jq -r .hex)
TXID_PJ=$($CLI -rpcwallet="$WALLET_A" sendrawtransaction "$RAW")
echo "TXID (PayJoin): $TXID_PJ"
$CLI generate 1 >/dev/null

echo "== CoinJoin (client against coordinator) =="
RID=$($CLI coinjoinjoin --coordinator "$COORD_URL" --amount 100000 --feerate 5 --min_peers 3 | jq -r .round_id)
echo "Round: $RID"
for i in {1..20}; do
  STATUS=$($CLI coinjoinstatus --round_id "$RID" | jq -r .phase)
  echo "Phase: $STATUS"
  [[ "$STATUS" == "done" ]] && break
  sleep 2
done

echo "== Metrics snapshot =="
curl -s "http://$NODE/metrics" | egrep "payjoin|silent|coinjoin" || true

echo "OK."
