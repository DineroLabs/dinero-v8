# DPI Receiver Verifier Implementation

## Version 0.1

---

## 1. Overview

This document provides a complete reference implementation of the DPI receiver verification logic in pseudocode. The verifier implements the V_* pipeline defined in the state machine specification.

---

## 2. Main Entry Point

```pseudocode
/// Main verification entry point
/// Returns: Result<AcceptedTier1, DPIError>
function verify_payment(
    request: PaymentRequest,
    intent: PaymentIntent,
    config: ReceiverConfig,
    state: ReceiverState
) -> Result<AcceptedTier1, DPIError>:

    // Stage 1: Request validation
    validate_request(request, config, state)?

    // Stage 2: Intent validation
    validate_intent(intent, request)?

    // Stage 3: Utreexo proof verification
    verify_utreexo(intent.utreexo_proof, config)?

    // Stage 4: Confidential transaction verification
    verify_ct(intent, request)?

    // Stage 5: Policy checks
    check_policy(intent, request, config)?

    // All checks passed
    return Ok(AcceptedTier1 {
        request_id: request.request_id,
        intent_hash: hash(intent),
        verified_at: now()
    })
```

---

## 3. Stage V_REQ: Request Validation

```pseudocode
/// Validates the PaymentRequest structure and state
function validate_request(
    request: PaymentRequest,
    config: ReceiverConfig,
    state: ReceiverState
) -> Result<(), DPIError>:

    // REQ-001: Check expiration
    let grace_period = config.grace_period_seconds ?? 60
    if now() > request.expires_at + grace_period:
        return Err(DPIError {
            code: "ERR_REQ_001_EXPIRED",
            message: f"PaymentRequest expired at {request.expires_at}",
            details: {
                expires_at: request.expires_at,
                current_time: now(),
                grace_period: grace_period
            }
        })

    // REQ-002: Validate format
    if not is_valid_json(request):
        return Err(DPIError {
            code: "ERR_REQ_002_MALFORMED",
            message: "PaymentRequest is not valid JSON"
        })

    // REQ-002: Required fields
    let required_fields = ["version", "request_id", "receiver_address",
                          "amount_una", "created_at", "expires_at"]
    for field in required_fields:
        if request[field] is None:
            return Err(DPIError {
                code: "ERR_REQ_002_MALFORMED",
                message: f"Missing required field: {field}"
            })

    // REQ-003: Version check
    if request.version > config.max_supported_version:
        return Err(DPIError {
            code: "ERR_REQ_003_UNSUPPORTED_VERSION",
            message: f"Version {request.version} not supported",
            details: {
                received: request.version,
                max_supported: config.max_supported_version
            }
        })

    // REQ-004: Address validation
    if not is_valid_address(request.receiver_address):
        return Err(DPIError {
            code: "ERR_REQ_004_INVALID_ADDRESS",
            message: "Invalid receiver address format"
        })

    // REQ-004: Verify address belongs to us
    if request.receiver_address != config.our_address:
        return Err(DPIError {
            code: "ERR_REQ_004_INVALID_ADDRESS",
            message: "Receiver address does not match our address"
        })

    // REQ-005: Non-zero amount
    if request.amount_una == 0:
        return Err(DPIError {
            code: "ERR_REQ_005_AMOUNT_ZERO",
            message: "Payment amount cannot be zero"
        })

    // REQ-006: Duplicate check
    if state.fulfilled_requests.contains(request.request_id):
        return Err(DPIError {
            code: "ERR_REQ_006_DUPLICATE_REQUEST_ID",
            message: f"Request {request.request_id} already fulfilled",
            details: {
                original_fulfillment: state.fulfilled_requests[request.request_id]
            }
        })

    return Ok(())
```

---

## 4. Stage V_INT: Intent Validation

```pseudocode
/// Validates the PaymentIntent structure and signatures
function validate_intent(
    intent: PaymentIntent,
    request: PaymentRequest
) -> Result<(), DPIError>:

    // INT-001: Transaction present
    if intent.transaction is None or intent.transaction.is_empty():
        return Err(DPIError {
            code: "ERR_INT_001_MISSING_TX",
            message: "PaymentIntent contains no transaction"
        })

    // INT-002: Utreexo proof present
    if intent.utreexo_proof is None:
        return Err(DPIError {
            code: "ERR_INT_002_MISSING_PROOF",
            message: "PaymentIntent missing Utreexo proof"
        })

    // INT-003: Blinding factors for CT
    let tx = parse_transaction(intent.transaction)
    let receiver_idx = intent.receiver_output_index

    if tx.outputs[receiver_idx].is_confidential:
        if intent.blinding_factors is None or
           intent.blinding_factors[receiver_idx] is None:
            return Err(DPIError {
                code: "ERR_INT_003_MISSING_BLINDING",
                message: "Blinding factor required for confidential output"
            })

    // INT-004: Request ID match
    if intent.request_id != request.request_id:
        return Err(DPIError {
            code: "ERR_INT_004_REQUEST_MISMATCH",
            message: "Intent request_id does not match request",
            details: {
                expected: request.request_id,
                received: intent.request_id
            }
        })

    // INT-005: Signature verification
    for (i, input) in enumerate(tx.inputs):
        let pubkey = get_pubkey_for_input(input)
        let sighash = compute_sighash(tx, i)

        if not schnorr_verify(pubkey, sighash, input.signature):
            return Err(DPIError {
                code: "ERR_INT_005_SIGNATURE_INVALID",
                message: f"Invalid signature on input {i}",
                details: {
                    input_index: i
                }
            })

    // INT-006: Output index bounds
    if receiver_idx >= len(tx.outputs):
        return Err(DPIError {
            code: "ERR_INT_006_OUTPUT_INDEX_INVALID",
            message: f"Output index {receiver_idx} out of bounds",
            details: {
                receiver_output_index: receiver_idx,
                num_outputs: len(tx.outputs)
            }
        })

    // INT-007: Output recipient check
    let output = tx.outputs[receiver_idx]
    if extract_address(output) != request.receiver_address:
        return Err(DPIError {
            code: "ERR_INT_007_WRONG_RECIPIENT",
            message: "Output is not addressed to receiver",
            details: {
                expected: request.receiver_address,
                found: extract_address(output)
            }
        })

    // INT-008: RBF check
    let RBF_THRESHOLD = 0xFFFFFFFE
    for input in tx.inputs:
        if input.sequence < RBF_THRESHOLD:
            return Err(DPIError {
                code: "ERR_INT_008_RBF_FORBIDDEN",
                message: "Transaction signals RBF which is forbidden",
                details: {
                    sequence: input.sequence,
                    threshold: RBF_THRESHOLD
                }
            })

    return Ok(())
```

---

## 5. Stage V_UTX: Utreexo Verification

```pseudocode
/// Verifies Utreexo inclusion proofs for all inputs
function verify_utreexo(
    proof: UtreexoProof,
    config: ReceiverConfig
) -> Result<(), DPIError>:

    // UTX-005: Parse proof
    let parsed = try_parse_utreexo_proof(proof)
    if parsed is Err:
        return Err(DPIError {
            code: "ERR_UTX_005_PROOF_MALFORMED",
            message: "Cannot deserialize Utreexo proof",
            details: {
                parse_error: parsed.error
            }
        })

    // UTX-002: Known root
    let known_roots = config.utreexo_roots  // Map<height, root_hash>
    if not known_roots.contains(proof.root_height):
        return Err(DPIError {
            code: "ERR_UTX_002_ROOT_UNKNOWN",
            message: f"Unknown Utreexo root at height {proof.root_height}",
            details: {
                proof_root_height: proof.root_height,
                our_known_heights: known_roots.keys()
            }
        })

    // UTX-003: Root freshness
    let current_height = config.current_block_height
    let tolerance = config.root_height_tolerance ?? 6

    if proof.root_height < (current_height - tolerance):
        return Err(DPIError {
            code: "ERR_UTX_003_ROOT_TOO_STALE",
            message: f"Utreexo root too old",
            details: {
                proof_height: proof.root_height,
                current_height: current_height,
                tolerance: tolerance,
                minimum_acceptable: current_height - tolerance
            }
        })

    // UTX-001: Verify each inclusion proof
    let expected_root = known_roots[proof.root_height]

    for (i, leaf_proof) in enumerate(proof.leaf_proofs):
        let computed_root = compute_merkle_root(
            leaf_proof.leaf_hash,
            leaf_proof.merkle_path
        )

        if computed_root != expected_root:
            return Err(DPIError {
                code: "ERR_UTX_001_PROOF_INVALID",
                message: f"Utreexo proof invalid for input {i}",
                details: {
                    input_index: i,
                    expected_root: hex(expected_root),
                    computed_root: hex(computed_root)
                }
            })

    // UTX-004: Verify UTXOs exist (not already spent)
    for (i, leaf_proof) in enumerate(proof.leaf_proofs):
        let utxo_id = leaf_proof.utxo_id
        if not utxo_exists_in_accumulator(utxo_id, proof.root_height):
            return Err(DPIError {
                code: "ERR_UTX_004_UTXO_NOT_FOUND",
                message: f"UTXO {utxo_id} not found in accumulator",
                details: {
                    input_index: i,
                    utxo_id: utxo_id
                }
            })

    return Ok(())

/// Helper: Compute Merkle root from leaf and path
function compute_merkle_root(
    leaf: Hash,
    path: List<(Hash, Direction)>
) -> Hash:
    let current = leaf

    for (sibling, direction) in path:
        if direction == Left:
            current = hash(sibling || current)
        else:
            current = hash(current || sibling)

    return current
```

---

## 6. Stage V_CT: Confidential Transaction Verification

```pseudocode
/// Verifies confidential transaction commitments and range proofs
function verify_ct(
    intent: PaymentIntent,
    request: PaymentRequest
) -> Result<(), DPIError>:

    let tx = parse_transaction(intent.transaction)
    let receiver_idx = intent.receiver_output_index
    let output = tx.outputs[receiver_idx]

    // Skip if not confidential
    if not output.is_confidential:
        // For non-CT, verify explicit amount
        if output.explicit_amount != request.amount_una:
            return Err(DPIError {
                code: "ERR_CT_001_COMMITMENT_MISMATCH",
                message: "Explicit amount does not match request",
                details: {
                    expected_una: request.amount_una,
                    received_una: output.explicit_amount
                }
            })
        return Ok(())

    // CT verification follows...

    // CT-003: Range proof present
    if output.range_proof is None:
        return Err(DPIError {
            code: "ERR_CT_003_RANGE_PROOF_MISSING",
            message: "Confidential output missing range proof"
        })

    // CT-004: Blinding factor format
    let bf = intent.blinding_factors[receiver_idx]
    let parsed_bf = try_parse_blinding_factor(bf)
    if parsed_bf is Err:
        return Err(DPIError {
            code: "ERR_CT_004_BLINDING_FACTOR_INVALID",
            message: "Cannot parse blinding factor",
            details: {
                parse_error: parsed_bf.error
            }
        })

    // CT-001: Commitment verification
    // Recompute: C = amount*G + blinding_factor*H
    let expected_commitment = pedersen_commit(
        request.amount_una,
        parsed_bf.value
    )

    if output.commitment != expected_commitment:
        return Err(DPIError {
            code: "ERR_CT_001_COMMITMENT_MISMATCH",
            message: "Pedersen commitment does not match amount",
            details: {
                expected_amount_una: request.amount_una,
                commitment_valid: false,
                hint: "Blinding factor may be incorrect"
            }
        })

    // CT-002: Range proof verification
    let range_valid = bulletproof_verify(
        output.commitment,
        output.range_proof
    )

    if not range_valid:
        return Err(DPIError {
            code: "ERR_CT_002_RANGE_PROOF_INVALID",
            message: "Range proof verification failed"
        })

    return Ok(())

/// Helper: Pedersen commitment
function pedersen_commit(amount: u64, blinding_factor: Scalar) -> Point:
    // C = amount * G + blinding_factor * H
    // G, H are generator points
    return (amount * G) + (blinding_factor * H)
```

---

## 7. Stage V_POL: Policy Checks

```pseudocode
/// Applies receiver-specific policy rules
function check_policy(
    intent: PaymentIntent,
    request: PaymentRequest,
    config: ReceiverConfig
) -> Result<(), DPIError>:

    // POL-005: Dust check
    if request.amount_una < config.dust_threshold_una:
        return Err(DPIError {
            code: "ERR_POL_005_AMOUNT_BELOW_DUST",
            message: f"Amount below dust threshold",
            details: {
                amount_una: request.amount_una,
                dust_threshold_una: config.dust_threshold_una
            }
        })

    // POL-001: Tier limits
    if config.tier_2_max_una is not None:
        if request.amount_una > config.tier_2_max_una:
            return Err(DPIError {
                code: "ERR_POL_001_AMOUNT_EXCEEDS_TIER",
                message: "Amount exceeds Tier 2 maximum",
                details: {
                    amount_una: request.amount_una,
                    tier_2_max_una: config.tier_2_max_una,
                    hint: "This payment requires block confirmation (Tier 3)"
                }
            })

    // POL-004: Velocity limits
    let recent_count = count_recent_payments(
        config.velocity_window_seconds
    )
    if recent_count >= config.velocity_max_count:
        return Err(DPIError {
            code: "ERR_POL_004_VELOCITY_LIMIT",
            message: "Too many recent payments",
            details: {
                recent_count: recent_count,
                max_count: config.velocity_max_count,
                window_seconds: config.velocity_window_seconds
            }
        })

    // POL-003: Receiver availability
    if config.suspended:
        return Err(DPIError {
            code: "ERR_POL_003_MERCHANT_SUSPENDED",
            message: "Receiver temporarily not accepting payments"
        })

    return Ok(())
```

---

## 8. Tier 2 Transition

```pseudocode
/// Transitions from Tier 1 to Tier 2 via network observation
function transition_to_tier2(
    tier1: AcceptedTier1,
    intent: PaymentIntent,
    config: ReceiverConfig
) -> Result<AcceptedTier2, DPIError>:

    let tx = parse_transaction(intent.transaction)
    let txid = compute_txid(tx)

    // Broadcast transaction
    let broadcast_result = broadcast_to_network(tx)
    if broadcast_result is Err:
        return Err(DPIError {
            code: "ERR_NET_001_BROADCAST_FAILED",
            message: "Failed to broadcast transaction",
            details: {
                error: broadcast_result.error
            }
        })

    // Observation window
    let window_seconds = config.conflict_window_seconds ?? 5
    let min_peers = config.min_peer_acks ?? 8

    let observation = observe_mempool(
        txid: txid,
        duration: window_seconds,
        min_peers: min_peers
    )

    // Check for conflicts
    if observation.conflict_detected:
        return Err(DPIError {
            code: "ERR_NET_002_CONFLICT_DETECTED",
            message: "Conflicting transaction detected",
            details: {
                our_txid: txid,
                conflict_txid: observation.conflict_txid
            }
        })

    // Check propagation
    if observation.peer_acks < min_peers:
        return Err(DPIError {
            code: "ERR_NET_003_INSUFFICIENT_PROPAGATION",
            message: "Insufficient peer acknowledgments",
            details: {
                required: min_peers,
                received: observation.peer_acks
            }
        })

    return Ok(AcceptedTier2 {
        request_id: tier1.request_id,
        txid: txid,
        peer_acks: observation.peer_acks,
        accepted_at: now()
    })
```

---

## 9. Data Structures

```pseudocode
struct PaymentRequest:
    version: u32
    request_id: UUID
    receiver_address: Address
    amount_una: u64
    currency: String  // "DIN"
    description: Option<String>
    metadata: Option<Map<String, String>>
    created_at: Timestamp
    expires_at: Timestamp

struct PaymentIntent:
    version: u32
    request_id: UUID
    transaction: Bytes  // Serialized transaction
    utreexo_proof: UtreexoProof
    receiver_output_index: u32
    blinding_factors: Option<Map<u32, Bytes>>

struct UtreexoProof:
    root_height: u64
    leaf_proofs: List<LeafProof>

struct LeafProof:
    utxo_id: OutPoint
    leaf_hash: Hash
    merkle_path: List<(Hash, Direction)>

struct ReceiverConfig:
    our_address: Address
    max_supported_version: u32
    grace_period_seconds: u32
    root_height_tolerance: u32
    current_block_height: u64
    utreexo_roots: Map<u64, Hash>
    tier_2_max_una: Option<u64>
    dust_threshold_una: u64
    velocity_window_seconds: u32
    velocity_max_count: u32
    conflict_window_seconds: u32
    min_peer_acks: u32
    suspended: bool

struct ReceiverState:
    fulfilled_requests: Map<UUID, FulfillmentRecord>

struct DPIError:
    code: String       // ERR_XXX_NNN_NAME
    message: String
    details: Option<Map<String, Any>>

struct AcceptedTier1:
    request_id: UUID
    intent_hash: Hash
    verified_at: Timestamp

struct AcceptedTier2:
    request_id: UUID
    txid: TxId
    peer_acks: u32
    accepted_at: Timestamp
```

---

## 10. Error Handling Patterns

```pseudocode
// Pattern: Early return on first error
function verify_pipeline(...) -> Result<T, DPIError>:
    step1()?   // Returns Err if fails, continues if Ok
    step2()?
    step3()?
    return Ok(result)

// Pattern: Logging all stages
function verify_with_logging(...) -> Result<T, DPIError>:
    log.info("Starting V_REQ")
    validate_request(...)?
    log.info("V_REQ passed")

    log.info("Starting V_INT")
    validate_intent(...)?
    log.info("V_INT passed")

    // ... etc

    log.info("All verification stages passed")
    return Ok(result)

// Pattern: Collecting metrics
function verify_with_metrics(...) -> Result<T, DPIError>:
    let start = now()

    let result = verify_payment(...)

    let duration = now() - start
    metrics.record("dpi.verify.duration_ms", duration)

    if result is Ok:
        metrics.increment("dpi.verify.success")
    else:
        metrics.increment("dpi.verify.failure",
            tags: {error_code: result.error.code})

    return result
```

---

## 11. Implementation Checklist

- [ ] Implement all V_* stages
- [ ] Implement cryptographic primitives (Schnorr, Pedersen, Bulletproofs)
- [ ] Implement Utreexo accumulator verification
- [ ] Add persistence for ReceiverState
- [ ] Add network layer for Tier 2 observation
- [ ] Add metrics and logging
- [ ] Write unit tests for each stage
- [ ] Write integration tests for full pipeline
- [ ] Fuzz test with malformed inputs
