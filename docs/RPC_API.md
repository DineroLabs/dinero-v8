# DineroCoin RPC API Reference

**Auto-generated:** 2025-11-03 02:15:44

**Total Methods:** 59

**Discovery Version:** 2.0

**Schema:** din.discovery.v2

---

## Table of Contents

- [Blockchain](#blockchain)
- [Wallet](#wallet)
- [Websocket](#websocket)
- [Discovery](#discovery)
- [Hardware_Wallet](#hardware_wallet)

---

## Blockchain

**Methods in this category:** 4

### getbestblockhash

**Category:** `blockchain`

Returns the hash of the best (tip) block

**Parameters:** None

**Returns:** `string` - Block hash as hex string

**Example:**

```bash
dinero-cli getbestblockhash
```

---

### getblockcount

**Category:** `blockchain`

Returns the number of blocks in the blockchain

**Parameters:** None

**Returns:** `number` - Current block height

**Example:**

```bash
dinero-cli getblockcount
```

---

### getblockheader

**Category:** `blockchain`

Returns block header information by hash

Note: `utreexo_root` is the legacy display-order hex form for compatibility. Use `utreexo_root_raw` when comparing against raw header bytes, `wallet.getproofbundle.accumulator_root`, or `blockchain.getutreexocommitment`.

**Parameters:**

  - **`hash`** (string) *(required)*: Block hash (64-character hex string)

**Returns:** `object` - Block header information

**Example:**

```bash
dinero-cli getblockheader "hash_value"
```

---

### getdifficulty

**Category:** `blockchain`

Returns the current network difficulty

**Parameters:** None

**Returns:** `number` - Current mining difficulty (1.0 minimum)

**Example:**

```bash
dinero-cli getdifficulty
```

---

## Wallet

**Methods in this category:** 40

### backupwallet

**Category:** `wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli backupwallet
```

---

### combinepsbt

**Category:** `wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli combinepsbt
```

---

### createhdwallet

**Category:** `wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli createhdwallet
```

---

### createrawtransaction

**Category:** `wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli createrawtransaction
```

---

### decoderawtransaction

**Category:** `wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli decoderawtransaction
```

---

### deriveaddress

**Category:** `wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli deriveaddress
```

---

### dumpprivkey

**Category:** `wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli dumpprivkey
```

---

### dumpwallet

**Category:** `wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli dumpwallet
```

---

### encryptwallet

**Category:** `wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli encryptwallet
```

---

### exportcsv

**Category:** `wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli exportcsv
```

---

### finalizepsbt

**Category:** `wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli finalizepsbt
```

---

### generateqrcode

**Category:** `wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli generateqrcode
```

---

### getbalance

**Category:** `wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli getbalance
```

---

### getlabel

**Category:** `wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli getlabel
```

---

### getnewaddress

**Category:** `wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli getnewaddress
```

---

### getrawtransaction

**Category:** `wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli getrawtransaction
```

---

### getsyncstate

**Category:** `wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli getsyncstate
```

---

### getwalletinfo

**Category:** `wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli getwalletinfo
```

---

### getwalletstatus

**Category:** `wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli getwalletstatus
```

---

### importprivkey

**Category:** `wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli importprivkey
```

---

### importwallet

**Category:** `wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli importwallet
```

---

### listaddresses

**Category:** `wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli listaddresses
```

---

### listaddresseswithbalances

**Category:** `wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli listaddresseswithbalances
```

---

### listtransactions

**Category:** `wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli listtransactions
```

---

### listunspent

**Category:** `wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli listunspent
```

---

### notarizebackup

**Category:** `wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli notarizebackup
```

---

### restorewallet

**Category:** `wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli restorewallet
```

---

### scanutxos

**Category:** `wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli scanutxos
```

---

### sendrawtransaction

**Category:** `wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli sendrawtransaction
```

---

### sendtoaddress

**Category:** `wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli sendtoaddress
```

---

### setlabel

**Category:** `wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli setlabel
```

---

### settxfee

**Category:** `wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli settxfee
```

---

### signrawtransactionwithwallet

**Category:** `wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli signrawtransactionwithwallet
```

---

### validateaddress

**Category:** `wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli validateaddress
```

---

### walletcreatefundedpsbt

**Category:** `wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli walletcreatefundedpsbt
```

---

### walletlock

**Category:** `wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli walletlock
```

---

### walletpassphrasechange

**Category:** `wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli walletpassphrasechange
```

---

### walletprocesspsbt

**Category:** `wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli walletprocesspsbt
```

---

### walletrescan

**Category:** `wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli walletrescan
```

---

### walletunlock

**Category:** `wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli walletunlock
```

---

## Websocket

**Methods in this category:** 9

### wsGetConnections

**Category:** `websocket`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli wsGetConnections
```

---

### wsGetStatus

**Category:** `websocket`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli wsGetStatus
```

---

### wsGetTopicStats

**Category:** `websocket`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli wsGetTopicStats
```

---

### wsReplay

**Category:** `websocket`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli wsReplay
```

---

### wsSubscribe

**Category:** `websocket`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli wsSubscribe
```

---

### ws_event_types

**Category:** `websocket`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli ws_event_types
```

---

### ws_list_subscriptions

**Category:** `websocket`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli ws_list_subscriptions
```

---

### ws_subscribe

**Category:** `websocket`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli ws_subscribe
```

---

### ws_unsubscribe

**Category:** `websocket`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli ws_unsubscribe
```

---

## Discovery

**Methods in this category:** 2

### rpc.discover

**Category:** `discovery`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli rpc.discover
```

---

### rpc.info

**Category:** `discovery`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli rpc.info
```

---

## Hardware_Wallet

**Methods in this category:** 4

### analyzepsbt

**Category:** `hardware_wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli analyzepsbt
```

---

### enumeratehwdevices

**Category:** `hardware_wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli enumeratehwdevices
```

---

### exportpsbttofile

**Category:** `hardware_wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli exportpsbttofile
```

---

### importpsbtfromfile

**Category:** `hardware_wallet`

*No description available*

**Parameters:** None

**Returns:** *No return information*

**Example:**

```bash
dinero-cli importpsbtfromfile
```

---


---

## Notes

- This documentation is auto-generated from `rpc.discover`
- To regenerate: `python3 scripts/generate_rpc_docs.py`
- For live testing, use: `dinero-cli help <method>` or `dinero-cli <method>`
- Documentation generated from daemon version with 59 RPC methods
