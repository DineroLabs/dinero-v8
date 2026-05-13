# 🏦 DineroCoin Exchange Integration Guide

## For Binance, Coinbase, Kraken, and Other Exchanges

This document provides comprehensive integration instructions for cryptocurrency exchanges to support DineroCoin (DIN) with full confidential transaction support.

---

## 📋 Table of Contents

1. [Overview](#overview)
2. [Deposit Integration](#deposit-integration)
3. [Withdrawal Integration](#withdrawal-integration)
4. [RPC API Reference](#rpc-api-reference)
5. [Confidential Transaction Handling](#confidential-transaction-handling)
6. [Security Considerations](#security-considerations)
7. [Testing](#testing)

---

## Overview

### What is DineroCoin?

DineroCoin is a privacy-focused cryptocurrency with:
- **Optional confidential transactions** (like Monero, Zcash)
- **Bulletproof range proofs** (compact, efficient)
- **Bitcoin-compatible UTXO model**
- **Transparent and confidential addresses**

### Integration Modes

Exchanges can choose one of two integration modes:

| Mode | Type | Privacy | Complexity |
|------|------|---------|------------|
| **Mode A** | Transparent Only | Public amounts | Low (like Bitcoin) |
| **Mode B** | Mixed (Transparent + Confidential) | Private amounts | Medium (like Monero) |

**Recommendation**: Start with Mode A, add Mode B later for advanced users.

---

## Deposit Integration

### Mode A: Transparent Deposits (Recommended)

**How it works**: Just like Bitcoin!

1. **Generate deposit address**:
   ```bash
   dinero-cli getnewaddress
   ```
   Returns: `din1q...` (Bech32 address)

2. **Monitor deposits**:
   ```bash
   dinero-cli listreceivedbyaddress 6 true
   ```
   Shows all received transactions with 6+ confirmations

3. **Credit user account**:
   - Wait for 6 confirmations (recommended)
   - Credit exact amount to user's exchange balance

**Example**:
```json
{
  "address": "din1qxy2kgdygjrsqtzq2n0yrf2493p83kkfjhx0wlh",
  "account": "",
  "amount": 10.50000000,
  "confirmations": 6,
  "txids": ["abc123..."]
}
```

### Mode B: Confidential Deposits (Advanced)

**How it works**: Like Monero with view keys

1. **Generate confidential address**:
   ```bash
   dinero-cli getconfidentialaddress
   ```
   Returns: `C9xF34K...L5fP2` (Base58 confidential address)

2. **Export view key** (one-time setup):
   ```bash
   dinero-cli getviewkey
   ```
   Returns: `b97a8e12cd4f7be2c31299aabc43fdf1...`

3. **Scan for deposits**:
   ```bash
   dinero-cli rescanconfidential
   ```
   Decrypts all confidential outputs owned by this wallet

4. **Credit user account**:
   - Wait for 6 confirmations
   - Decrypt amount using view key
   - Credit to user balance

**Why use Mode B?**
- Better privacy for users
- Regulatory compliance in privacy-friendly jurisdictions
- Competitive advantage (few exchanges support confidential deposits)

---

## Withdrawal Integration

### Mode A: Transparent Withdrawals

**Standard Bitcoin-style flow**:

1. **User requests withdrawal**:
   - User provides destination address: `din1q...`
   - User specifies amount: `1.5 DIN`

2. **Send transaction**:
   ```bash
   dinero-cli sendtoaddress "din1qxy2..." 1.5
   ```

3. **Return TXID to user**:
   ```json
   {
     "txid": "abc123...",
     "confirmations": 0
   }
   ```

### Mode B: Confidential Withdrawals

**Private Bitcoin-style flow**:

1. **User requests confidential withdrawal**:
   - User provides destination: `C9xF34K...` (confidential address)
   - User specifies amount: `1.5 DIN`

2. **Import recipient's address** (automatic view key lookup):
   ```bash
   dinero-cli importconfidentialaddress "C9xF34K..."
   ```

3. **Send confidential transaction**:
   ```bash
   dinero-cli sendconfidential "C9xF34K..." 1.5
   ```

4. **Return TXID**:
   ```json
   {
     "txid": "xyz789...",
     "confirmations": 0,
     "confidential": true
   }
   ```

---

## RPC API Reference

### 1. Blockchain Info

#### `getblockchaininfo`
Returns blockchain state.

**Request**:
```bash
dinero-cli getblockchaininfo
```

**Response**:
```json
{
  "chain": "main",
  "blocks": 123456,
  "headers": 123456,
  "bestblockhash": "000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f",
  "difficulty": 1234567.89,
  "mediantime": 1699564800,
  "verificationprogress": 0.999999,
  "chainwork": "00000000000000000000000000000000000000000000000000012345abcdef"
}
```

#### `getblockcount`
Returns current block height.

**Request**:
```bash
dinero-cli getblockcount
```

**Response**:
```json
123456
```

#### `getblockhash <height>`
Returns block hash at height.

**Request**:
```bash
dinero-cli getblockhash 123456
```

**Response**:
```json
"000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f"
```

#### `getblock <hash>`
Returns block details.

**Request**:
```bash
dinero-cli getblock "000000000019d6689..."
```

**Response**:
```json
{
  "hash": "000000000019d6689...",
  "confirmations": 100,
  "height": 123456,
  "version": 2,
  "merkleroot": "4a5e1e4baab89f3...",
  "time": 1699564800,
  "nonce": 2083236893,
  "bits": "1d00ffff",
  "difficulty": 1.0,
  "tx": ["txid1", "txid2", ...]
}
```

#### `getrawtransaction <txid> <verbose>`
Returns transaction details.

**Request**:
```bash
dinero-cli getrawtransaction "abc123..." true
```

**Response** (Transparent TX):
```json
{
  "txid": "abc123...",
  "version": 2,
  "locktime": 0,
  "vin": [...],
  "vout": [
    {
      "value": 1.50000000,
      "n": 0,
      "scriptPubKey": {
        "asm": "OP_0 a914...",
        "hex": "0014a914...",
        "address": "din1qxy2..."
      }
    }
  ]
}
```

**Response** (Confidential TX):
```json
{
  "txid": "xyz789...",
  "version": 2,
  "vin": [...],
  "vout": [
    {
      "value": 0,
      "confidential": true,
      "commitment": "02abc123...",
      "range_proof": "0424fd...",
      "nonce": "03def456...",
      "scriptPubKey": {
        "asm": "OP_0 <32-byte-hash>",
        "hex": "0020abc123..."
      }
    }
  ]
}
```

---

### 2. Address & Keys

#### `validateaddress <address>`
Validates an address.

**Request**:
```bash
dinero-cli validateaddress "din1qxy2..."
```

**Response**:
```json
{
  "isvalid": true,
  "address": "din1qxy2...",
  "scriptPubKey": "0014a914...",
  "ismine": false,
  "iswatchonly": false,
  "isscript": false,
  "iswitness": true
}
```

#### `getnewaddress`
Generates new address.

**Request**:
```bash
dinero-cli getnewaddress
```

**Response**:
```json
"din1qxy2kgdygjrsqtzq2n0yrf2493p83kkfjhx0wlh"
```

#### `listreceivedbyaddress <minconf> <include_empty>`
Lists received amounts.

**Request**:
```bash
dinero-cli listreceivedbyaddress 6 false
```

**Response**:
```json
[
  {
    "address": "din1qxy2...",
    "amount": 10.50000000,
    "confirmations": 12,
    "txids": ["abc123..."]
  }
]
```

---

### 3. Wallet

#### `getbalance`
Returns wallet balance.

**Request**:
```bash
dinero-cli getbalance
```

**Response**:
```json
{
  "transparent": 10.50000000,
  "confidential": 5.25000000,
  "total": 15.75000000
}
```

#### `getwalletinfo`
Returns wallet information.

**Request**:
```bash
dinero-cli getwalletinfo
```

**Response**:
```json
{
  "walletname": "wallet.dat",
  "walletversion": 169900,
  "balance": 15.75000000,
  "unconfirmed_balance": 0.0,
  "immature_balance": 0.0,
  "txcount": 42,
  "keypoolsize": 1000,
  "unlocked_until": 0,
  "paytxfee": 0.00001000
}
```

#### `sendtoaddress <address> <amount>`
Sends transparent transaction.

**Request**:
```bash
dinero-cli sendtoaddress "din1qxy2..." 1.5
```

**Response**:
```json
"abc123def456...txid..."
```

#### `sendrawtransaction <hex>`
Broadcasts raw transaction.

**Request**:
```bash
dinero-cli sendrawtransaction "0200000001..."
```

**Response**:
```json
"txid..."
```

---

### 4. Confidential-Specific Methods

#### `getconfidentialbalance`
Returns confidential balance only.

**Request**:
```bash
dinero-cli getconfidentialbalance
```

**Response**:
```json
5.25000000
```

#### `gettotalbalance`
Returns all balances.

**Request**:
```bash
dinero-cli gettotalbalance
```

**Response**:
```json
{
  "confirmed": 15.75000000,
  "unconfirmed": 0.0,
  "immature": 0.0,
  "transparent": 10.50000000,
  "confidential": 5.25000000
}
```

#### `listconfidential`
Lists confidential outputs.

**Request**:
```bash
dinero-cli listconfidential
```

**Response**:
```json
[
  {
    "txid": "xyz789...",
    "vout": 0,
    "amount": 2.50000000,
    "confirmations": 6,
    "commitment": "02abc123...",
    "blinding_factor": "b97a8e12..."
  }
]
```

#### `rescanconfidential`
Rescans blockchain for confidential outputs.

**Request**:
```bash
dinero-cli rescanconfidential
```

**Response**:
```json
{
  "found": 42,
  "scanned_height": 123456
}
```

#### `getviewkey`
Exports view key (for shared viewing).

**Request**:
```bash
dinero-cli getviewkey
```

**Response**:
```json
{
  "viewkey_private": "b97a8e12cd4f7be2...",
  "viewkey_public": "02abc123def456...",
  "fingerprint": "b97a8e12cd4f7be2"
}
```

#### `sendconfidential <address> <amount>`
Sends confidential transaction.

**Request**:
```bash
dinero-cli sendconfidential "C9xF34K..." 1.5
```

**Response**:
```json
{
  "txid": "xyz789...",
  "fee": 0.00001000,
  "confidential": true
}
```

---

## Confidential Transaction Handling

### For Deposits

**Step-by-Step Process**:

1. **Setup** (one-time):
   ```bash
   # Generate confidential address
   dinero-cli getconfidentialaddress

   # Export view key
   dinero-cli getviewkey

   # Store view key securely
   ```

2. **Give address to user**:
   - User deposits to `C9xF34K...`

3. **Scan for deposits**:
   ```bash
   # Automated scanning (runs every 60 seconds)
   dinero-cli rescanconfidential
   ```

4. **Decrypt amounts**:
   ```bash
   # List decrypted outputs
   dinero-cli listconfidential
   ```

5. **Credit user**:
   - After 6 confirmations
   - Credit exact decrypted amount

### For Withdrawals

**Step-by-Step Process**:

1. **Validate user's address**:
   ```bash
   dinero-cli validateconfidentialaddress "C9xF34K..."
   ```

2. **Import address** (automatic view key extraction):
   ```bash
   dinero-cli importconfidentialaddress "C9xF34K..."
   ```

3. **Send confidential**:
   ```bash
   dinero-cli sendconfidential "C9xF34K..." 1.5
   ```

4. **Return TXID to user**

---

## Security Considerations

### 1. View Key Security

**Risk**: View key can decrypt all confidential balances

**Mitigation**:
- Store view key in encrypted vault
- Use hardware security module (HSM) if possible
- Limit view key access to authorized personnel

### 2. Hot Wallet Security

**Best Practices**:
- Use multisig for hot wallet
- Keep majority of funds in cold storage
- Implement withdrawal limits
- Enable 2FA for all RPC operations

### 3. Deposit Confirmation Requirements

**Recommended**:
- Small deposits (< 10 DIN): 6 confirmations
- Medium deposits (10-100 DIN): 12 confirmations
- Large deposits (> 100 DIN): 24 confirmations

### 4. RPC Authentication

**Always enable**:
```conf
# dinero.conf
rpcuser=exchange_user
rpcpassword=secure_random_password_here
rpcallowip=127.0.0.1
rpcport=8332
```

**Use HTTPS** for RPC calls (nginx reverse proxy recommended)

---

## Testing

### Testnet Setup

1. **Run testnet node**:
   ```bash
   dinerod -testnet
   ```

2. **Get testnet coins**:
   - Visit: `https://testnet-faucet.dinero-coin.com`
   - Enter testnet address
   - Receive test DIN

3. **Test deposit flow**:
   ```bash
   # Generate testnet address
   dinero-cli -testnet getnewaddress

   # Send to yourself from faucet
   # Monitor reception
   dinero-cli -testnet listreceivedbyaddress 1 true
   ```

4. **Test withdrawal flow**:
   ```bash
   # Send to another address
   dinero-cli -testnet sendtoaddress "din1test..." 0.1
   ```

5. **Test confidential flow**:
   ```bash
   # Get confidential address
   dinero-cli -testnet getconfidentialaddress

   # Send confidential
   dinero-cli -testnet sendconfidential "Ctest..." 0.1

   # Scan for receipt
   dinero-cli -testnet rescanconfidential
   ```

### Regtest Setup (Private Testing)

```bash
# Start regtest
dinerod -regtest

# Generate blocks
dinero-cli -regtest generate 101

# Test instantly without waiting for confirmations
```

---

## Support

### Documentation
- Website: `https://dinero-coin.com`
- Docs: `https://docs.dinero-coin.com`
- GitHub: `https://github.com/Trucker2827/Dinero-Coin`

### Exchange Support
- Email: `exchanges@dinero-coin.com`
- Telegram: `@DineroCoinExchangeSupport`
- Response time: < 24 hours

### Bug Reports
- GitHub Issues: `https://github.com/Trucker2827/Dinero-Coin/issues`
- Security: `security@dinero-coin.com` (PGP encouraged)

---

## Appendix: Address Formats

### Transparent Address (Bech32)
```
Format: din1q...
Example: din1qxy2kgdygjrsqtzq2n0yrf2493p83kkfjhx0wlh
Length: 42-62 characters
Type: Public amounts
```

### Confidential Address (Base58)
```
Format: C...
Example: C9xF34K2mNpR8vL5fP2qD7eW1jH3sY6tB4
Length: ~95 characters
Type: Private amounts (view key required)
```

---

**Last Updated**: 2025-11-17
**API Version**: 1.0.0
**Document Version**: 1.0

---
