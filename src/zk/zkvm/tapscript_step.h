// Copyright (c) 2026 Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

/**
 * Tapscript VM Step Circuit
 *
 * Encodes a single Tapscript opcode execution as R1CS constraints.
 * This is the core of the ZK Tapscript proving system.
 *
 * The VM state at each step:
 *   - stack[0..MAX_STACK-1]: Stack contents (each element is a field element)
 *   - stack_size: Current number of stack elements
 *   - pc: Program counter (byte offset into script)
 *   - script_hash: Canonical BIP341 TapLeaf hash of the script being executed
 *   - success: Whether execution has succeeded/failed
 *
 * Each step:
 *   1. Read opcode at script[pc]
 *   2. Dispatch to opcode-specific constraint sub-circuit
 *   3. Update state (stack, pc, success)
 *
 * The opcode dispatch uses a multiplexer: for each supported opcode,
 * compute the "would-be" next state, then select the correct one
 * based on the actual opcode byte.
 *
 * This circuit is used as the StepCircuit for Nova IVC:
 * each fold = one opcode execution.
 *
 * === FIRST PRODUCTION IMPLEMENTATION OF GENERAL TAPSCRIPT ZK ON SECP256K1 ===
 */

#include "zk/zkvm/nova.h"
#include "zk/zkvm/ipa.h"
#include "zk/zkvm/gadgets.h"
#include <vector>

namespace dinero {
namespace zk {
namespace zkvm {

// VM limits (from BIP342)
constexpr size_t ZKVM_MAX_STACK = 64;     // Reduced from 1000 for circuit efficiency
constexpr size_t ZKVM_MAX_SCRIPT = 512;   // Max script bytes provable in ZK
constexpr size_t ZKVM_ELEMENT_BITS = 256; // Each stack element is a 256-bit field element

/**
 * Encoded Tapscript VM state for the ZK circuit.
 *
 * All values are field elements (Scalar). The stack uses a fixed-size
 * array with a size counter — unused slots are zero.
 *
 * This is a "flat" representation suitable for R1CS.
 */
struct VMState {
    std::vector<Scalar> stack;   // Fixed-size stack (ZKVM_MAX_STACK elements)
    Scalar stack_size;            // Current stack depth
    Scalar pc;                    // Program counter
    Scalar script_hash;           // Canonical BIP341 TapLeaf hash (public input, fixed)
    Scalar success;               // 1 = still running/success, 0 = failed
    Scalar if_depth;              // Current IF/ELSE nesting depth (0 = top level)
    Scalar skip_depth;            // 0 = executing, >0 = skipping from this IF depth
    Scalar checksig_pk_x;         // Pubkey x-coord from last OP_CHECKSIG (for deferred verify)

    // Flatten to a single vector for Nova state
    std::vector<Scalar> flatten() const;

    // Unflatten from Nova state vector
    static VMState unflatten(const std::vector<Scalar>& flat);

    // State vector size
    static size_t flat_size() { return ZKVM_MAX_STACK + 7; }
};

/**
 * Tapscript Step Circuit — implements StepCircuit for Nova IVC.
 *
 * Each invocation encodes one opcode execution. The script bytes are
 * provided as a public parameter (committed via hash); the prover
 * knows the full script but the verifier only sees the hash.
 *
 * Supported opcodes (Phase 1 — sufficient for CTV vaults and CSFS delegations):
 *   - OP_0, OP_1, OP_1NEGATE: Push constants
 *   - OP_DUP, OP_DROP: Stack manipulation
 *   - OP_EQUAL, OP_EQUALVERIFY: Equality checks
 *   - OP_VERIFY: Verify top of stack
 *   - OP_CHECKTEMPLATEVERIFY: CTV hash check
 *   - OP_CHECKSIGFROMSTACK: CSFS signature check
 *   - Push data (0x01-0x4b): Direct push
 *
 * Each unsupported opcode causes success=0 (script fails).
 *
 *   - OP_IF, OP_NOTIF, OP_ELSE, OP_ENDIF: Conditional execution (Phase 2)
 *
 *   - OP_CHECKSIG: Deferred Schnorr verification (Phase 2)
 *
 * Phase 2 remaining: OP_TXHASH
 * Phase 3: Full Tapscript (all opcodes including OP_CHECKCONTRACTVERIFY)
 */
class TapscriptStepCircuit : public StepCircuit {
public:
    /**
     * Create a Tapscript step circuit.
     *
     * @param script_bytes  The full Tapscript being executed
     * @param tx_template_hash  The CTV template hash (for OP_CHECKTEMPLATEVERIFY)
     */
    TapscriptStepCircuit(
        const std::vector<uint8_t>& script_bytes,
        const Scalar& tx_template_hash
    );

    size_t state_size() const override { return VMState::flat_size(); }

    std::vector<Variable> synthesize(
        R1CS& cs,
        const std::vector<Variable>& z_in
    ) override;

private:
    std::vector<uint8_t> script_;
    Scalar tx_template_hash_;

    // Opcode sub-circuits: each returns the "would-be" next state
    // if this opcode were the one being executed.

    struct OpcodeResult {
        std::vector<Variable> new_stack;  // ZKVM_MAX_STACK variables
        Variable new_stack_size;
        Variable new_pc;
        Variable new_success;
        Variable new_if_depth;
        Variable new_skip_depth;
        Variable new_checksig_pk_x;
    };

    // Opcode handlers — each adds constraints and returns potential next state
    OpcodeResult handle_op_push_constant(R1CS& cs, const std::vector<Variable>& stack,
                                          Variable stack_size, Variable pc, Variable success,
                                          const Scalar& push_value);

    OpcodeResult handle_op_dup(R1CS& cs, const std::vector<Variable>& stack,
                                Variable stack_size, Variable pc, Variable success);

    OpcodeResult handle_op_drop(R1CS& cs, const std::vector<Variable>& stack,
                                 Variable stack_size, Variable pc, Variable success);

    OpcodeResult handle_op_equal(R1CS& cs, const std::vector<Variable>& stack,
                                  Variable stack_size, Variable pc, Variable success);

    OpcodeResult handle_op_verify(R1CS& cs, const std::vector<Variable>& stack,
                                   Variable stack_size, Variable pc, Variable success);

    OpcodeResult handle_op_ctv(R1CS& cs, const std::vector<Variable>& stack,
                                Variable stack_size, Variable pc, Variable success);

    OpcodeResult handle_op_nop(R1CS& cs, const std::vector<Variable>& stack,
                                Variable stack_size, Variable pc, Variable success);

    OpcodeResult handle_op_checksig(R1CS& cs, const std::vector<Variable>& stack,
                                     Variable stack_size, Variable pc, Variable success,
                                     Variable checksig_pk_x);

    // Combined OP_IF/OP_NOTIF handler — single stack_read, two skip_depth outcomes
    struct IfNotifResults {
        OpcodeResult if_result;
        Variable notif_skip_depth;  // Only skip_depth differs for NOTIF
    };

    IfNotifResults handle_op_if_notif(R1CS& cs, const std::vector<Variable>& stack,
                                       Variable stack_size, Variable pc, Variable success,
                                       Variable if_depth, Variable skip_depth);

    OpcodeResult handle_op_else(R1CS& cs, const std::vector<Variable>& stack,
                                 Variable stack_size, Variable pc, Variable success,
                                 Variable if_depth, Variable skip_depth);

    OpcodeResult handle_op_endif(R1CS& cs, const std::vector<Variable>& stack,
                                  Variable stack_size, Variable pc, Variable success,
                                  Variable if_depth, Variable skip_depth);

    // Stack helpers for circuit context
    Variable stack_read(R1CS& cs, const std::vector<Variable>& stack,
                        Variable index);
    void stack_write(R1CS& cs, std::vector<Variable>& stack,
                     Variable index, Variable value);
};

/**
 * Full Tapscript ZK Proof — Nova Decider Architecture
 *
 * Contains everything needed to verify that a Tapscript executed
 * successfully without revealing the script contents.
 *
 * The proof chain:
 *   1. Nova fold chain: all intermediate steps folded correctly
 *   2. Decider proof: the FOLDED relaxed R1CS instance is satisfiable
 *      - Extended IPA: <l, r> = 0 for r = (W || -E || 0)
 *      - Combined Schnorr: commit_W - commit_E is opened by r
 *   3. Application check: final output state has success=1
 *   4. Schnorr sub-proofs: deferred OP_CHECKSIG verifications
 *
 * No ipa_witness_commit. No last-step witness. The proof is about
 * running_witness_ (the folded data), not a fresh synthesis.
 */

/**
 * Deferred Schnorr signature verification sub-proof.
 *
 * Generated for each OP_CHECKSIG in the script. Contains a BIP-340
 * Schnorr signature verified via libsecp256k1.
 *
 * Binding: pubkey_x must match the VM state's checksig_pk_x field,
 * which is authenticated by the Nova IVC + IPA decider chain.
 *
 * The ZK property of the script execution comes from:
 *   - Ring signature hides the sender
 *   - Nova IVC hides the script + execution path
 *   - Script hash hides which opcodes were used
 * The Schnorr sub-proof reveals the pubkey and signature for
 * OP_CHECKSIG branches. Full ZK for the Schnorr check is available
 * via the 4.34M-constraint BIP-340 R1CS circuit (schnorr_gadget.h)
 * and can be activated when proof generation time is acceptable.
 */
struct SchnorrSubProof {
    // BIP-340 signature: R_x (32 bytes) || s (32 bytes)
    std::vector<uint8_t> signature;   // 64 bytes

    // Public key x-coordinate (for binding check against VM state)
    std::vector<uint8_t> pubkey_x;    // 32 bytes

    // Message hash (sighash from transaction context)
    std::vector<uint8_t> message_hash; // 32 bytes

    // Serialization
    std::vector<uint8_t> serialize() const;
    static bool deserialize(const std::vector<uint8_t>& data, size_t& offset,
                            SchnorrSubProof& out);
};

struct TapscriptZKProof {
    // Nova IVC output
    CommittedInstance final_instance;
    std::vector<FoldingProof> folding_proofs;
    size_t num_steps;

    // Final (unfolded) output state from the last step.
    // After Nova folding, the committed instance's public inputs are linear
    // combinations and cannot be checked directly for application-level
    // correctness (e.g. success==1). This field holds the raw output of the
    // last step so the verifier can check the VM state.
    std::vector<Scalar> final_output_state;

    // Deferred OP_CHECKSIG sub-proofs (one per OP_CHECKSIG in the script)
    std::vector<SchnorrSubProof> schnorr_sub_proofs;

    // --- Serialized proof fields (all verified by the decider) ---

    IPAProof ipa_proof;                     // Inner product argument
    Point ipa_commitment;                   // Pedersen vector commitment for IPA
    Point wire_commitment;                  // Wire value commitment (before challenge)
    std::vector<uint8_t> witness_hash;      // 32 bytes: SHA256(W || gen_hash || nonce)
    std::vector<uint8_t> circuit_hash;      // 32 bytes: SHA256(R1CS structure A,B,C)
    std::vector<uint8_t> gen_hash;          // 32 bytes: SHA256(IPA generators)
    size_t num_r1cs_constraints = 0;
    size_t num_r1cs_variables = 0;

    // Commitment binding: logarithmic Pedersen vector opening proof.
    // Proves <r, H> + r_combined*Q = commit_W - commit_E in O(log N).
    // Replaces the previous O(N) Schnorr response vector.
    Scalar r_combined;                                    // r_W - r_E (32 bytes)
    std::vector<Point> opening_L;      // Left commits per folding round
    std::vector<Point> opening_R;      // Right commits per folding round
    Scalar opening_a_final;            // Final folded scalar
    Scalar opening_blind_final;        // Final blinding

    // --- Prover-only fields (NOT serialized, NOT in proof blob) ---

    std::vector<uint8_t> witness_nonce;     // Random nonce for ZK (prover-only)
    Point decider_commit_W;                 // Convenience copy (verifier derives from Nova)

    // Public inputs
    Scalar script_hash;        // Canonical BIP341 TapLeaf hash of the executed script
    Scalar tx_template_hash;   // CTV template hash (if CTV was used)

    // Serialization
    std::vector<uint8_t> serialize(secp256k1_context* ctx) const;
    static bool deserialize(const std::vector<uint8_t>& data,
                            TapscriptZKProof& out,
                            secp256k1_context* ctx);
};

/**
 * Witness data for an OP_CHECKSIG deferred verification.
 * One per OP_CHECKSIG in the script, provided in execution order.
 */
struct ChecksigWitness {
    std::vector<uint8_t> signature;    // 64 bytes: BIP-340 R_x(32) || s(32)
    std::vector<uint8_t> message_hash; // 32 bytes: sighash
};

/**
 * Generate a ZK proof of Tapscript execution.
 *
 * Executes the script step by step, building Nova IVC proofs,
 * then produces the final IPA proof. For scripts with OP_CHECKSIG,
 * generates Schnorr sub-proofs bound to the VM state.
 *
 * @param script             The Tapscript to prove execution of
 * @param witness_stack      Initial stack (from transaction witness)
 * @param tx_template_hash   CTV template hash for this transaction
 * @param ctx                secp256k1 context
 * @param checksig_witnesses Signature data for OP_CHECKSIG opcodes (optional)
 * @return                   The ZK proof, or empty on failure
 */
TapscriptZKProof prove_tapscript(
    const std::vector<uint8_t>& script,
    const std::vector<std::vector<uint8_t>>& witness_stack,
    const Scalar& tx_template_hash,
    secp256k1_context* ctx,
    const std::vector<ChecksigWitness>& checksig_witnesses = {}
);

/**
 * Verify a ZK proof of Tapscript execution.
 *
 * Checks that the proof is valid: some Tapscript with the committed
 * hash executed successfully on a transaction with the given template hash.
 *
 * @param proof                       The ZK proof
 * @param expected_tx_template_hash   Expected CTV template hash from the
 *                                    transaction context. The proof's
 *                                    tx_template_hash must match this.
 *                                    (P2 Fix #3: prevents existential forgery)
 * @param ctx                         secp256k1 context
 * @return                            true if the proof is valid
 */
bool verify_tapscript(
    const TapscriptZKProof& proof,
    const Scalar& expected_tx_template_hash,
    secp256k1_context* ctx
);

} // namespace zkvm
} // namespace zk
} // namespace dinero
