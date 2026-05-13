# Lightning Wallet Wire Protocol Specification

## Overview

This document specifies the binary wire protocol used for communication between `lightningd` (Lightning daemon) and `dinerod` (Bitcoin-style full node) in Dinero's Lightning Network implementation.

**Design Philosophy**: Bitcoin Core-style simplicity

- Zero external dependencies (no protobuf, no gRPC in release mode)
- Binary serialization with network byte order
- Variable-length integer encoding (Bitcoin varint)
- Length-prefixed data structures
- Minimal overhead, maximum portability

## Transport Layer

### Connection Types

1. **TCP Socket** (primary)
   - Endpoint format: `127.0.0.1:50051`
   - Default port: 50051 (same as gRPC for compatibility)
   - Binding: localhost only (trusted connection)

2. **Unix Domain Socket** (future)
   - Endpoint format: `/tmp/dinerod.sock`
   - Faster than TCP for local IPC
   - Better security (filesystem permissions)

### Connection Lifecycle

```
Client (lightningd)                    Server (dinerod)
        |                                      |
        |-- TCP Connect (127.0.0.1:50051) --->|
        |<-------- Accept Connection ---------|
        |                                      |
        |-- Request Message ------------------>|
        |                   Process Request    |
        |<-------- Response Message -----------|
        |                                      |
        |-- Request Message ------------------>|
        |                   Process Request    |
        |<-------- Response Message -----------|
        |                                      |
        |-- Close Connection ----------------->|
        |<-------- Connection Closed ----------|
```

## Message Framing

### Wire Format

All messages use the following framing:

```
┌──────────────┬──────────────┬─────────────────┐
│ message_type │ payload_size │     payload     │
│  (4 bytes)   │  (4 bytes)   │   (N bytes)     │
└──────────────┴──────────────┴─────────────────┘
```

**Field Descriptions:**

- `message_type` (uint32_t): Message type identifier (network byte order / big-endian)
- `payload_size` (uint32_t): Size of payload in bytes (network byte order / big-endian)
- `payload` (bytes): Binary-serialized payload (variable length)

**Constraints:**

- Maximum message size: 32 MB (32 * 1024 * 1024 bytes)
- Messages exceeding this limit are rejected

### Byte Order

- All multi-byte integers are in **network byte order** (big-endian)
- Use `htonl()` / `ntohl()` for conversion on little-endian systems

## Message Types

### Request-Response Pairs

| Message Type | Value (Hex) | Description |
|-------------|-------------|-------------|
| `GET_NETWORK_HRP_REQUEST` | 0x0001 | Get network HRP for bech32 addresses |
| `GET_NETWORK_HRP_RESPONSE` | 0x0002 | Network HRP response |
| `LIST_UTXOS_REQUEST` | 0x0003 | List unspent UTXOs |
| `LIST_UTXOS_RESPONSE` | 0x0004 | UTXO list response |
| `DERIVE_LIGHTNING_KEY_REQUEST` | 0x0005 | Derive Lightning key from HD wallet |
| `DERIVE_LIGHTNING_KEY_RESPONSE` | 0x0006 | Lightning key response |
| `COMPUTE_TAPROOT_SIGHASH_REQUEST` | 0x0007 | Compute BIP-341 Taproot sighash |
| `COMPUTE_TAPROOT_SIGHASH_RESPONSE` | 0x0008 | Taproot sighash response |
| `GET_NEW_CHANGE_ADDRESS_REQUEST` | 0x0009 | Get new change address |
| `GET_NEW_CHANGE_ADDRESS_RESPONSE` | 0x000A | Change address response |
| `DERIVE_KEY_FOR_SCRIPTPUBKEY_REQUEST` | 0x000B | Derive key for scriptPubKey |
| `DERIVE_KEY_FOR_SCRIPTPUBKEY_RESPONSE` | 0x000C | Private key response |
| `ERROR_RESPONSE` | 0xFFFF | Error response (any request) |

## Payload Serialization

### Primitive Types

#### Variable-Length Integer (Varint)

Bitcoin-style variable-length encoding:

```
Value Range         | First Byte | Additional Bytes | Total Size
--------------------|------------|------------------|------------
0 - 0xFC            | value      | -                | 1 byte
0xFD - 0xFFFF       | 0xFD       | value (2 bytes)  | 3 bytes
0x10000 - 0xFFFFFFFF| 0xFE       | value (4 bytes)  | 5 bytes
> 0xFFFFFFFF        | 0xFF       | value (8 bytes)  | 9 bytes
```

**Example**:
- `42` → `0x2A` (1 byte)
- `300` → `0xFD 0x2C 0x01` (3 bytes, little-endian value)
- `70000` → `0xFE 0x70 0x11 0x01 0x00` (5 bytes, little-endian value)

#### Fixed-Size Integers

- `uint8_t`: 1 byte (no byte order conversion needed)
- `uint32_t`: 4 bytes (network byte order)
- `uint64_t`: 8 bytes (network byte order)

**Note**: Values within varints are stored in **little-endian**, but the overall message uses **network byte order** for fixed-size fields.

#### Strings

```
┌─────────────┬────────────────┐
│   length    │      data      │
│  (varint)   │  (N bytes)     │
└─────────────┴────────────────┘
```

1. Write length as varint
2. Write UTF-8 encoded string bytes

#### Byte Arrays

```
┌─────────────┬────────────────┐
│   length    │      data      │
│  (varint)   │  (N bytes)     │
└─────────────┴────────────────┘
```

1. Write length as varint
2. Write raw bytes

**Empty arrays/strings**: Encoded as varint `0x00` (zero length)

## Message Payloads

### 1. GetNetworkHRP

**Request** (`0x0001`):
- Empty payload

**Response** (`0x0002`):
```
┌─────────────┐
│     hrp     │
│  (string)   │
└─────────────┘
```

**Fields**:
- `hrp` (string): Network HRP ("din", "tdin", or "rdin")

**Example**:
```
Request:  [type=0x0001][size=0x00000000]
Response: [type=0x0002][size=0x00000004][0x03]"din"
```

### 2. ListUnspentUTXOs

**Request** (`0x0003`):
```
┌──────────────────┬──────────────────┐
│ min_confirmations│ max_confirmations│
│    (uint32_t)    │    (uint32_t)    │
└──────────────────┴──────────────────┘
```

**Response** (`0x0004`):
```
┌─────────────┬────────────┬─────────┬─────┐
│ utxo_count  │   UTXO #1  │ UTXO #2 │ ... │
│  (varint)   │  (struct)  │(struct) │     │
└─────────────┴────────────┴─────────┴─────┘
```

**UTXO Structure**:
```
┌──────────┬──────┬────────┬──────────────┬───────────────┬─────────────┐
│   txid   │ vout │  value │ scriptPubKey │ confirmations │ is_coinbase │
│(32 bytes)│(u32) │ (u64)  │   (bytes)    │    (uint32)   │   (bool)    │
└──────────┴──────┴────────┴──────────────┴───────────────┴─────────────┘
```

**Fields**:
- `txid` (bytes): 32-byte transaction ID
- `vout` (uint32_t): Output index
- `value` (uint64_t): Value in una (smallest unit)
- `scriptPubKey` (bytes): Locking script
- `confirmations` (uint32_t): Number of confirmations
- `is_coinbase` (bool): 0x01 if coinbase, 0x00 otherwise

### 3. DeriveLightningKey

**Request** (`0x0005`):
```
┌──────────┬─────────┬────────┐
│ key_type │ account │  index │
│ (uint32) │ (uint32)│(uint32)│
└──────────┴─────────┴────────┘
```

**Key Types**:
- `0x00`: NODE_IDENTITY
- `0x01`: FUNDING
- `0x02`: REVOCATION_BASE
- `0x03`: PAYMENT_BASE
- `0x04`: DELAYED_PAYMENT_BASE
- `0x05`: HTLC_BASE

**Response** (`0x0006`):
```
┌────────────────┐
│  private_key   │
│   (32 bytes)   │
└────────────────┘
```

### 4. ComputeTaprootSighash

**Request** (`0x0007`):
```
┌────────┬─────────────┬──────────────┬────────────────┬─────────────┬───────┐
│ raw_tx │ input_index │ prevout_count│ prevout_values │ script_count│scripts│
│(bytes) │  (uint32)   │   (varint)   │    (u64[])     │  (varint)   │(bytes)│
└────────┴─────────────┴──────────────┴────────────────┴─────────────┴───────┘
  ┌──────────────┬───────┐
  │ sighash_type │ annex │
  │   (uint8)    │(bytes)│
  └──────────────┴───────┘
```

**Fields**:
- `raw_tx` (bytes): Serialized transaction
- `input_index` (uint32_t): Input index to sign
- `prevout_count` (varint): Number of previous outputs
- `prevout_values` (uint64_t[]): Array of previous output values
- `script_count` (varint): Number of previous output scripts
- `prevout_scripts` (bytes[]): Array of previous output scripts
- `sighash_type` (uint8_t): Sighash type (0x00 = SIGHASH_DEFAULT)
- `annex` (bytes): Optional annex data (empty if not used)

**Response** (`0x0008`):
```
┌──────────┐
│ sighash  │
│(32 bytes)│
└──────────┘
```

### 5. GetNewChangeAddress

**Request** (`0x0009`):
```
┌─────────┐
│  label  │
│(string) │
└─────────┘
```

**Response** (`0x000A`):
```
┌──────────┐
│ address  │
│ (string) │
└──────────┘
```

### 6. DeriveKeyForScriptPubKey

**Request** (`0x000B`):
```
┌───────────────────┐
│ script_pubkey_hex │
│     (string)      │
└───────────────────┘
```

**Response** (`0x000C`):
```
┌──────────────┐
│ private_key  │
│  (32 bytes)  │
└──────────────┘
```

### 7. Error Response

**Response** (`0xFFFF`):
```
┌───────────────┐
│ error_message │
│   (string)    │
└───────────────┘
```

**Usage**: Sent by server when any request fails (invalid message, wallet unavailable, etc.)

## Implementation Notes

### WireSerializer Class

The reference implementation provides a `WireSerializer` class for encoding/decoding:

```cpp
// Writing
WireSerializer serializer;
serializer.writeVarInt(utxos.size());
serializer.writeUint32(confirmations);
serializer.writeString("din");
serializer.writeBytes(private_key);
std::vector<uint8_t> payload = serializer.finalize();

// Reading
WireSerializer deserializer;
deserializer.reset(payload);
uint64_t count = deserializer.readVarInt();
uint32_t conf = deserializer.readUint32();
std::string hrp = deserializer.readString();
std::vector<uint8_t> key = deserializer.readBytes();
```

### Error Handling

**Client-side**:
1. Check response message type
2. If `ERROR_RESPONSE` (0xFFFF), deserialize error message and throw exception
3. Otherwise, deserialize expected response payload

**Server-side**:
1. Catch all exceptions during request processing
2. Serialize error message into payload
3. Send `ERROR_RESPONSE` with error string

### Security Considerations

1. **Localhost Only**: Server binds to `127.0.0.1` (not `0.0.0.0`)
2. **Message Size Limit**: 32 MB maximum prevents memory exhaustion
3. **No Authentication**: Assumes trusted local connection
4. **No Encryption**: Assumes localhost traffic is secure

**Production Deployments**: For remote connections, use:
- SSH tunneling
- VPN
- TLS wrapper (e.g., stunnel)

## Comparison: gRPC vs Socket Wire Protocol

| Aspect | gRPC (Dev Mode) | Socket Wire Protocol (Release) |
|--------|-----------------|--------------------------------|
| **Dependencies** | protobuf, gRPC, abseil (80+ libs) | Zero external dependencies |
| **Binary Size** | +20 MB (dynamic libs) | No overhead (static) |
| **Portability** | Requires Homebrew/apt | Fully portable |
| **Performance** | ~10 μs per RPC call | ~5 μs per message (faster) |
| **Debugging** | grpcurl, Postman | netcat, telnet, Wireshark |
| **Type Safety** | Compile-time (protobuf) | Runtime (manual validation) |
| **Versioning** | Built-in (protobuf) | Manual (message type evolution) |
| **Use Case** | Development (fast iteration) | Production (exchange-ready) |

## Version History

| Version | Date | Changes |
|---------|------|---------|
| 1.0.0 | 2026-01-10 | Initial specification (Phase 5) |

## References

- **Bitcoin Protocol**: https://en.bitcoin.it/wiki/Protocol_documentation
- **Bitcoin Varint**: https://en.bitcoin.it/wiki/Protocol_documentation#Variable_length_integer
- **BIP-341 (Taproot)**: https://github.com/bitcoin/bips/blob/master/bip-0341.mediawiki
- **Dinero Release Builds**: docs/RELEASE_BUILDS.md
- **Implementation**: include/lightning/wallet_wire_protocol.h

---

**Status**: ✅ Phase 5 Complete - Wire protocol implemented and tested

**Last Updated**: January 2026
