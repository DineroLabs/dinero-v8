# DPI QR Code Encoding Specification

## Version 0.1

---

## 1. Overview

This document specifies how DPI PaymentRequests are encoded for transmission via QR codes. The design prioritizes:

1. **Compactness** — Fits in standard QR codes
2. **Scannability** — Works with phone cameras
3. **Extensibility** — Supports future fields
4. **Compatibility** — URI scheme for universal handling

---

## 2. URI Scheme

### 2.1 Format

```
dinero:<address>?<parameters>
```

### 2.2 Components

| Component | Required | Description |
|-----------|----------|-------------|
| `dinero:` | Yes | URI scheme identifier |
| `<address>` | Yes | Receiver's Dinero address |
| `?<params>` | Optional | Query parameters |

### 2.3 Parameters

| Parameter | Required | Type | Description |
|-----------|----------|------|-------------|
| `amount` | Yes* | Decimal | Amount in DIN (e.g., `1.5`) |
| `una` | Yes* | Integer | Amount in una (e.g., `150000000`) |
| `rid` | Yes | UUID | Request ID |
| `exp` | Yes | Unix timestamp | Expiration time |
| `desc` | No | String | Payment description (URL-encoded) |
| `m` | No | String | Arbitrary metadata (URL-encoded JSON) |
| `v` | No | Integer | DPI version (default: 1) |
| `cb` | No | URL | Callback URL for PaymentIntent submission |

*Either `amount` (DIN) or `una` must be provided, not both.

### 2.4 Examples

**Simple payment:**
```
dinero:din1qw508d6qejxtdg4y5r3zarvary0c5xw7kxpjzsx?amount=1.5&rid=550e8400-e29b-41d4-a716-446655440000&exp=1700000300
```

**With description:**
```
dinero:din1qw508d6qejxtdg4y5r3zarvary0c5xw7kxpjzsx?una=150000000&rid=550e8400-e29b-41d4-a716-446655440000&exp=1700000300&desc=Coffee%20purchase
```

**With callback:**
```
dinero:din1qw508d6qejxtdg4y5r3zarvary0c5xw7kxpjzsx?amount=10&rid=550e8400-e29b-41d4-a716-446655440000&exp=1700000300&cb=https://merchant.com/pay/callback
```

---

## 3. Encoding Formats

DPI supports three encoding formats for different use cases:

### 3.1 Format Selection

| Format | Use Case | Size | QR Version |
|--------|----------|------|------------|
| URI | Simple payments | ~150-300 bytes | L/M |
| CBOR | Compact with metadata | ~100-200 bytes | L |
| Binary | Minimal size | ~80-150 bytes | L |

### 3.2 Format Indicator

When encoded in QR, the first byte indicates format:

| Byte | Format |
|------|--------|
| `0x00` | URI (text follows) |
| `0x01` | CBOR |
| `0x02` | Binary (fixed structure) |

---

## 4. CBOR Encoding

### 4.1 Structure

```cbor
{
  1: <version>,           // unsigned int
  2: <address>,           // byte string (decoded address)
  3: <amount_una>,        // unsigned int
  4: <request_id>,        // byte string (16 bytes UUID)
  5: <expires_at>,        // unsigned int (unix timestamp)
  6: <description>,       // text string (optional)
  7: <metadata>,          // map (optional)
  8: <callback_url>       // text string (optional)
}
```

### 4.2 Example

Request for 1 DIN:

```
A5                        // map(5)
  01                      // key: 1 (version)
  01                      // value: 1
  02                      // key: 2 (address)
  58 20                   // bytes(32)
  <32 bytes address>
  03                      // key: 3 (amount_una)
  1A 05F5E100            // value: 100000000
  04                      // key: 4 (request_id)
  50                      // bytes(16)
  <16 bytes UUID>
  05                      // key: 5 (expires_at)
  1A 6557C22C            // value: 1700000300
```

### 4.3 CBOR Field Tags

| Tag | Field | Type |
|-----|-------|------|
| 1 | version | uint |
| 2 | address | bstr |
| 3 | amount_una | uint |
| 4 | request_id | bstr(16) |
| 5 | expires_at | uint |
| 6 | description | tstr |
| 7 | metadata | map |
| 8 | callback_url | tstr |

---

## 5. Binary Encoding

### 5.1 Fixed Structure

For maximum compactness with fixed-size fields:

```
Offset  Size  Field
------  ----  -----
0       1     Format (0x02)
1       1     Version
2       1     Flags
3       32    Address (raw bytes)
35      8     Amount (una, big-endian)
43      16    Request ID (UUID bytes)
59      4     Expires At (unix timestamp, big-endian)
63      var   Optional fields (if flags indicate)
```

### 5.2 Flags Byte

```
Bit 0: Has description
Bit 1: Has callback URL
Bit 2: Has metadata
Bit 3-7: Reserved
```

### 5.3 Optional Fields

If present, optional fields follow in order:

```
[1 byte: description length][N bytes: description UTF-8]
[2 bytes: callback length][N bytes: callback URL UTF-8]
[2 bytes: metadata length][N bytes: metadata CBOR]
```

### 5.4 Example

Minimal payment request (63 bytes):

```
02                          // format: binary
01                          // version: 1
00                          // flags: no optional fields
<32 bytes: address>
00 00 00 00 05 F5 E1 00    // amount: 100000000 una (1 DIN)
<16 bytes: request_id>
65 57 C2 2C                 // expires_at: 1700000300
```

---

## 6. QR Code Generation

### 6.1 Recommended Settings

| Parameter | Value | Rationale |
|-----------|-------|-----------|
| Error Correction | M (15%) | Balance size/reliability |
| Version | Auto (min viable) | Minimize size |
| Encoding | Binary (Mode 4) | For CBOR/Binary formats |
| Encoding | Alphanumeric | For URI format |

### 6.2 Size Estimates

| Format | Typical Size | QR Version | Modules |
|--------|--------------|------------|---------|
| URI (simple) | 120 bytes | 5 | 37x37 |
| URI (full) | 250 bytes | 8 | 49x49 |
| CBOR (simple) | 80 bytes | 3 | 29x29 |
| Binary (min) | 63 bytes | 2 | 25x25 |

### 6.3 Display Guidelines

- Minimum display size: 2cm x 2cm
- Recommended: 3cm x 3cm or larger
- Quiet zone: 4 modules minimum
- Contrast: Black on white preferred

---

## 7. Parsing Algorithm

```pseudocode
function parse_payment_qr(data: bytes) -> Result<PaymentRequest, Error>:
    if data is empty:
        return Err("Empty QR data")

    // Check format indicator
    let format = data[0]

    match format:
        0x00:
            // URI format (skip format byte if present, or detect 'd' for 'dinero:')
            return parse_uri(data)

        0x01:
            // CBOR format
            return parse_cbor(data[1:])

        0x02:
            // Binary format
            return parse_binary(data)

        _:
            // Try URI (no format byte, starts with 'dinero:')
            if data.starts_with(b"dinero:"):
                return parse_uri(data)
            else:
                return Err("Unknown QR format")

function parse_uri(data: bytes) -> Result<PaymentRequest, Error>:
    let uri = decode_utf8(data)?

    // Parse scheme
    if not uri.starts_with("dinero:"):
        return Err("Invalid URI scheme")

    let rest = uri[7:]  // Skip "dinero:"

    // Split address and params
    let (address, params_str) = split_on_first('?', rest)

    // Parse query parameters
    let params = parse_query_string(params_str)

    // Build PaymentRequest
    return Ok(PaymentRequest {
        version: params.get("v")?.parse() ?? 1,
        receiver_address: address,
        amount_una: parse_amount(params)?,
        request_id: params.get("rid")?,
        expires_at: params.get("exp")?.parse()?,
        description: params.get("desc")?.url_decode(),
        callback_url: params.get("cb")?.url_decode(),
        metadata: params.get("m")?.url_decode()?.parse_json()
    })

function parse_amount(params: Map) -> Result<u64, Error>:
    if params.contains("una"):
        return params["una"].parse()
    else if params.contains("amount"):
        let din = params["amount"].parse::<f64>()?
        return (din * 100_000_000) as u64
    else:
        return Err("Missing amount")
```

---

## 8. Validation Rules

### 8.1 URI Validation

| Field | Rule |
|-------|------|
| Scheme | Must be `dinero:` |
| Address | Must be valid Dinero address |
| Amount | Must be positive, max 21M DIN |
| Request ID | Must be valid UUID |
| Expires At | Must be future timestamp |
| Callback | Must be valid HTTPS URL |

### 8.2 Size Limits

| Field | Max Size |
|-------|----------|
| Address | 62 bytes (bech32) |
| Description | 256 characters |
| Callback URL | 512 characters |
| Metadata | 1024 bytes |
| Total URI | 2048 characters |

---

## 9. Callback Protocol

### 9.1 Intent Submission

If `callback_url` is provided, sender wallet submits PaymentIntent via POST:

```http
POST <callback_url>
Content-Type: application/json

{
  "type": "dinero.payment_intent",
  "version": 1,
  "request_id": "550e8400-e29b-41d4-a716-446655440000",
  "transaction": "<hex-encoded-tx>",
  "utreexo_proof": { ... },
  "receiver_output_index": 0,
  "blinding_factors": { ... }
}
```

### 9.2 Response

Success (Tier 1 accepted):
```json
{
  "type": "dinero.payment_ack",
  "status": "accepted_tier_1",
  "request_id": "550e8400-e29b-41d4-a716-446655440000"
}
```

Rejection:
```json
{
  "type": "dinero.payment_reject",
  "error_code": "ERR_CT_001_COMMITMENT_MISMATCH",
  "message": "Amount verification failed",
  "request_id": "550e8400-e29b-41d4-a716-446655440000"
}
```

---

## 10. Security Considerations

### 10.1 QR Code Attacks

| Attack | Mitigation |
|--------|------------|
| QR replacement | User verifies address/amount before confirming |
| Malicious callback | Wallet warns on non-HTTPS callbacks |
| Oversized QR | Parser enforces size limits |
| Malformed data | Strict validation before display |

### 10.2 Callback Security

- MUST use HTTPS
- SHOULD verify TLS certificate
- SHOULD timeout after 30 seconds
- MUST NOT follow redirects to different hosts

---

## 11. Examples

### 11.1 Minimal QR (URI)

```
dinero:din1qxy...xyz?una=50000000&rid=550e8400-e29b-41d4-a716-446655440000&exp=1700000300
```

Decoded:
- Address: din1qxy...xyz
- Amount: 0.5 DIN (50,000,000 una)
- Request ID: 550e8400-e29b-41d4-a716-446655440000
- Expires: 2023-11-14T22:05:00Z

### 11.2 Full QR (URI)

```
dinero:din1qxy...xyz?amount=25.5&rid=550e8400-e29b-41d4-a716-446655440000&exp=1700000300&desc=Monthly%20subscription&cb=https://shop.example.com/api/pay
```

### 11.3 CBOR (hex)

```
A501010258200123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF031A05F5E1000450550E8400E29B41D4A716446655440000051A6557C22C
```

---

## 12. Implementation Checklist

- [ ] URI parser and generator
- [ ] CBOR encoder and decoder
- [ ] Binary encoder and decoder
- [ ] QR code generation library integration
- [ ] QR code scanner integration
- [ ] Callback HTTP client
- [ ] Input validation
- [ ] Size limit enforcement
- [ ] Test vectors for all formats
