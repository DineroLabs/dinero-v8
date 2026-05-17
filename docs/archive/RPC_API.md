# 📡 DineroCoin RPC API Reference

Complete reference for all DineroCoin RPC methods.

**Version**: v0.1.0  
**Total Methods**: 58

---

## 📋 Table of Contents

1. [Authentication](#authentication)
2. [Blockchain RPCs](#blockchain-rpcs) (15 methods)
3. [Wallet RPCs](#wallet-rpcs) (20 methods)
4. [Mining RPCs](#mining-rpcs) (5 methods)
5. [Network RPCs](#network-rpcs) (8 methods)
6. [Utility RPCs](#utility-rpcs) (7 methods)
7. [Dinero-Specific RPCs](#dinero-specific-rpcs) (3 methods)

---

## Authentication

All RPC calls require cookie authentication:

```bash
# Cookie location
~/.dinero/.cookie  # Mainnet
./your-datadir/.cookie  # Custom datadir

# Usage
curl -u "$(cat ~/.dinero/.cookie)" \
  -X POST http://127.0.0.1:20998/ \
  -H "Content-Type: application/json" \
  -d '{"method":"<method>","params":[...],"id":1}'
```

**Ports:**
- Mainnet RPC: `20998`
- Mainnet P2P: `20999`
- Regtest RPC: Custom (specified with `--rpcport`)

---

## Blockchain RPCs

### getblockchaininfo

Get detailed blockchain information.

**Parameters:** None

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"getblockchaininfo","params":[],"id":1}'
```

**Response:**
```json
{
  "result": {
    "chain": "main",
    "blocks": 296,
    "headers": 296,
    "bestblockhash": "0000001a2b3c...",
    "difficulty": 491733567,
    "mediantime": 1729436782,
    "verificationprogress": 1.0,
    "chainwork": "00000000000000...00a5",
    "size_on_disk": 12458960
  }
}
```

---

### getblockcount

Get current blockchain height.

**Parameters:** None

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"getblockcount","params":[],"id":1}'
```

**Response:**
```json
{
  "result": 296
}
```

---

### getblockhash

Get block hash by height.

**Parameters:**
1. `height` (integer) - Block height

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"getblockhash","params":[100],"id":1}'
```

**Response:**
```json
{
  "result": "00000a1b2c3d..."
}
```

---

### getblock

Get block data by hash.

**Parameters:**
1. `blockhash` (string) - Block hash
2. `verbosity` (integer, optional) - 0=hex, 1=json, 2=json+txs (default: 1)

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"getblock","params":["00000a1b2c...", 1],"id":1}'
```

**Response:**
```json
{
  "result": {
    "hash": "00000a1b2c...",
    "confirmations": 196,
    "height": 100,
    "version": 1,
    "merkleroot": "abc123...",
    "time": 1729436000,
    "nonce": 123456,
    "bits": "21ffffff",
    "difficulty": 491733567,
    "tx": ["txid1...", "txid2..."]
  }
}
```

---

### getbestblockhash

Get hash of the best (tip) block.

**Parameters:** None

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"getbestblockhash","params":[],"id":1}'
```

**Response:**
```json
{
  "result": "0000001a2b3c..."
}
```

---

### getblockheader

Get block header information.

**Parameters:**
1. `blockhash` (string) - Block hash
2. `verbose` (boolean, optional) - true=json, false=hex (default: true)

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"getblockheader","params":["00000a1b...", true],"id":1}'
```

---

### getchaintips

Get information about all known chain tips.

**Parameters:** None

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"getchaintips","params":[],"id":1}'
```

---

### getdifficulty

Get current network difficulty.

**Parameters:** None

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"getdifficulty","params":[],"id":1}'
```

**Response:**
```json
{
  "result": 491733567
}
```

---

### gettxout

Get details about an unspent transaction output (UTXO).

**Parameters:**
1. `txid` (string) - Transaction ID
2. `vout` (integer) - Output index
3. `include_mempool` (boolean, optional) - Include mempool (default: true)

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"gettxout","params":["abc123...", 0, true],"id":1}'
```

---

### gettxoutsetinfo

Get statistics about the UTXO set.

**Parameters:** None

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"gettxoutsetinfo","params":[],"id":1}'
```

---

### getrawtransaction

Get raw transaction data.

**Parameters:**
1. `txid` (string) - Transaction ID
2. `verbose` (boolean, optional) - true=json, false=hex (default: false)

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"getrawtransaction","params":["abc123...", true],"id":1}'
```

---

### decoderawtransaction

Decode a raw transaction hex string.

**Parameters:**
1. `hexstring` (string) - Raw transaction hex

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"decoderawtransaction","params":["0100000001..."],"id":1}'
```

---

### getmempoolinfo

Get mempool information.

**Parameters:** None

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"getmempoolinfo","params":[],"id":1}'
```

**Response:**
```json
{
  "result": {
    "size": 42,
    "bytes": 12580,
    "usage": 25600,
    "maxmempool": 300000000,
    "mempoolminfee": 0.00001000
  }
}
```

---

### getrawmempool

Get all transaction IDs in mempool.

**Parameters:**
1. `verbose` (boolean, optional) - true=details, false=txids (default: false)

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"getrawmempool","params":[false],"id":1}'
```

---

### getchaintxstats

Get statistics about blockchain transactions.

**Parameters:**
1. `nblocks` (integer, optional) - Number of blocks to analyze (default: 1 month)

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"getchaintxstats","params":[100],"id":1}'
```

---

## Wallet RPCs

### createhdwallet

Create new HD (Hierarchical Deterministic) wallet.

**Parameters:** None

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"createhdwallet","params":[],"id":1}'
```

**Response:**
```json
{
  "result": {
    "success": true,
    "wallet_id": "default",
    "message": "HD wallet created successfully"
  }
}
```

---

### getnewaddress

Generate new receiving address.

**Parameters:**
1. `label` (string, optional) - Address label

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"getnewaddress","params":[""],"id":1}'
```

**Response:**
```json
{
  "result": "din1q5u3xjgjn8qdehahyecwdrusg2qprvng46sjkkf"
}
```

---

### getbalance

Get wallet balance.

**Parameters:** None

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"getbalance","params":[],"id":1}'
```

**Response:**
```json
{
  "result": {
    "total": "1300.00000000",
    "confirmed": "1200.00000000",
    "unconfirmed": "100.00000000",
    "immature": "0.00000000"
  }
}
```

---

### listunspent

List unspent transaction outputs (UTXOs).

**Parameters:**
1. `minconf` (integer, optional) - Minimum confirmations (default: 1)
2. `maxconf` (integer, optional) - Maximum confirmations (default: 9999999)

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"listunspent","params":[1, 9999999],"id":1}'
```

---

### sendtoaddress

Send amount to address.

**Parameters:**
1. `address` (string) - Recipient address
2. `amount` (numeric) - Amount in DIN

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"sendtoaddress","params":["din1q...", 10.5],"id":1}'
```

**Response:**
```json
{
  "result": {
    "txid": "abc123...",
    "fee": "0.00001000"
  }
}
```

---

### sendmany

Send to multiple addresses in one transaction.

**Parameters:**
1. `amounts` (object) - Address:amount pairs
2. `minconf` (integer, optional) - Minimum confirmations (default: 1)

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{
    "method":"sendmany",
    "params":[
      {
        "din1q...addr1": 10.5,
        "din1q...addr2": 5.25
      },
      1
    ],
    "id":1
  }'
```

---

### settxfee

Set transaction fee rate.

**Parameters:**
1. `amount` (numeric) - Fee rate in DIN/kB

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"settxfee","params":[0.00001],"id":1}'
```

---

### encryptwallet

Encrypt wallet with passphrase.

**Parameters:**
1. `passphrase` (string) - Encryption password

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"encryptwallet","params":["StrongPassword123!"],"id":1}'
```

---

### walletpassphrase

Unlock wallet for signing.

**Parameters:**
1. `passphrase` (string) - Wallet password
2. `timeout` (integer) - Seconds to keep unlocked

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"walletpassphrase","params":["StrongPassword123!", 60],"id":1}'
```

---

### walletlock

Lock encrypted wallet.

**Parameters:** None

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"walletlock","params":[],"id":1}'
```

---

### backupwallet

Get BIP39 mnemonic for wallet backup.

**Parameters:** None

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"backupwallet","params":[],"id":1}'
```

**Response:**
```json
{
  "result": {
    "mnemonic": "abandon ability able about above absent absorb abstract absurd abuse access accident",
    "warning": "Write down these 12 words and store them safely!"
  }
}
```

---

### dumpwallet

Export wallet to file.

**Parameters:**
1. `filename` (string) - Output filename

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"dumpwallet","params":["wallet_backup.txt"],"id":1}'
```

---

### importwallet

Import wallet from backup file.

**Parameters:**
1. `filename` (string) - Backup filename

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"importwallet","params":["wallet_backup.txt"],"id":1}'
```

---

### dumpprivkey

Export private key for address (HD wallet aware).

**Parameters:**
1. `address` (string) - Address

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"dumpprivkey","params":["din1q..."],"id":1}'
```

---

### importprivkey

Import private key in WIF format.

**Parameters:**
1. `wif` (string) - WIF private key
2. `label` (string, optional) - Address label

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"importprivkey","params":["5Kb8k...wif", "imported"],"id":1}'
```

---

### createrawtransaction

Create raw transaction.

**Parameters:**
1. `inputs` (array) - Transaction inputs
2. `outputs` (object) - Address:amount pairs

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{
    "method":"createrawtransaction",
    "params":[
      [{"txid":"abc...", "vout":0}],
      {"din1q...": 10.5}
    ],
    "id":1
  }'
```

---

### signrawtransactionwithwallet

Sign raw transaction with wallet.

**Parameters:**
1. `hexstring` (string) - Raw transaction hex

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"signrawtransactionwithwallet","params":["0100000001..."],"id":1}'
```

---

### sendrawtransaction

Broadcast signed transaction.

**Parameters:**
1. `hexstring` (string) - Signed transaction hex

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"sendrawtransaction","params":["0100000001..."],"id":1}'
```

---

### gettransaction

Get detailed transaction information.

**Parameters:**
1. `txid` (string) - Transaction ID

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"gettransaction","params":["abc123..."],"id":1}'
```

---

### listtransactions

List wallet transactions.

**Parameters:**
1. `count` (integer, optional) - Number to return (default: 10)
2. `skip` (integer, optional) - Number to skip (default: 0)

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"listtransactions","params":[10, 0],"id":1}'
```

---

### fundrawtransaction

Add inputs to raw transaction.

**Parameters:**
1. `hexstring` (string) - Raw transaction hex

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"fundrawtransaction","params":["0100000001..."],"id":1}'
```

---

## Mining RPCs

### getblocktemplate

Get block template for mining.

**Parameters:**
1. `template_request` (object, optional) - Template options

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"getblocktemplate","params":[{}],"id":1}'
```

**Response:**
```json
{
  "result": {
    "version": 1,
    "previousblockhash": "00000a1b...",
    "transactions": [],
    "coinbasevalue": 10000000000,
    "target": "0000ffff00000000...",
    "mintime": 1729436000,
    "curtime": 1729436182,
    "bits": "21ffffff",
    "height": 297
  }
}
```

---

### submitblock

Submit mined block.

**Parameters:**
1. `hexdata` (string) - Block hex data

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"submitblock","params":["0100000001..."],"id":1}'
```

---

### getmininginfo

Get mining status and statistics.

**Parameters:** None

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"getmininginfo","params":[],"id":1}'
```

**Response:**
```json
{
  "result": {
    "blocks": 296,
    "difficulty": 491733567,
    "networkhashps": 125300,
    "pooledtx": 42,
    "chain": "main"
  }
}
```

---

### getnetworkhashps

Get estimated network hashes per second.

**Parameters:**
1. `nblocks` (integer, optional) - Blocks to average (default: 120)

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"getnetworkhashps","params":[120],"id":1}'
```

---

### prioritisetransaction

Modify transaction priority for mining.

**Parameters:**
1. `txid` (string) - Transaction ID
2. `fee_delta` (numeric) - Fee adjustment in una

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"prioritisetransaction","params":["abc...", 10000],"id":1}'
```

---

## Network RPCs

### getpeerinfo

Get connected peer information.

**Parameters:** None

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"getpeerinfo","params":[],"id":1}'
```

**Response:**
```json
{
  "result": [
    {
      "addr": "172.93.160.131:20999",
      "version": 70001,
      "subver": "/Dinero:0.1.0/",
      "conntime": 1729436000,
      "bytessent": 125800,
      "bytesrecv": 458900
    }
  ]
}
```

---

### getnetworkinfo

Get network status.

**Parameters:** None

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"getnetworkinfo","params":[],"id":1}'
```

---

### getnettotals

Get network traffic statistics.

**Parameters:** None

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"getnettotals","params":[],"id":1}'
```

---

### addnode

Add node to connection list.

**Parameters:**
1. `node` (string) - Node address (ip:port)
2. `command` (string) - "add", "remove", or "onetry"

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"addnode","params":["172.93.160.131:20999", "add"],"id":1}'
```

---

### disconnectnode

Disconnect from node.

**Parameters:**
1. `address` (string) - Node address

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"disconnectnode","params":["172.93.160.131:20999"],"id":1}'
```

---

### getconnectioncount

Get number of connections.

**Parameters:** None

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"getconnectioncount","params":[],"id":1}'
```

---

### ping

Ping all connected nodes.

**Parameters:** None

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"ping","params":[],"id":1}'
```

---

### setban

Add/remove IP from ban list.

**Parameters:**
1. `ip` (string) - IP address
2. `command` (string) - "add" or "remove"

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"setban","params":["192.168.1.100", "add"],"id":1}'
```

---

## Utility RPCs

### validateaddress

Validate DineroCoin address.

**Parameters:**
1. `address` (string) - Address to validate

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"validateaddress","params":["din1q..."],"id":1}'
```

**Response:**
```json
{
  "result": {
    "isvalid": true,
    "address": "din1q5u3xjgjn8qdehahyecwdrusg2qprvng46sjkkf",
    "ismine": true,
    "iswatchonly": false
  }
}
```

---

### estimatesmartfee

Estimate fee for confirmation target.

**Parameters:**
1. `conf_target` (integer) - Confirmation blocks

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"estimatesmartfee","params":[6],"id":1}'
```

---

### signmessage

Sign message with address.

**Parameters:**
1. `address` (string) - Address
2. `message` (string) - Message to sign

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"signmessage","params":["din1q...", "Hello"],"id":1}'
```

---

### verifymessage

Verify signed message.

**Parameters:**
1. `address` (string) - Address
2. `signature` (string) - Signature
3. `message` (string) - Original message

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"verifymessage","params":["din1q...", "sig...", "Hello"],"id":1}'
```

---

### stop

Stop the daemon gracefully.

**Parameters:** None

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"stop","params":[],"id":1}'
```

---

### uptime

Get daemon uptime in seconds.

**Parameters:** None

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"uptime","params":[],"id":1}'
```

---

### help

Get help for RPC methods.

**Parameters:**
1. `command` (string, optional) - Method name

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"help","params":["getblockcount"],"id":1}'
```

---

## Dinero-Specific RPCs

### getsupply

Get current money supply.

**Parameters:** None

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"getsupply","params":[],"id":1}'
```

**Response:**
```json
{
  "result": {
    "total_supply_din": 2960000.0,
    "total_supply_una": 296000000000000,
    "height": 296,
    "algorithm": "Dinero",
    "max_supply_din": 99000000.0
  }
}
```

---

### geteconomics

Get economic parameters.

**Parameters:** None

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"geteconomics","params":[],"id":1}'
```

**Response:**
```json
{
  "result": {
    "premine": 2000000,
    "genesis_burn": 100000,
    "max_supply": 99000000,
    "phase1_blocks": 200000,
    "phase1_reward": 100,
    "phase2_initial_reward": 50,
    "halving_interval": 210000
  }
}
```

---

### getphase

Get current consensus phase.

**Parameters:** None

**Example:**
```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"getphase","params":[],"id":1}'
```

**Response:**
```json
{
  "result": {
    "phase": 1,
    "name": "CPU-Friendly",
    "block_reward": 100,
    "difficulty_target": "0x21ffffff",
    "blocks_until_phase2": 199704
  }
}
```

---

## Error Codes

Common RPC error codes:

| Code | Meaning |
|------|---------|
| -1 | Miscellaneous error |
| -3 | Invalid amount |
| -4 | Out of memory |
| -5 | Invalid address or key |
| -6 | Insufficient funds |
| -8 | Transaction rejected |
| -13 | Wallet unlock needed |
| -14 | Wallet passphrase incorrect |
| -15 | Wallet key pool ran out |
| -17 | Wallet already unlocked |
| -18 | Passphrase too short |

---

## Batch Requests

Send multiple RPC calls in one HTTP request:

```bash
curl -u "$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -H "Content-Type: application/json" \
  -d '[
    {"method":"getblockcount","params":[],"id":1},
    {"method":"getdifficulty","params":[],"id":2},
    {"method":"getbalance","params":[],"id":3}
  ]'
```

---

## Further Reading

- [QUICK_START.md](QUICK_START.md) - Getting started guide
- [FAQ.md](FAQ.md) - Common questions
- [PRODUCTION_STATUS.md](PRODUCTION_STATUS.md) - Security & status

---

**Last Updated:** October 20, 2025  
**Version:** v0.1.0
