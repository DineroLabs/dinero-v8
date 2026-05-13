# Dinero RPC Authentication Guide

This document describes how to authenticate with the Dinero RPC server using Basic Authentication.

## Overview

The Dinero RPC server uses HTTP Basic Authentication with credentials stored in a cookie file. The server supports two authentication formats:

1. **user:pass** - Direct format where the cookie file contains `username:password`
2. **__cookie__:TOKEN** - Compatible format where the cookie file contains just the token

## Port Configuration

- **20998** → RPC (HTTP/1.1 JSON-RPC) — GUI uses this
- **21001** → WebSocket `/ws` (optional realtime bus) — GUI subscribes here
- **20999** → P2P (node networking) — GUI never connects here

## Authentication Methods

### Method 1: Direct user:pass

If your cookie file contains `username:password`:

```bash
# Read credentials from cookie file
CREDS=$(cat ./data/.cookie)
AUTH=$(echo -n "$CREDS" | base64)

# Make RPC call
curl -H "Authorization: Basic $AUTH" \
     -H 'Content-Type: application/json' \
     --data '{"jsonrpc":"2.0","id":1,"method":"getblockchaininfo","params":[]}' \
     http://127.0.0.1:20998/
```

### Method 2: __cookie__:TOKEN format

If your cookie file contains just the token:

```bash
# Extract token and create __cookie__:TOKEN format
TOKEN=$(cat ./data/.cookie)
CREDS="__cookie__:$TOKEN"
AUTH=$(echo -n "$CREDS" | base64)

# Make RPC call
curl -H "Authorization: Basic $AUTH" \
     -H 'Content-Type: application/json' \
     --data '{"jsonrpc":"2.0","id":1,"method":"getblockchaininfo","params":[]}' \
     http://127.0.0.1:20998/
```

## Qt Client Authentication

For Qt applications, connect to the `authenticationRequired` signal:

```cpp
connect(networkManager, &QNetworkAccessManager::authenticationRequired,
        this, [this](QNetworkReply*, QAuthenticator* authenticator) {
    // Parse cookie file
    QFile cookieFile("./data/.cookie");
    if (cookieFile.open(QIODevice::ReadOnly)) {
        QString cookieData = QString::fromUtf8(cookieFile.readAll().trimmed());
        
        // Check if it's user:pass or just token
        if (cookieData.contains(':')) {
            QStringList parts = cookieData.split(':');
            authenticator->setUser(parts[0]);
            authenticator->setPassword(parts.mid(1).join(':'));
        } else {
            // Just token, use __cookie__:token format
            authenticator->setUser("__cookie__");
            authenticator->setPassword(cookieData);
        }
    }
});
```

## Error Responses

On authentication failure, the server returns:

```http
HTTP/1.1 401 Unauthorized
Content-Type: application/json
WWW-Authenticate: Basic realm="Dinero RPC"

"Unauthorized"
```

## Testing

Use the provided smoke test script:

```bash
# Make sure daemon is running
./scripts/run_node.sh

# Test authentication
./scripts/smoke_rpc.sh
```

## Troubleshooting

### Common Issues

1. **401 Unauthorized** → Check cookie file exists and has correct format
2. **Qt Status: 0 after 401** → Ensure `authenticationRequired` signal is connected
3. **Mixed ports** → RPC must be 20998; WS 21001; never hit P2P from GUI

### Debug Mode

Enable server debug logs to see authentication details:

```bash
./build/bin/dinerod -printtoconsole -rpcport=20998 -port=20999
```

### Cookie File Location

The cookie file is typically located at:
- `./data/.cookie` (default)
- `./data/mainnet/.cookie` (mainnet specific)

## Security Notes

- Keep cookie files out of version control
- Cookie files contain sensitive credentials
- Use HTTPS in production environments
- Rotate credentials regularly
