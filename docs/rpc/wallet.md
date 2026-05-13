# Wallet RPC Methods

Wallet RPC methods provide comprehensive wallet management functionality with support for wallet-scoped URLs.

## URL Scoping

Wallet methods can be called using wallet-scoped URLs to eliminate context ambiguity:

```bash
# Traditional approach (uses active wallet)
curl -u "$AUTH" -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","method":"wallet.getnewaddress","id":1}' \
  http://127.0.0.1:20996/

# Wallet-scoped approach (explicit wallet context)
curl -u "$AUTH" -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","method":"wallet.getnewaddress","id":1}' \
  http://127.0.0.1:20996/wallet/my_wallet
```

## wallet.create

Creates a new HD wallet with BIP39 seed phrase generation.

**Request:**
```json
{
  "jsonrpc": "2.0",
  "method": "wallet.create",
  "params": {"name": "my_wallet"},
  "id": 1
}
```

**Response:**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "name": "my_wallet",
    "success": true,
    "seed_phrase": "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about"
  }
}
```

## wallet.load

Loads an existing wallet and sets it as the active wallet.

**Request:**
```json
{
  "jsonrpc": "2.0",
  "method": "wallet.load",
  "params": {"name": "my_wallet"},
  "id": 1
}
```

**Response:**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "name": "my_wallet",
    "ok": true
  }
}
```

## wallet.getnewaddress

Derives a new receiving address using BIP84 derivation path.

**Request:**
```json
{
  "jsonrpc": "2.0",
  "method": "wallet.getnewaddress",
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
    "address": "rdin1qz8nwny729fx4u529l0pt2l4hghn60fkmts3zd4",
    "index": 0,
    "path": "m/84'/1'/0'/0/0",
    "purpose": "receive"
  }
}
```

**Address Format by Network:**
- **Mainnet**: `din1...` (HRP: din)
- **Testnet**: `tdin1...` (HRP: tdin)  
- **Regtest**: `rdin1...` (HRP: rdin)

## wallet.validateaddress

Validates an address and returns detailed information about it.

**Request:**
```json
{
  "jsonrpc": "2.0",
  "method": "wallet.validateaddress",
  "params": ["rdin1qz8nwny729fx4u529l0pt2l4hghn60fkmts3zd4"],
  "id": 1
}
```

**Response:**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "isvalid": true,
    "address": "rdin1qz8nwny729fx4u529l0pt2l4hghn60fkmts3zd4",
    "scriptPubKey": "001411e6e993ca2a4d5e5145fbc2b57eb745e7a7a6db",
    "ismine": true,
    "iswatchonly": false,
    "isscript": false,
    "iswitness": true,
    "witness_version": 0,
    "witness_program": "11e6e993ca2a4d5e5145fbc2b57eb745e7a7a6db",
    "pubkey": "0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798",
    "iscompressed": true,
    "label": "",
    "timestamp": 1757196555,
    "hdkeypath": "m/84'/1'/0'/0/0",
    "hdseedid": "0000000000000000000000000000000000000000",
    "hdmasterfingerprint": "00000000",
    "labels": []
  }
}
```

## wallet.listaddresses

Lists all addresses in the wallet with their metadata.

**Request:**
```json
{
  "jsonrpc": "2.0",
  "method": "wallet.listaddresses",
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
    {
      "address": "rdin1qz8nwny729fx4u529l0pt2l4hghn60fkmts3zd4",
      "label": "",
      "purpose": "receive",
      "hdkeypath": "m/84'/1'/0'/0/0",
      "timestamp": 1757196555
    },
    {
      "address": "rdin1qy8nwny729fx4u529l0pt2l4hghn60fkmts3zd5",
      "label": "",
      "purpose": "receive", 
      "hdkeypath": "m/84'/1'/0'/0/1",
      "timestamp": 1757196556
    }
  ]
}
```

## Error Handling

**Wallet Not Found:**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "error": {
    "code": -18,
    "message": "Wallet not found: nonexistent_wallet"
  }
}
```

**Wallet Already Exists:**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "error": {
    "code": -4,
    "message": "Wallet already exists: my_wallet"
  }
}
```

**Invalid Address:**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "error": {
    "code": -5,
    "message": "Invalid address format"
  }
}
```

## Security Notes

- All wallet operations require cookie-based authentication
- Seed phrases are generated using cryptographically secure randomness
- Private keys never leave the wallet database
- HD derivation follows BIP32/BIP44/BIP84 standards
- Wallet-scoped URLs ensure operations target the correct wallet context
