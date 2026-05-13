# Meta RPC Methods

Meta RPC methods provide introspection and health monitoring capabilities for the DineroCoin daemon.

## rpc.capabilities

Returns the RPC server capabilities and feature set.

**Request:**
```json
{
  "jsonrpc": "2.0",
  "method": "rpc.capabilities",
  "params": [],
  "id": 1
}
```

**Response:**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "versions": {
      "daemon": "1.0.0",
      "rpc": 1
    },
    "features": {
      "authentication": "cookie",
      "transport": ["http", "websocket"],
      "style": "namespaced",
      "url_scoping": true,
      "wallet_path_supported": true,
      "method_aliasing": true,
      "legacy_aliases": false
    },
    "namespaces": ["wallet", "blockchain", "mempool", "mining", "rpc"],
    "methods": {
      "wallet": 5,
      "blockchain": 3,
      "mempool": 1,
      "mining": 2,
      "rpc": 4
    },
    "deprecation": {
      "legacy_aliases_removed": true
    }
  }
}
```

## rpc.listmethods

Lists all available RPC methods.

**Request:**
```json
{
  "jsonrpc": "2.0",
  "method": "rpc.listmethods",
  "params": [],
  "id": 1
}
```

**Response:**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": [
    "wallet.create",
    "wallet.load",
    "wallet.getnewaddress",
    "wallet.validateaddress",
    "wallet.listaddresses",
    "blockchain.getbestblockhash",
    "blockchain.getblockhash",
    "blockchain.info",
    "mining.setaddress",
    "mining.getaddress",
    "rpc.capabilities",
    "rpc.listmethods",
    "rpc.help",
    "rpc.health"
  ]
}
```

## rpc.help

Provides detailed help documentation for RPC methods.

**Request (general help):**
```json
{
  "jsonrpc": "2.0",
  "method": "rpc.help",
  "params": [],
  "id": 1
}
```

**Request (specific method help):**
```json
{
  "jsonrpc": "2.0",
  "method": "rpc.help",
  "params": ["mining.getaddress"],
  "id": 1
}
```

**Response (specific method):**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "name": "mining.getaddress",
    "description": "Get the current mining address for the active or scoped wallet",
    "params": [],
    "result": {
      "type": "object",
      "description": "Mining address information",
      "properties": {
        "address": {"type": "string", "description": "Current mining address"},
        "wallet": {"type": "string", "description": "Wallet name"},
        "network": {"type": "string", "description": "Network HRP"},
        "ismine": {"type": "boolean", "description": "Address is owned by wallet"},
        "source": {"type": "string", "description": "configured|derived|none"}
      }
    }
  }
}
```

## rpc.health

Comprehensive health check endpoint for monitoring and readiness probes.

**Request:**
```json
{
  "jsonrpc": "2.0",
  "method": "rpc.health",
  "params": [],
  "id": 1
}
```

**Response:**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "status": "healthy",
    "timestamp": 1757196555,
    "rpc": {
      "port": 20996,
      "methods": 92,
      "ready": true
    },
    "blockchain": {
      "height": 0,
      "hash": "",
      "ready": true
    },
    "wallet": {
      "manager_ready": true,
      "has_active": false
    },
    "mining": {
      "enabled": false,
      "ready": true
    }
  }
}
```

**Health Status Fields:**
- `status`: Overall health status ("healthy" or "unhealthy")
- `timestamp`: Current Unix timestamp
- `rpc.ready`: RPC server is operational
- `blockchain.ready`: Blockchain component is initialized
- `wallet.manager_ready`: Wallet manager is available
- `wallet.has_active`: An active wallet is loaded
- `mining.ready`: Mining component is initialized
- `mining.enabled`: Mining is currently active
