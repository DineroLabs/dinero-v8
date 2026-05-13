# DPI Sender State Machine

## Version 0.1

---

## 1. Overview

This document specifies the sender-side state machine for DPI payments. While the receiver performs verification, the sender must:

1. Parse and validate PaymentRequests
2. Build valid PaymentIntents
3. Submit intents and handle responses
4. Track payment outcomes

---

## 2. Sender States

```
┌──────────────────────────────────────────────────────────────────┐
│                     SENDER STATE MACHINE                          │
└──────────────────────────────────────────────────────────────────┘

                         ┌──────────┐
                         │   IDLE   │
                         └────┬─────┘
                              │ scan QR / receive request
                              ▼
                         ┌──────────┐
                    ┌───▶│ REVIEWING│◄───┐
                    │    └────┬─────┘    │
                    │         │ user     │ user declines
                    │         │ approves │
                    │         ▼          │
                    │    ┌──────────┐    │
                    │    │ BUILDING │────┴───▶ [CANCELLED]
                    │    └────┬─────┘
                    │         │ intent built
                    │         ▼
                    │    ┌──────────┐
                    │    │  READY   │
                    │    └────┬─────┘
                    │         │ send intent
                    │         ▼
                    │    ┌──────────┐
     retry (new     │    │  SENT    │
     request_id)    │    └────┬─────┘
                    │         │ receive response
                    │         ▼
                    │    ┌──────────────────────────┐
                    │    │                          │
                    │    ▼                          ▼
               ┌──────────┐                  ┌──────────┐
               │ REJECTED │                  │ ACCEPTED │
               └────┬─────┘                  └────┬─────┘
                    │                             │
                    └─────────────────────────────┤
                                                  │ await settlement
                                                  ▼
                                           ┌──────────┐
                                           │ SETTLED  │
                                           └──────────┘
```

---

## 3. State Definitions

| State | Description | Next States |
|-------|-------------|-------------|
| `IDLE` | No active payment | REVIEWING |
| `REVIEWING` | User examining request | BUILDING, CANCELLED |
| `BUILDING` | Constructing intent | READY, CANCELLED |
| `READY` | Intent ready to send | SENT |
| `SENT` | Awaiting receiver response | ACCEPTED, REJECTED |
| `ACCEPTED` | Receiver accepted | SETTLED |
| `REJECTED` | Payment failed | REVIEWING (retry) |
| `SETTLED` | Payment finalized | IDLE |
| `CANCELLED` | User cancelled | IDLE |

---

## 4. State Transitions

### 4.1 IDLE → REVIEWING

**Trigger:** QR scan or request received

```pseudocode
function handle_request_received(qr_data: bytes) -> Result<(), Error>:
    assert state == IDLE

    // Parse QR
    let request = parse_payment_qr(qr_data)?

    // Basic validation
    validate_request_locally(request)?

    // Store and transition
    current_request = request
    state = REVIEWING

    // Notify UI
    emit(ReviewPaymentEvent {
        address: request.receiver_address,
        amount_una: request.amount_una,
        description: request.description,
        expires_at: request.expires_at
    })
```

### 4.2 REVIEWING → BUILDING

**Trigger:** User approves payment

```pseudocode
function handle_user_approval() -> Result<(), Error>:
    assert state == REVIEWING

    // Check expiry
    if now() > current_request.expires_at:
        return Err("Request expired")

    state = BUILDING

    // Start building in background
    spawn(build_intent_async())
```

### 4.3 BUILDING → READY

**Trigger:** Intent construction complete

```pseudocode
async function build_intent_async():
    assert state == BUILDING

    try:
        // Select UTXOs
        let utxos = select_utxos(
            amount: current_request.amount_una,
            fee_rate: get_current_fee_rate()
        )?

        // Build transaction
        let tx = build_transaction(
            inputs: utxos,
            outputs: [
                Output {
                    address: current_request.receiver_address,
                    amount: current_request.amount_una,
                    is_confidential: true
                },
                Output {
                    address: get_change_address(),
                    amount: calculate_change(utxos, current_request.amount_una),
                    is_confidential: true
                }
            ]
        )?

        // Generate Utreexo proofs
        let proofs = generate_utreexo_proofs(utxos)?

        // Generate blinding factors
        let blinding_factors = generate_blinding_factors(tx)?

        // Sign transaction
        let signed_tx = sign_transaction(tx)?

        // Assemble intent
        current_intent = PaymentIntent {
            version: 1,
            request_id: current_request.request_id,
            transaction: serialize(signed_tx),
            utreexo_proof: proofs,
            receiver_output_index: 0,
            blinding_factors: blinding_factors
        }

        state = READY

        emit(IntentReadyEvent {
            request_id: current_request.request_id,
            txid: compute_txid(signed_tx)
        })

    catch error:
        state = REVIEWING  // Allow retry
        emit(BuildFailedEvent { error: error })
```

### 4.4 READY → SENT

**Trigger:** Send intent to receiver

```pseudocode
async function send_intent():
    assert state == READY

    // Determine submission method
    if current_request.callback_url is not None:
        // HTTP callback
        response = await http_post(
            url: current_request.callback_url,
            body: current_intent.to_json(),
            timeout: 30_seconds
        )
    else:
        // Direct P2P (future)
        response = await p2p_send(
            address: current_request.receiver_address,
            intent: current_intent
        )

    state = SENT

    // Handle response
    handle_receiver_response(response)
```

### 4.5 SENT → ACCEPTED / REJECTED

**Trigger:** Receiver response received

```pseudocode
function handle_receiver_response(response: Response):
    assert state == SENT

    match response.type:
        "dinero.payment_ack":
            state = ACCEPTED
            emit(PaymentAcceptedEvent {
                request_id: current_request.request_id,
                tier: response.tier
            })

        "dinero.payment_reject":
            state = REJECTED
            emit(PaymentRejectedEvent {
                request_id: current_request.request_id,
                error_code: response.error_code,
                message: response.message
            })

        _:
            // Timeout or unknown response
            state = REJECTED
            emit(PaymentRejectedEvent {
                error_code: "ERR_SYS_005_NETWORK_UNAVAILABLE",
                message: "No response from receiver"
            })
```

### 4.6 ACCEPTED → SETTLED

**Trigger:** Block confirmation observed

```pseudocode
function handle_confirmation(confirmation: Confirmation):
    assert state == ACCEPTED

    if confirmation.txid == current_intent.txid:
        if confirmation.depth >= required_depth:
            state = SETTLED

            emit(PaymentSettledEvent {
                request_id: current_request.request_id,
                txid: confirmation.txid,
                block_hash: confirmation.block_hash,
                confirmations: confirmation.depth
            })

            // Persist final state
            persist_settled_payment(current_request, current_intent)

            // Cleanup
            current_request = None
            current_intent = None
```

---

## 5. Error Handling

### 5.1 Expiry During Build

```pseudocode
function check_expiry():
    if state in [BUILDING, READY] and now() > current_request.expires_at:
        state = CANCELLED
        emit(PaymentExpiredEvent {
            request_id: current_request.request_id
        })
```

### 5.2 Retry After Rejection

```pseudocode
function handle_retry():
    assert state == REJECTED

    // Clear old intent
    current_intent = None

    // Return to reviewing with same request
    state = REVIEWING

    emit(RetryPaymentEvent {
        original_error: last_error_code
    })
```

### 5.3 Network Timeout

```pseudocode
function handle_send_timeout():
    assert state == SENT

    // We don't know if receiver got it
    // Conservative: assume failed
    state = REJECTED

    emit(PaymentRejectedEvent {
        error_code: "ERR_NET_005_TIMEOUT",
        message: "Receiver did not respond in time"
    })
```

---

## 6. UTXO Selection

### 6.1 Selection Algorithm

```pseudocode
function select_utxos(amount: u64, fee_rate: u64) -> Result<List<UTXO>, Error>:
    // Get available UTXOs
    let available = wallet.get_spendable_utxos()

    // Sort by value (largest first for fewer inputs)
    available.sort_by(|u| -u.amount)

    let selected = []
    let total = 0
    let estimated_fee = 0

    for utxo in available:
        selected.push(utxo)
        total += utxo.amount

        // Estimate fee with current selection
        estimated_fee = estimate_fee(
            num_inputs: len(selected),
            num_outputs: 2,  // receiver + change
            fee_rate: fee_rate
        )

        if total >= amount + estimated_fee:
            return Ok(selected)

    return Err("ERR_SYS_001: Insufficient funds")
```

### 6.2 Fee Estimation

```pseudocode
function estimate_fee(num_inputs: u32, num_outputs: u32, fee_rate: u64) -> u64:
    // Approximate virtual bytes
    let base_size = 10  // Version, locktime, etc.
    let input_size = 68  // Per input (Schnorr sig + witness)
    let output_size = 43  // Per output (CT commitment + range proof ref)

    let vbytes = base_size + (num_inputs * input_size) + (num_outputs * output_size)

    return vbytes * fee_rate
```

---

## 7. Transaction Building

### 7.1 Build Process

```pseudocode
function build_transaction(inputs: List<UTXO>, outputs: List<Output>) -> Transaction:
    let tx = Transaction {
        version: 2,
        lock_time: 0,
        inputs: [],
        outputs: []
    }

    // Add inputs (no RBF signaling)
    for utxo in inputs:
        tx.inputs.push(TxInput {
            prev_txid: utxo.txid,
            prev_vout: utxo.vout,
            sequence: 0xFFFFFFFF,  // Final, no RBF
            witness: []  // Filled during signing
        })

    // Add outputs
    for output in outputs:
        if output.is_confidential:
            let (commitment, blinding) = create_ct_output(output.amount)
            tx.outputs.push(TxOutput {
                commitment: commitment,
                range_proof: generate_range_proof(output.amount, blinding),
                script_pubkey: address_to_script(output.address)
            })
            store_blinding_factor(output.address, blinding)
        else:
            tx.outputs.push(TxOutput {
                amount: output.amount,
                script_pubkey: address_to_script(output.address)
            })

    return tx
```

### 7.2 Signing

```pseudocode
function sign_transaction(tx: Transaction) -> Transaction:
    for (i, input) in enumerate(tx.inputs):
        // Get private key for input
        let privkey = wallet.get_key_for_utxo(input.prev_txid, input.prev_vout)

        // Compute sighash
        let sighash = compute_taproot_sighash(tx, i, SIGHASH_ALL)

        // Schnorr sign
        let signature = schnorr_sign(privkey, sighash)

        // Add to witness
        tx.inputs[i].witness = [signature]

    return tx
```

---

## 8. Utreexo Proof Generation

```pseudocode
function generate_utreexo_proofs(utxos: List<UTXO>) -> UtreexoProof:
    // Get current accumulator state
    let accumulator = wallet.get_utreexo_accumulator()

    let leaf_proofs = []

    for utxo in utxos:
        // Compute leaf hash
        let leaf_hash = hash(utxo.txid || utxo.vout || utxo.commitment)

        // Generate inclusion proof
        let proof = accumulator.prove_inclusion(leaf_hash)

        leaf_proofs.push(LeafProof {
            utxo_id: OutPoint { txid: utxo.txid, vout: utxo.vout },
            leaf_hash: leaf_hash,
            merkle_path: proof.path
        })

    return UtreexoProof {
        root_height: accumulator.height,
        leaf_proofs: leaf_proofs
    }
```

---

## 9. Persistence

### 9.1 State Persistence

```pseudocode
function persist_state():
    let snapshot = StateSnapshot {
        state: state,
        request: current_request,
        intent: current_intent,
        timestamp: now()
    }

    storage.save("dpi_sender_state", snapshot)

function recover_state():
    let snapshot = storage.load("dpi_sender_state")

    if snapshot is None:
        return  // Fresh start

    // Check if still valid
    if snapshot.timestamp < now() - 24_hours:
        storage.delete("dpi_sender_state")
        return  // Too old, discard

    // Recover
    state = snapshot.state
    current_request = snapshot.request
    current_intent = snapshot.intent

    // Handle interrupted states
    match state:
        BUILDING:
            // Restart build
            spawn(build_intent_async())

        SENT:
            // Re-check status (if callback available)
            spawn(check_payment_status())

        ACCEPTED:
            // Resume monitoring
            spawn(monitor_confirmations())
```

### 9.2 Payment History

```pseudocode
function persist_settled_payment(request: PaymentRequest, intent: PaymentIntent):
    let record = PaymentRecord {
        request_id: request.request_id,
        receiver_address: request.receiver_address,
        amount_una: request.amount_una,
        txid: compute_txid(parse_transaction(intent.transaction)),
        settled_at: now()
    }

    storage.append("payment_history", record)
```

---

## 10. UI Events

| Event | Payload | UI Action |
|-------|---------|-----------|
| `ReviewPaymentEvent` | address, amount, description | Show confirmation dialog |
| `IntentReadyEvent` | request_id, txid | Show "Ready to send" |
| `PaymentAcceptedEvent` | request_id, tier | Show "Payment accepted (Tier N)" |
| `PaymentRejectedEvent` | error_code, message | Show error, offer retry |
| `PaymentSettledEvent` | txid, confirmations | Show "Complete" |
| `PaymentExpiredEvent` | request_id | Show "Request expired" |
| `BuildFailedEvent` | error | Show build error |

---

## 11. Implementation Checklist

- [ ] State machine implementation
- [ ] QR scanner integration
- [ ] UTXO selection algorithm
- [ ] Transaction builder
- [ ] CT commitment/proof generation
- [ ] Schnorr signing
- [ ] Utreexo proof generation
- [ ] HTTP callback client
- [ ] State persistence
- [ ] Confirmation monitoring
- [ ] UI event system
- [ ] Unit tests for each state transition
- [ ] Integration test with receiver
