# Dinero RPC Auth & WebSocket — Fix + Persist Checklist

> A concise, long‑lived reference for what we changed, how to rebuild, how to verify, and how to avoid regressions.

---

## ✅ What’s fixed
- **Server auth** now decodes `Authorization: Basic …` and compares to the cookie’s `user:pass` (accepts both `__cookie__:TOKEN` *and* `user:pass` formats).
- **WWW-Authenticate** header returned on 401 (Qt/curl interop).
- **Qt client** wired with `authenticationRequired` and HTTP/1.1 (no proxy surprises).
- Optional **WebSocket bus** enabled (auth-reusing) for live GUI updates.

---

## 📁 Files added/changed
```
src/rpc/auth_cookie.hpp
src/rpc/auth_cookie.cpp
(rpc handler) uses: check_basic_authorization()/auth_ok()
(CMake) target_sources: add the two files to rpc/daemon targets
```

> Tip: keep cookie file **out of git** (see .gitignore below).

---

## 🛠️ Server auth (reference impl)
- Load cookie as **`user:pass`** if the file contains a colon; otherwise treat as `__cookie__:TOKEN`.
- Base64‑**decode** the request’s `Authorization` value and compare **strings**.
- On failure, return:
  ```http
  401 Unauthorized
  WWW-Authenticate: Basic realm="Dinero RPC"
  Content-Type: application/json
  "Unauthorized"
  ```

---

## 🔌 Port map (use consistently)
- **20998** → RPC (HTTP/1.1 JSON-RPC) — GUI uses this.
- **18332** → WebSocket `/ws` (optional realtime bus) — GUI subscribes here.
- **20999** → P2P (node networking) — GUI never connects here.

---

## 🧪 Smoke tests (copy/paste)

### Curl (A/B both should succeed)
```bash
# A) Send cookie line AS-IS (user:pass)
AUTH_A=$(echo -n "$(cat ./data/.cookie)" | base64)
curl -i -H "Authorization: Basic $AUTH_A" -H 'Content-Type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"getblockchaininfo","params":[]}' \
  http://127.0.0.1:20998/

# B) Compose __cookie__:TOKEN (compatible form)
TOKEN=$(cut -d: -f2- ./data/.cookie)
AUTH_B=$(echo -n "__cookie__:$TOKEN" | base64)
curl -i -H "Authorization: Basic $AUTH_B" -H 'Content-Type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"getblockchaininfo","params":[]}' \
  http://127.0.0.1:20998/
```

### WebSocket (if enabled)
```bash
# brew install websocat
AUTH=$(echo -n "$(cat ./data/.cookie)" | base64)
websocat -H="Authorization: Basic $AUTH" ws://127.0.0.1:18332/ws
# then type:
# {"type":"sub","topics":["blocks","mining"]}
```

---

## 🧱 Qt client (minimal pattern)
- One persistent `QNetworkAccessManager`.
- Connect `authenticationRequired` and set `user="__cookie__"`, `password=<cookie token>`.
- Disable HTTP/2 via `Http2AllowedAttribute=false`. Avoid proxies.

---

## 🧰 Scripts to keep

### `scripts/run_node.sh`
```bash
#!/usr/bin/env bash
set -euo pipefail
./build/bin/dinerod -daemon -rpcport=20998 -port=20999
```

### `scripts/smoke_rpc.sh`
```bash
#!/usr/bin/env bash
set -euo pipefail
AUTH=$(echo -n "$(cat ./data/.cookie)" | base64)
curl -s -H "Authorization: Basic $AUTH" -H 'Content-Type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"getblockchaininfo","params":[]}' \
  http://127.0.0.1:20998/ | jq .
```

> Make them executable: `chmod +x scripts/*.sh`

---

## 🧪 CI/Tests (quick wins)
- **Unit:** base64 decode → equals cookie `user:pass` (handles both formats).
- **Integration:** run `scripts/run_node.sh`, then `scripts/smoke_rpc.sh` in CI; fail if HTTP ≠ 200.
- **Fuzz:** header case/leading/trailing whitespace.

---

## 🔐 .gitignore additions
```
# runtime data
/data/
*.db
*.sqlite

# auth cookie
/data/.cookie

# binaries
/build/
/bin/
*.o
```

---

## 📝 Docs to add
- `docs/RPC_AUTH.md`
  - Cookie file formats accepted (user:pass or token → __cookie__:token)
  - How to auth (curl, Qt) with examples
  - Port map: 20998 RPC, 18332 WS, 20999 P2P
  - Troubleshooting: enable server debug logs; compare expected vs got creds

---

## 🧭 Git/Release flow
```bash
git checkout -b feat/unified-auth-ws
# add modified/new files
git add src/rpc/auth_cookie.* src/daemon/* rpc/* CMakeLists.txt docs/RPC_AUTH.md scripts/

# sanity build
cmake --build build --target dinerod -j8

# verify
./scripts/run_node.sh & sleep 2
./scripts/smoke_rpc.sh

# commit + tag
git commit -m "rpc: unify Basic auth (decode & compare to cookie user:pass); add WS reuse; docs+smoke"
git tag -a v0.1.0-authfix -m "Auth hardening + WS reuse"

git push origin feat/unified-auth-ws --tags
```

---

## 🧯 Troubleshooting cheatsheet
- `401 Unauthorized` in curl/Qt → dump expected vs got (temporary log):
  ```cpp
  LOG_INFO("AUTH expected:'" + expected + "'");
  LOG_INFO("AUTH got     :'" + decoded  + "'");
  ```
- Qt sees `Status: 0` after 401 → ensure `authenticationRequired` is connected and you’re not forcing `Connection: close` client-side.
- Mixed ports → RPC must be 20998; WS 18332; never hit P2P from GUI.

---

## 🔮 Future niceties
- Add `-wsauth=off` dev flag for WS during local demos.
- Optional token rotation endpoint; short cache already handled.
- TLS termination (nginx) if exposing beyond localhost.

---

**This doc is your single source of truth** to keep auth solid across restarts, rebuilds, and contributors. Save it, commit it, and ship it. 🚀

