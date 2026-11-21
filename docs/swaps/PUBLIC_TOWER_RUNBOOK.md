# Running the public swap watchtower

What it is and what it can and cannot do: `docs/design/din-btc-swaps-without-bitcoin-core.md`,
section 14.2. This page is how to run one.

## Parts

| Part | Program | Listens | Needs |
|---|---|---|---|
| The tower | `dinero-swap-tower` | nothing | a synced `dinerod` on the same machine (RPC), outbound HTTPS and Bitcoin P2P |
| The door | `dinero-swap-tower-intake` | `127.0.0.1:8742` | write access to the inbox |
| TLS and request limits | nginx or Caddy | `:443` | a certificate for the tower's host name |

Both programs come from a build with `-DDINERO_SWAP_LIGHT_BTC=ON` (the tower needs it; the door
does not link libcurl but is built beside it):

```
cmake --build <build> --target dinero-swap-tower dinero-swap-tower-intake
```

Run both as one unprivileged user that owns the inbox and nothing else. Neither holds a key.

## Commands

```
INBOX=/var/lib/dinero-swap-tower/inbox      # mode 0700, owned by the tower's user

dinero-swap-tower-intake --inbox $INBOX --listen 127.0.0.1:8742 --network main

dinero-swap-tower --inbox $INBOX \
  --din-rpc 127.0.0.1:20998 --din-cookie /path/to/datadir/.cookie --din-hrp din \
  --btc-chain main --btc-backend light \
  --btc-servers https://mempool.space/api,https://blockstream.info/api \
  --interval 30 --expire-unfunded-after 604800
```

`--din-cookie` is the node's cookie file; the tower reads it again whenever the node restarts and
writes a new one (`--din-auth USER:PASS` instead, for a node with a fixed user and password). The
tower's user must be able to read that file.

Use `--network testnet`, `--din-hrp tdin`, `--btc-chain testnet4` and the testnet servers for a
testnet tower. One tower serves one network; the door refuses packages of any other.

Run each under the service manager with restart-on-exit. A restart loses nothing: the tower reads
the inbox again and works out every swap's state from the chains.

## The web server

It must: end TLS; pass `/v1/` to `127.0.0.1:8742` and nothing else; buffer request bodies; refuse
bodies over 256 KiB; limit each address to a few requests a second. For nginx:

```
limit_req_zone $binary_remote_addr zone=swaptower:10m rate=5r/s;
server {
    listen 443 ssl;
    server_name TOWER_HOST;
    client_max_body_size 256k;
    location /v1/ {
        limit_req zone=swaptower burst=20 nodelay;
        proxy_pass http://127.0.0.1:8742;
        proxy_read_timeout 15s;
    }
    location / { return 404; }
}
```

## Checking it

```
curl -s https://TOWER_HOST/v1/health                      # {"ok":true}
curl -s -o /dev/null -w '%{http_code}\n' -d junk https://TOWER_HOST/v1/packages   # 400
```

In the inbox: `<hash>.pkg` is a swap being guarded, `<hash>.armed` its last confirmation (it should
be seconds old while both chains are reachable), `.pkg.done` a finished swap, `.pkg.expired` one
whose buyer never locked, `.pkg.rejected` should never appear (the door verifies first).

Watch for, in the tower's output: `Bitcoin:` lines (its view of Bitcoin has a problem: too few
servers, a stale tip), `not observed` (a node is unreachable) and `ALERT`. While any of these
lasts the tower stops confirming, so buyers stop locking: nobody is put at risk by an outage, but
swaps that are already locked depend on the tower coming back before their deadlines.

## What to keep

The inbox is the only state. Losing it loses protection for the swaps in it (their owners' own
nodes still act while online) and nothing else. It holds no secrets beyond the swaps' terms.
