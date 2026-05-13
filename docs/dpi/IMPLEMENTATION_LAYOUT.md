# DPI Implementation Layout

## Version 0.1

---

## 1. Repository Structure

```
DineroCoin/
├── specs/dpi/                      # Protocol specifications
│   ├── DPI_v0.1_SPEC.md           # Main specification
│   ├── DPI_ERROR_CATALOG.md       # Error codes
│   ├── DPI_STATE_MACHINE.md       # State machine (both perspectives)
│   ├── DPI_THREAT_MODEL.md        # Security analysis
│   ├── DPI_RECEIVER_VERIFIER.md   # Receiver pseudocode
│   ├── DPI_SENDER_STATE_MACHINE.md # Sender pseudocode
│   └── DPI_QR_ENCODING.md         # QR/URI encoding
│
├── tests/dpi/                      # Test vectors and test suites
│   └── test_vectors.json          # Canonical test cases
│
├── docs/dpi/                       # Implementation guides
│   └── IMPLEMENTATION_LAYOUT.md   # This file
│
├── src/                            # Source code (future)
│   ├── dpi/                        # Core DPI library
│   │   ├── mod.rs                 # Module root
│   │   ├── types.rs               # Data structures
│   │   ├── errors.rs              # Error types
│   │   ├── request.rs             # PaymentRequest handling
│   │   ├── intent.rs              # PaymentIntent handling
│   │   ├── verifier.rs            # V_* verification pipeline
│   │   ├── qr.rs                  # QR encoding/decoding
│   │   └── state.rs               # State machine
│   │
│   ├── crypto/                     # Cryptographic primitives
│   │   ├── schnorr.rs             # BIP340 signatures
│   │   ├── pedersen.rs            # Pedersen commitments
│   │   ├── bulletproof.rs         # Range proofs
│   │   └── utreexo.rs             # Utreexo accumulator
│   │
│   └── wallet/                     # Wallet integration
│       ├── sender.rs              # Sender state machine
│       └── receiver.rs            # Receiver state machine
│
└── examples/                       # Usage examples
    ├── simple_payment.rs
    └── merchant_pos.rs
```

---

## 2. Module Dependencies

```
                    ┌─────────────┐
                    │   wallet    │
                    │ (sender/    │
                    │  receiver)  │
                    └──────┬──────┘
                           │
              ┌────────────┼────────────┐
              │            │            │
              ▼            ▼            ▼
        ┌─────────┐  ┌─────────┐  ┌─────────┐
        │   dpi   │  │ network │  │ storage │
        │         │  │         │  │         │
        └────┬────┘  └─────────┘  └─────────┘
             │
    ┌────────┼────────┐
    │        │        │
    ▼        ▼        ▼
┌────────┐ ┌────┐ ┌──────────┐
│ crypto │ │ qr │ │ types/   │
│        │ │    │ │ errors   │
└────────┘ └────┘ └──────────┘
```

---

## 3. Implementation Phases

### Phase 1: Core Types and Errors

**Goal:** Establish foundation

**Tasks:**
- [ ] Define `PaymentRequest` struct
- [ ] Define `PaymentIntent` struct
- [ ] Define all `ERR_*` error types
- [ ] Implement serialization (JSON, CBOR)
- [ ] Write type unit tests

**Deliverables:**
- `src/dpi/types.rs`
- `src/dpi/errors.rs`

---

### Phase 2: Cryptographic Primitives

**Goal:** Implement verification building blocks

**Tasks:**
- [ ] Schnorr signature verification (BIP340)
- [ ] Pedersen commitment computation
- [ ] Bulletproof range proof verification
- [ ] Utreexo inclusion proof verification

**Deliverables:**
- `src/crypto/*.rs`

**Dependencies:**
- secp256k1 library
- bulletproofs library (or custom)
- Utreexo reference implementation

---

### Phase 3: Verification Pipeline

**Goal:** Implement V_* stages

**Tasks:**
- [ ] `validate_request()` - V_REQ
- [ ] `validate_intent()` - V_INT
- [ ] `verify_utreexo()` - V_UTX
- [ ] `verify_ct()` - V_CT
- [ ] `check_policy()` - V_POL
- [ ] Pipeline orchestration
- [ ] Test against test vectors

**Deliverables:**
- `src/dpi/verifier.rs`

---

### Phase 4: QR Encoding

**Goal:** Implement all encoding formats

**Tasks:**
- [ ] URI parser/generator
- [ ] CBOR encoder/decoder
- [ ] Binary encoder/decoder
- [ ] QR code generation (via library)
- [ ] QR code scanning (via library)

**Deliverables:**
- `src/dpi/qr.rs`

---

### Phase 5: State Machines

**Goal:** Implement sender and receiver state machines

**Tasks:**
- [ ] Sender state machine
- [ ] Receiver state machine
- [ ] State persistence
- [ ] Event emission
- [ ] Timeout handling

**Deliverables:**
- `src/wallet/sender.rs`
- `src/wallet/receiver.rs`

---

### Phase 6: Network Layer

**Goal:** Implement communication

**Tasks:**
- [ ] HTTP callback client (sender)
- [ ] HTTP callback server (receiver)
- [ ] P2P transport (future)
- [ ] Mempool observation
- [ ] Peer management

**Deliverables:**
- `src/network/*.rs`

---

### Phase 7: Integration

**Goal:** End-to-end payment flow

**Tasks:**
- [ ] Sender wallet integration
- [ ] Receiver wallet integration
- [ ] UI event handling
- [ ] End-to-end tests
- [ ] Performance benchmarks

**Deliverables:**
- `examples/simple_payment.rs`
- Integration test suite

---

## 4. External Dependencies

| Dependency | Purpose | Version |
|------------|---------|---------|
| `secp256k1` | Schnorr signatures | latest |
| `bulletproofs` | Range proofs | latest |
| `cbor` | CBOR encoding | latest |
| `qrcode` | QR generation | latest |
| `zbar` / `quirc` | QR scanning | latest |
| `reqwest` | HTTP client | latest |
| `axum` / `warp` | HTTP server | latest |
| `serde` | Serialization | latest |
| `tokio` | Async runtime | latest |

---

## 5. Testing Strategy

### 5.1 Unit Tests

| Module | Test Focus |
|--------|------------|
| types | Serialization roundtrip |
| errors | Error code mapping |
| verifier | Each V_* stage in isolation |
| qr | Encoding/decoding all formats |
| state | State transitions |

### 5.2 Integration Tests

| Test | Description |
|------|-------------|
| `test_happy_path` | Complete payment INIT→SETTLED |
| `test_rejection_*` | Each error code triggered |
| `test_timeout_*` | Expiry and network timeouts |
| `test_retry` | Retry after rejection |

### 5.3 Test Vectors

All implementations MUST pass the canonical test vectors in `tests/dpi/test_vectors.json`.

```
TV_001: Happy path
TV_002: Expired request
TV_003: Invalid signature
TV_004: Stale Utreexo root
TV_005: CT amount mismatch
TV_006: RBF forbidden
TV_007: Broadcast failure
TV_008: Double-spend detected
TV_009: Low propagation
TV_010: Late conflict
TV_011: Reorg during confirm
TV_012: Duplicate request_id
TV_013: Amount exceeds tier
TV_014: Retry after rejection
```

---

## 6. Security Checklist

### Implementation Security

- [ ] No secret data in logs
- [ ] Constant-time comparison for signatures
- [ ] Secure random number generation
- [ ] Memory zeroization for keys
- [ ] Input validation at all boundaries

### Protocol Security

- [ ] All V_* stages implemented and enforced
- [ ] RBF detection enabled
- [ ] Utreexo root staleness check
- [ ] CT commitment verification
- [ ] Duplicate request_id rejection

### Operational Security

- [ ] State persistence encryption
- [ ] Secure key storage integration
- [ ] TLS for callbacks
- [ ] Rate limiting

---

## 7. Performance Targets

| Operation | Target | Measurement |
|-----------|--------|-------------|
| V_* pipeline | <100ms | Time from intent receipt to Tier 1 |
| QR parse | <10ms | Time to parse QR data |
| Intent build | <500ms | Time to build complete intent |
| Utreexo proof | <50ms | Time to generate inclusion proof |

---

## 8. Conformance Requirements

An implementation is DPI v0.1 conformant if:

1. **MUST** implement all V_* verification stages
2. **MUST** return correct `ERR_*` codes
3. **MUST** pass all test vectors (TV_001-TV_014)
4. **MUST** enforce state machine invariants
5. **MUST** support URI QR encoding (CBOR/binary optional)
6. **SHOULD** implement all three QR encoding formats
7. **MAY** extend metadata fields

---

## 9. Version History

| Version | Date | Changes |
|---------|------|---------|
| 0.1 | 2024-01 | Initial specification |

---

## 10. References

- DPI v0.1 Specification: `specs/dpi/DPI_v0.1_SPEC.md`
- Error Catalog: `specs/dpi/DPI_ERROR_CATALOG.md`
- State Machine: `specs/dpi/DPI_STATE_MACHINE.md`
- Threat Model: `specs/dpi/DPI_THREAT_MODEL.md`
- Test Vectors: `tests/dpi/test_vectors.json`
- Receiver Verifier: `specs/dpi/DPI_RECEIVER_VERIFIER.md`
- Sender State Machine: `specs/dpi/DPI_SENDER_STATE_MACHINE.md`
- QR Encoding: `specs/dpi/DPI_QR_ENCODING.md`
