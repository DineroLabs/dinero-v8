// Copyright (c) 2026 Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "zk/zkvm/tapscript_step.h"
#include "zk/zkvm/r1cs_ipa.h"
#include "crypto/sha256.h"
#include "crypto/tagged_hash.h"
#include <openssl/sha.h>
#include <secp256k1_schnorrsig.h>
#include <secp256k1_extrakeys.h>
#include <cassert>
#include <cstring>

namespace dinero {
namespace zk {
namespace zkvm {

namespace {

void WriteCompactSize(std::vector<uint8_t>& out, uint64_t n) {
    if (n < 253) {
        out.push_back(static_cast<uint8_t>(n));
    } else if (n <= 0xFFFF) {
        out.push_back(253);
        out.push_back(n & 0xFF);
        out.push_back((n >> 8) & 0xFF);
    } else if (n <= 0xFFFFFFFF) {
        out.push_back(254);
        for (int i = 0; i < 4; ++i) {
            out.push_back((n >> (8 * i)) & 0xFF);
        }
    } else {
        out.push_back(255);
        for (int i = 0; i < 8; ++i) {
            out.push_back((n >> (8 * i)) & 0xFF);
        }
    }
}

std::array<uint8_t, 32> ComputeCanonicalTapLeafHash(const std::vector<uint8_t>& script,
                                                    uint8_t leaf_version = 0xC0) {
    std::vector<uint8_t> data;
    data.reserve(1 + script.size() + 9);
    data.push_back(leaf_version);
    WriteCompactSize(data, script.size());
    data.insert(data.end(), script.begin(), script.end());
    return dinero::crypto::TaggedHashArray("TapLeaf", data);
}

} // namespace

// ---------------------------------------------------------------------------
// VMState serialization
// ---------------------------------------------------------------------------

std::vector<Scalar> VMState::flatten() const {
    std::vector<Scalar> flat;
    flat.reserve(flat_size());

    // Stack elements
    for (size_t i = 0; i < ZKVM_MAX_STACK; ++i) {
        flat.push_back(i < stack.size() ? stack[i] : Scalar::zero());
    }
    flat.push_back(stack_size);
    flat.push_back(pc);
    flat.push_back(script_hash);
    flat.push_back(success);
    flat.push_back(if_depth);
    flat.push_back(skip_depth);
    flat.push_back(checksig_pk_x);

    return flat;
}

VMState VMState::unflatten(const std::vector<Scalar>& flat) {
    assert(flat.size() == flat_size());
    VMState state;
    state.stack.resize(ZKVM_MAX_STACK);
    for (size_t i = 0; i < ZKVM_MAX_STACK; ++i) {
        state.stack[i] = flat[i];
    }
    state.stack_size = flat[ZKVM_MAX_STACK];
    state.pc = flat[ZKVM_MAX_STACK + 1];
    state.script_hash = flat[ZKVM_MAX_STACK + 2];
    state.success = flat[ZKVM_MAX_STACK + 3];
    state.if_depth = flat[ZKVM_MAX_STACK + 4];
    state.skip_depth = flat[ZKVM_MAX_STACK + 5];
    state.checksig_pk_x = flat[ZKVM_MAX_STACK + 6];
    return state;
}

// ---------------------------------------------------------------------------
// TapscriptStepCircuit
// ---------------------------------------------------------------------------

TapscriptStepCircuit::TapscriptStepCircuit(
    const std::vector<uint8_t>& script_bytes,
    const Scalar& tx_template_hash
)
    : script_(script_bytes)
    , tx_template_hash_(tx_template_hash)
{
}

std::vector<Variable> TapscriptStepCircuit::synthesize(
    R1CS& cs,
    const std::vector<Variable>& z_in
) {
    assert(z_in.size() == VMState::flat_size());

    // Unpack input state variables
    std::vector<Variable> stack(z_in.begin(), z_in.begin() + ZKVM_MAX_STACK);
    Variable stack_size = z_in[ZKVM_MAX_STACK];
    Variable pc = z_in[ZKVM_MAX_STACK + 1];
    Variable script_hash = z_in[ZKVM_MAX_STACK + 2];
    Variable success = z_in[ZKVM_MAX_STACK + 3];
    Variable if_depth = z_in[ZKVM_MAX_STACK + 4];
    Variable skip_depth = z_in[ZKVM_MAX_STACK + 5];
    Variable checksig_pk_x = z_in[ZKVM_MAX_STACK + 6];

    // Get current PC value to determine which opcode to execute
    Scalar pc_val = cs.get_value(pc);
    uint64_t pc_int = 0;
    const uint8_t* pc_bytes = pc_val.data();
    for (int i = 0; i < 8; ++i) {
        pc_int = (pc_int << 8) | pc_bytes[24 + i];
    }

    uint8_t opcode = 0;
    if (pc_int < script_.size()) {
        opcode = script_[pc_int];
    }

    Variable opcode_var = cs.alloc(Scalar(static_cast<uint64_t>(opcode)));

    // --- Compute potential next states for each opcode class ---
    // All handlers run regardless of opcode (fixed-shape constraint structure).
    // The MUX selects the correct result based on the actual opcode.

    // Helper lambda to set passthrough for if_depth, skip_depth, checksig_pk_x
    auto set_passthrough = [&](OpcodeResult& r) {
        r.new_if_depth = if_depth;
        r.new_skip_depth = skip_depth;
        r.new_checksig_pk_x = checksig_pk_x;
    };

    // Default: NOP (advance PC by 1, sets success=0 for unknown opcodes)
    OpcodeResult result_nop = handle_op_nop(cs, stack, stack_size, pc, success);
    set_passthrough(result_nop);

    OpcodeResult result_op0 = handle_op_push_constant(
        cs, stack, stack_size, pc, success, Scalar::zero());
    set_passthrough(result_op0);

    OpcodeResult result_op1 = handle_op_push_constant(
        cs, stack, stack_size, pc, success, Scalar::one());
    set_passthrough(result_op1);

    OpcodeResult result_op1neg = handle_op_push_constant(
        cs, stack, stack_size, pc, success, -Scalar::one());
    set_passthrough(result_op1neg);

    OpcodeResult result_dup = handle_op_dup(cs, stack, stack_size, pc, success);
    set_passthrough(result_dup);

    OpcodeResult result_drop = handle_op_drop(cs, stack, stack_size, pc, success);
    set_passthrough(result_drop);

    OpcodeResult result_equal = handle_op_equal(cs, stack, stack_size, pc, success);
    set_passthrough(result_equal);

    OpcodeResult result_verify = handle_op_verify(cs, stack, stack_size, pc, success);
    set_passthrough(result_verify);

    OpcodeResult result_ctv = handle_op_ctv(cs, stack, stack_size, pc, success);
    set_passthrough(result_ctv);

    // Direct push (0x01-0x4b)
    Scalar push_val = Scalar::zero();
    size_t push_len = 0;
    if (opcode >= 0x01 && opcode <= 0x4b) {
        push_len = opcode;
        if (pc_int + 1 + push_len <= script_.size()) {
            uint8_t padded[32] = {0};
            size_t copy_len = std::min(push_len, size_t(32));
            std::memcpy(padded + 32 - copy_len,
                        script_.data() + pc_int + 1, copy_len);
            push_val = Scalar(padded);
        }
    }
    OpcodeResult result_push = handle_op_push_constant(
        cs, stack, stack_size, pc, success, push_val);
    set_passthrough(result_push);
    // Push PC adjustment (fixed-shape: always add this constraint)
    {
        Variable extra_var = cs.alloc(Scalar(static_cast<uint64_t>(push_len)));
        Scalar new_pc_val = cs.get_value(result_push.new_pc) + Scalar(static_cast<uint64_t>(push_len));
        Variable adjusted_pc = cs.alloc(new_pc_val);
        cs.constrain(
            LinearCombination(adjusted_pc) - LinearCombination(result_push.new_pc) - LinearCombination(extra_var),
            LinearCombination(VAR_ONE),
            LinearCombination::constant(Scalar::zero()),
            "push_pc_adjust"
        );
        result_push.new_pc = adjusted_pc;
    }

    // OP_IF (0x63) + OP_NOTIF (0x64): Combined handler, single stack_read
    auto if_notif = handle_op_if_notif(cs, stack, stack_size, pc, success, if_depth, skip_depth);
    OpcodeResult result_if = if_notif.if_result;
    result_if.new_checksig_pk_x = checksig_pk_x;
    OpcodeResult result_notif = if_notif.if_result;
    result_notif.new_skip_depth = if_notif.notif_skip_depth;
    result_notif.new_checksig_pk_x = checksig_pk_x;

    // OP_ELSE (0x67)
    OpcodeResult result_else = handle_op_else(cs, stack, stack_size, pc, success, if_depth, skip_depth);
    result_else.new_checksig_pk_x = checksig_pk_x;

    // OP_ENDIF (0x68)
    OpcodeResult result_endif = handle_op_endif(cs, stack, stack_size, pc, success, if_depth, skip_depth);
    result_endif.new_checksig_pk_x = checksig_pk_x;

    // OP_CHECKSIG (0xAC): Deferred Schnorr verification
    // Pops pubkey_x from stack, pushes 1 (tentative success).
    // Records pubkey_x in checksig_pk_x for binding with sub-proof.
    OpcodeResult result_checksig = handle_op_checksig(cs, stack, stack_size, pc, success, checksig_pk_x);
    result_checksig.new_if_depth = if_depth;
    result_checksig.new_skip_depth = skip_depth;

    // --- Opcode selector flags ---

    Variable is_op0 = cs.alloc(opcode == 0x00 ? Scalar::one() : Scalar::zero());
    Variable is_op1 = cs.alloc(opcode == 0x51 ? Scalar::one() : Scalar::zero());
    Variable is_op1neg = cs.alloc(opcode == 0x4f ? Scalar::one() : Scalar::zero());
    Variable is_dup = cs.alloc(opcode == 0x76 ? Scalar::one() : Scalar::zero());
    Variable is_drop = cs.alloc(opcode == 0x75 ? Scalar::one() : Scalar::zero());
    Variable is_equal = cs.alloc(opcode == 0x87 ? Scalar::one() : Scalar::zero());
    Variable is_verify = cs.alloc(opcode == 0x69 ? Scalar::one() : Scalar::zero());
    Variable is_ctv = cs.alloc(opcode == 0xb3 ? Scalar::one() : Scalar::zero());
    Variable is_push = cs.alloc((opcode >= 0x01 && opcode <= 0x4b) ? Scalar::one() : Scalar::zero());
    Variable is_if = cs.alloc(opcode == 0x63 ? Scalar::one() : Scalar::zero());
    Variable is_notif = cs.alloc(opcode == 0x64 ? Scalar::one() : Scalar::zero());
    Variable is_else = cs.alloc(opcode == 0x67 ? Scalar::one() : Scalar::zero());
    Variable is_endif = cs.alloc(opcode == 0x68 ? Scalar::one() : Scalar::zero());
    Variable is_checksig = cs.alloc(opcode == 0xAC ? Scalar::one() : Scalar::zero());

    gadgets::enforce_boolean(cs, is_op0, "is_op0");
    gadgets::enforce_boolean(cs, is_op1, "is_op1");
    gadgets::enforce_boolean(cs, is_op1neg, "is_op1neg");
    gadgets::enforce_boolean(cs, is_dup, "is_dup");
    gadgets::enforce_boolean(cs, is_drop, "is_drop");
    gadgets::enforce_boolean(cs, is_equal, "is_equal");
    gadgets::enforce_boolean(cs, is_verify, "is_verify");
    gadgets::enforce_boolean(cs, is_ctv, "is_ctv");
    gadgets::enforce_boolean(cs, is_push, "is_push");
    gadgets::enforce_boolean(cs, is_if, "is_if");
    gadgets::enforce_boolean(cs, is_notif, "is_notif");
    gadgets::enforce_boolean(cs, is_else, "is_else");
    gadgets::enforce_boolean(cs, is_endif, "is_endif");
    gadgets::enforce_boolean(cs, is_checksig, "is_checksig");

    // NOP flag = 1 - sum(known flags). Enforced boolean → at most one known flag is 1.
    Scalar nop_val = Scalar::one();
    bool any_known = false;
    for (auto f : {is_op0, is_op1, is_op1neg, is_dup, is_drop, is_equal, is_verify,
                   is_ctv, is_push, is_if, is_notif, is_else, is_endif, is_checksig}) {
        if (!cs.get_value(f).is_zero()) { nop_val = Scalar::zero(); any_known = true; }
    }
    (void)any_known;
    Variable is_nop = cs.alloc(nop_val);
    gadgets::enforce_boolean(cs, is_nop, "is_nop");
    // Constrain: is_nop + sum(known_flags) = 1
    LinearCombination flag_sum_lc = LinearCombination(is_nop);
    for (auto f : {is_op0, is_op1, is_op1neg, is_dup, is_drop, is_equal, is_verify,
                   is_ctv, is_push, is_if, is_notif, is_else, is_endif, is_checksig}) {
        flag_sum_lc = flag_sum_lc + LinearCombination(f);
    }
    cs.enforce_equal(flag_sum_lc, LinearCombination(Scalar::one(), VAR_ONE), "flag_sum_1");

    // --- MUX: select executing-mode result based on opcode ---

    struct FlaggedResult {
        Variable flag;
        const OpcodeResult* result;
    };
    std::vector<FlaggedResult> opcodes = {
        {is_op0, &result_op0},
        {is_op1, &result_op1},
        {is_op1neg, &result_op1neg},
        {is_dup, &result_dup},
        {is_drop, &result_drop},
        {is_equal, &result_equal},
        {is_verify, &result_verify},
        {is_ctv, &result_ctv},
        {is_push, &result_push},
        {is_if, &result_if},
        {is_notif, &result_notif},
        {is_else, &result_else},
        {is_endif, &result_endif},
        {is_checksig, &result_checksig},
        {is_nop, &result_nop},
    };

    // MUX helper: weighted sum over all opcode results
    auto mux_component = [&](auto getter, const std::string& name) -> Variable {
        Scalar out_val = Scalar::zero();
        for (const auto& op : opcodes) {
            if (!cs.get_value(op.flag).is_zero()) {
                out_val = cs.get_value(getter(*op.result));
            }
        }
        Variable out = cs.alloc(out_val);
        Variable acc = gadgets::constant(cs, Scalar::zero(), name + "_a");
        for (const auto& op : opcodes) {
            Variable term = gadgets::mul(cs, op.flag, getter(*op.result), name + "_m");
            acc = gadgets::add(cs, acc, term, name + "_s");
        }
        gadgets::assert_equal(cs, out, acc, name + "_f");
        return out;
    };

    // MUX for stack slots
    std::vector<Variable> mux_stack(ZKVM_MAX_STACK);
    for (size_t slot = 0; slot < ZKVM_MAX_STACK; ++slot) {
        std::string sn = "ms" + std::to_string(slot);
        mux_stack[slot] = mux_component(
            [slot](const OpcodeResult& r) { return r.new_stack[slot]; }, sn);
    }

    // MUX for scalar state variables
    Variable mux_ss = mux_component([](const OpcodeResult& r) { return r.new_stack_size; }, "mss");
    Variable mux_pc = mux_component([](const OpcodeResult& r) { return r.new_pc; }, "mpc");
    Variable mux_succ = mux_component([](const OpcodeResult& r) { return r.new_success; }, "msu");
    Variable mux_ifd = mux_component([](const OpcodeResult& r) { return r.new_if_depth; }, "mid");
    Variable mux_skd = mux_component([](const OpcodeResult& r) { return r.new_skip_depth; }, "msd");
    Variable mux_cpk = mux_component([](const OpcodeResult& r) { return r.new_checksig_pk_x; }, "mck");

    // --- Skip override ---
    // When skip_depth > 0 (inside a false IF branch), stack/stack_size/success
    // are preserved from input. PC always advances (from MUX). if_depth and
    // skip_depth use skip-mode logic (IF/ELSE/ENDIF modify depth even when skipping).

    Variable is_skipping = gadgets::not_bit(cs,
        gadgets::is_zero(cs, skip_depth, "skip_iz"), "is_skip");

    // Skip-mode if_depth:
    //   IF/NOTIF: if_depth + 1  (track nesting)
    //   ENDIF:    if_depth - 1
    //   Others:   if_depth       (unchanged)
    Variable sk_one = gadgets::constant(cs, Scalar::one(), "sk1");
    Variable sk_ifd_p1 = gadgets::add(cs, if_depth, sk_one, "sk_ifd_p1");
    Variable sk_ifd_m1 = gadgets::sub(cs, if_depth, sk_one, "sk_ifd_m1");
    Variable sk_is_ifn = gadgets::or_bits(cs, is_if, is_notif, "sk_ifn");
    Variable sk_ifd_inner = gadgets::select(cs, is_endif, sk_ifd_m1, if_depth, "sk_ifd_i");
    Variable sk_ifd = gadgets::select(cs, sk_is_ifn, sk_ifd_p1, sk_ifd_inner, "sk_ifd");

    // Skip-mode skip_depth:
    //   ELSE at matching depth:  0 (resume executing)
    //   ENDIF at matching depth: 0 (resume executing)
    //   Others:                  skip_depth (unchanged)
    Variable sk_depth_eq = gadgets::is_equal(cs, if_depth, skip_depth, "sk_deq");
    Variable sk_is_eoe = gadgets::or_bits(cs, is_else, is_endif, "sk_eoe");
    Variable sk_resume = gadgets::and_bits(cs, sk_depth_eq, sk_is_eoe, "sk_res");
    Variable sk_zero = gadgets::constant(cs, Scalar::zero(), "sk_z");
    Variable sk_skd = gadgets::select(cs, sk_resume, sk_zero, skip_depth, "sk_skd");

    // --- Final state assembly ---
    std::vector<Variable> z_out;
    z_out.reserve(VMState::flat_size());

    // Stack: input when skipping, MUX when executing
    for (size_t slot = 0; slot < ZKVM_MAX_STACK; ++slot) {
        z_out.push_back(gadgets::select(cs, is_skipping, stack[slot], mux_stack[slot],
                                          "fo_s" + std::to_string(slot)));
    }

    // stack_size: input when skipping, MUX when executing
    z_out.push_back(gadgets::select(cs, is_skipping, stack_size, mux_ss, "fo_ss"));

    // pc: always from MUX (handles push data length correctly regardless of skip state)
    z_out.push_back(mux_pc);

    // script_hash: unchanged
    z_out.push_back(script_hash);

    // success: when skipping, step_success=1 (skipped opcodes can't fail)
    Variable sk_ok = gadgets::constant(cs, Scalar::one(), "sk_ok");
    Variable step_succ = gadgets::select(cs, is_skipping, sk_ok, mux_succ, "fo_ssu");
    Variable final_succ = gadgets::and_bits(cs, success, step_succ, "fo_succ");
    z_out.push_back(final_succ);

    // if_depth: skip-mode when skipping, MUX when executing
    z_out.push_back(gadgets::select(cs, is_skipping, sk_ifd, mux_ifd, "fo_ifd"));

    // skip_depth: skip-mode when skipping, MUX when executing
    z_out.push_back(gadgets::select(cs, is_skipping, sk_skd, mux_skd, "fo_skd"));

    // checksig_pk_x: from MUX when executing, preserve when skipping
    z_out.push_back(gadgets::select(cs, is_skipping, checksig_pk_x, mux_cpk, "fo_cpk"));

    assert(z_out.size() == VMState::flat_size());
    return z_out;
}

// ---------------------------------------------------------------------------
// Fixed-shape stack helpers
//
// These use MUX/SELECT gadgets to ensure the SAME R1CS structure regardless
// of the runtime stack_size value. This is critical for Nova folding:
// every step must produce the identical constraint wiring.
// ---------------------------------------------------------------------------

Variable TapscriptStepCircuit::stack_read(
    R1CS& cs, const std::vector<Variable>& stack, Variable index)
{
    // MUX: output = stack[index], using fixed indicator-based selection
    return gadgets::mux(cs, index, stack, "stack_read");
}

void TapscriptStepCircuit::stack_write(
    R1CS& cs, std::vector<Variable>& stack, Variable index, Variable value)
{
    // For each slot i: stack[i] = (index == i) ? value : stack[i]
    // Build indicator variables for the write index
    Scalar idx_val = cs.get_value(index);
    std::vector<Variable> indicators(ZKVM_MAX_STACK);
    for (size_t i = 0; i < ZKVM_MAX_STACK; ++i) {
        Scalar slot_idx(static_cast<uint64_t>(i));
        bool is_target = (idx_val == slot_idx);
        indicators[i] = cs.alloc(is_target ? Scalar::one() : Scalar::zero());
        gadgets::enforce_boolean(cs, indicators[i], "sw_ind_" + std::to_string(i));
    }

    // Enforce: sum(indicators) = 1 (exactly one slot is the target)
    LinearCombination ind_sum;
    for (size_t i = 0; i < ZKVM_MAX_STACK; ++i) {
        ind_sum = ind_sum + LinearCombination(indicators[i]);
    }
    cs.enforce_equal(ind_sum, LinearCombination(Scalar::one(), VAR_ONE), "sw_ind_sum");

    // Enforce: index = sum(i * indicators[i])
    LinearCombination sel_sum;
    for (size_t i = 0; i < ZKVM_MAX_STACK; ++i) {
        sel_sum = sel_sum + LinearCombination(Scalar(static_cast<uint64_t>(i)), indicators[i]);
    }
    cs.enforce_equal(LinearCombination(index), sel_sum, "sw_ind_sel");

    // Conditional write: for each slot, select(indicator, value, old_stack[slot])
    for (size_t i = 0; i < ZKVM_MAX_STACK; ++i) {
        stack[i] = gadgets::select(cs, indicators[i], value, stack[i],
                                    "sw_slot_" + std::to_string(i));
    }
}

// ---------------------------------------------------------------------------
// Opcode handlers
// ---------------------------------------------------------------------------

TapscriptStepCircuit::OpcodeResult TapscriptStepCircuit::handle_op_push_constant(
    R1CS& cs, const std::vector<Variable>& stack,
    Variable stack_size, Variable pc, Variable success,
    const Scalar& push_value)
{
    OpcodeResult result;
    result.new_stack.resize(ZKVM_MAX_STACK);

    // Copy existing stack
    for (size_t i = 0; i < ZKVM_MAX_STACK; ++i) {
        result.new_stack[i] = stack[i];
    }

    // FIXED-SHAPE: Allocate push variable and write via fixed selector gadgets
    // Clamp write index to [0, MAX-1] in case stack is full
    Scalar ss_val = cs.get_value(stack_size);
    Variable push_var = cs.alloc(push_value);
    Variable push_is_full_diff = gadgets::sub(cs, stack_size,
        gadgets::constant(cs, Scalar(uint64_t(ZKVM_MAX_STACK)), "push_max"), "push_fdiff");
    Variable push_is_full = gadgets::is_zero(cs, push_is_full_diff, "push_full");
    Variable push_last = gadgets::constant(cs, Scalar(uint64_t(ZKVM_MAX_STACK - 1)), "push_last");
    Variable safe_push_idx = gadgets::select(cs, push_is_full, push_last, stack_size, "push_sidx");
    // Write push_var at safe position (fixed-shape using indicators+select)
    stack_write(cs, result.new_stack, safe_push_idx, push_var);

    // stack_size += 1
    Scalar new_ss_val = ss_val + Scalar::one();
    result.new_stack_size = cs.alloc(new_ss_val);
    cs.constrain(
        LinearCombination(result.new_stack_size),
        LinearCombination(VAR_ONE),
        LinearCombination(stack_size) + LinearCombination(Scalar::one(), VAR_ONE),
        "push_ss"
    );

    // pc += 1
    Scalar pc_val = cs.get_value(pc);
    Scalar new_pc_val = pc_val + Scalar::one();
    result.new_pc = cs.alloc(new_pc_val);
    cs.constrain(
        LinearCombination(result.new_pc),
        LinearCombination(VAR_ONE),
        LinearCombination(pc) + LinearCombination(Scalar::one(), VAR_ONE),
        "push_pc"
    );

    // success unchanged (push can't fail if stack not full)
    result.new_success = success;

    return result;
}

TapscriptStepCircuit::OpcodeResult TapscriptStepCircuit::handle_op_dup(
    R1CS& cs, const std::vector<Variable>& stack,
    Variable stack_size, Variable pc, Variable success)
{
    OpcodeResult result;
    result.new_stack.resize(ZKVM_MAX_STACK);

    for (size_t i = 0; i < ZKVM_MAX_STACK; ++i) {
        result.new_stack[i] = stack[i];
    }

    // FIXED-SHAPE: Read top via MUX, write via selector gadgets
    // Clamp read index: safe_read = is_empty ? 0 : (stack_size - 1)
    // Clamp write index: safe_write = is_empty ? 0 : stack_size
    // These clamps ensure MUX selectors stay in [0, MAX) so indicator
    // constraints are always satisfiable.
    Scalar ss_val = cs.get_value(stack_size);
    Variable is_empty = gadgets::is_zero(cs, stack_size, "dup_empty");

    Variable ss_minus_1 = cs.alloc(ss_val - Scalar::one());
    cs.constrain(
        LinearCombination(ss_minus_1),
        LinearCombination(VAR_ONE),
        LinearCombination(stack_size) - LinearCombination(Scalar::one(), VAR_ONE),
        "dup_ss_m1"
    );
    Variable zero_var = gadgets::constant(cs, Scalar::zero(), "dup_zero");
    Variable safe_read_idx = gadgets::select(cs, is_empty, zero_var, ss_minus_1, "dup_ridx");
    Variable safe_write_idx = gadgets::select(cs, is_empty, zero_var, stack_size, "dup_widx");

    // Read the top element using fixed MUX
    Variable top_val = stack_read(cs, stack, safe_read_idx);

    // Write top_val at position safe_write_idx (fixed-shape)
    stack_write(cs, result.new_stack, safe_write_idx, top_val);

    // stack_size += 1
    result.new_stack_size = cs.alloc(cs.get_value(stack_size) + Scalar::one());
    cs.constrain(
        LinearCombination(result.new_stack_size),
        LinearCombination(VAR_ONE),
        LinearCombination(stack_size) + LinearCombination(Scalar::one(), VAR_ONE),
        "dup_ss"
    );

    // pc += 1
    result.new_pc = cs.alloc(cs.get_value(pc) + Scalar::one());
    cs.constrain(
        LinearCombination(result.new_pc),
        LinearCombination(VAR_ONE),
        LinearCombination(pc) + LinearCombination(Scalar::one(), VAR_ONE),
        "dup_pc"
    );

    result.new_success = success;
    return result;
}

TapscriptStepCircuit::OpcodeResult TapscriptStepCircuit::handle_op_drop(
    R1CS& cs, const std::vector<Variable>& stack,
    Variable stack_size, Variable pc, Variable success)
{
    OpcodeResult result;
    result.new_stack.resize(ZKVM_MAX_STACK);

    // FIXED-SHAPE: Stack contents unchanged (just decrement size)
    // No conditional reads or writes needed — drop doesn't modify stack contents
    for (size_t i = 0; i < ZKVM_MAX_STACK; ++i) {
        result.new_stack[i] = stack[i];
    }

    // stack_size -= 1 (the "dropped" element just becomes inaccessible)
    Scalar ss_val = cs.get_value(stack_size);
    result.new_stack_size = cs.alloc(ss_val - Scalar::one());
    cs.constrain(
        LinearCombination(result.new_stack_size),
        LinearCombination(VAR_ONE),
        LinearCombination(stack_size) - LinearCombination(Scalar::one(), VAR_ONE),
        "drop_ss"
    );

    // pc += 1
    result.new_pc = cs.alloc(cs.get_value(pc) + Scalar::one());
    cs.constrain(
        LinearCombination(result.new_pc),
        LinearCombination(VAR_ONE),
        LinearCombination(pc) + LinearCombination(Scalar::one(), VAR_ONE),
        "drop_pc"
    );

    result.new_success = success;
    return result;
}

TapscriptStepCircuit::OpcodeResult TapscriptStepCircuit::handle_op_equal(
    R1CS& cs, const std::vector<Variable>& stack,
    Variable stack_size, Variable pc, Variable success)
{
    OpcodeResult result;
    result.new_stack.resize(ZKVM_MAX_STACK);

    for (size_t i = 0; i < ZKVM_MAX_STACK; ++i) {
        result.new_stack[i] = stack[i];
    }

    // FIXED-SHAPE: Read two elements via MUX, write result via fixed selector
    // Clamp indices to [0, MAX) to keep MUX satisfiable even on underflow
    Scalar ss_val = cs.get_value(stack_size);

    // Check underflow: need at least 2 elements
    // Compute is_underflow = is_zero(stack_size) OR is_zero(stack_size - 1)
    Variable eq_is_zero_ss = gadgets::is_zero(cs, stack_size, "equal_iz_ss");
    Variable eq_ss_m1 = cs.alloc(ss_val - Scalar::one());
    cs.constrain(
        LinearCombination(eq_ss_m1),
        LinearCombination(VAR_ONE),
        LinearCombination(stack_size) - LinearCombination(Scalar::one(), VAR_ONE),
        "equal_ss_m1"
    );
    Variable eq_is_one = gadgets::is_zero(cs, eq_ss_m1, "equal_iz_ss1");
    Variable eq_underflow = gadgets::or_bits(cs, eq_is_zero_ss, eq_is_one, "equal_uf");

    // Compute raw indices
    Scalar two(uint64_t(2));
    Variable eq_ss_m2 = cs.alloc(ss_val - two);
    cs.constrain(
        LinearCombination(eq_ss_m2),
        LinearCombination(VAR_ONE),
        LinearCombination(stack_size) - LinearCombination(two, VAR_ONE),
        "equal_ss_m2"
    );

    // Safe indices: underflow ? 0 : raw
    Variable eq_zero = gadgets::constant(cs, Scalar::zero(), "equal_zero");
    Variable safe_idx_a = gadgets::select(cs, eq_underflow, eq_zero, eq_ss_m1, "equal_sidxa");
    Variable safe_idx_b = gadgets::select(cs, eq_underflow, eq_zero, eq_ss_m2, "equal_sidxb");

    // Read two elements via fixed MUX
    Variable a = stack_read(cs, stack, safe_idx_a);
    Variable b = stack_read(cs, stack, safe_idx_b);

    // is_eq = (a == b) ? 1 : 0
    Variable eq = gadgets::is_equal(cs, a, b, "op_equal");

    // Write result at position safe_idx_b (= stack_size - 2), fixed-shape
    stack_write(cs, result.new_stack, safe_idx_b, eq);

    // stack_size -= 1 (two popped, one pushed = net -1)
    result.new_stack_size = cs.alloc(ss_val - Scalar::one());
    cs.constrain(
        LinearCombination(result.new_stack_size),
        LinearCombination(VAR_ONE),
        LinearCombination(stack_size) - LinearCombination(Scalar::one(), VAR_ONE),
        "equal_ss"
    );

    result.new_pc = cs.alloc(cs.get_value(pc) + Scalar::one());
    cs.constrain(
        LinearCombination(result.new_pc),
        LinearCombination(VAR_ONE),
        LinearCombination(pc) + LinearCombination(Scalar::one(), VAR_ONE),
        "equal_pc"
    );

    result.new_success = success;
    return result;
}

TapscriptStepCircuit::OpcodeResult TapscriptStepCircuit::handle_op_verify(
    R1CS& cs, const std::vector<Variable>& stack,
    Variable stack_size, Variable pc, Variable success)
{
    OpcodeResult result;
    result.new_stack.resize(ZKVM_MAX_STACK);

    for (size_t i = 0; i < ZKVM_MAX_STACK; ++i) {
        result.new_stack[i] = stack[i];
    }

    Scalar ss_val = cs.get_value(stack_size);

    // FIXED-SHAPE: Read top element via MUX with clamped index
    Variable is_empty = gadgets::is_zero(cs, stack_size, "verify_empty");
    Variable ss_minus_1 = cs.alloc(ss_val - Scalar::one());
    cs.constrain(
        LinearCombination(ss_minus_1),
        LinearCombination(VAR_ONE),
        LinearCombination(stack_size) - LinearCombination(Scalar::one(), VAR_ONE),
        "verify_ss_m1"
    );
    Variable zero_var = gadgets::constant(cs, Scalar::zero(), "verify_zero");
    Variable safe_top_idx = gadgets::select(cs, is_empty, zero_var, ss_minus_1, "verify_ridx");
    Variable top = stack_read(cs, stack, safe_top_idx);

    // is_zero returns 1 if zero; we want the inverse for success
    Variable top_is_zero = gadgets::is_zero(cs, top, "verify_top");
    Variable top_nonzero = gadgets::not_bit(cs, top_is_zero, "verify_nonzero");

    // stack_size -= 1
    result.new_stack_size = cs.alloc(ss_val - Scalar::one());
    cs.constrain(
        LinearCombination(result.new_stack_size),
        LinearCombination(VAR_ONE),
        LinearCombination(stack_size) - LinearCombination(Scalar::one(), VAR_ONE),
        "verify_ss"
    );

    result.new_pc = cs.alloc(cs.get_value(pc) + Scalar::one());
    cs.constrain(
        LinearCombination(result.new_pc),
        LinearCombination(VAR_ONE),
        LinearCombination(pc) + LinearCombination(Scalar::one(), VAR_ONE),
        "verify_pc"
    );

    // success = success AND top_nonzero
    result.new_success = gadgets::and_bits(cs, success, top_nonzero, "verify_success");

    return result;
}

TapscriptStepCircuit::OpcodeResult TapscriptStepCircuit::handle_op_ctv(
    R1CS& cs, const std::vector<Variable>& stack,
    Variable stack_size, Variable pc, Variable success)
{
    OpcodeResult result;
    result.new_stack.resize(ZKVM_MAX_STACK);

    for (size_t i = 0; i < ZKVM_MAX_STACK; ++i) {
        result.new_stack[i] = stack[i];
    }

    Scalar ss_val = cs.get_value(stack_size);

    // CTV: Check that top of stack equals the transaction's template hash
    // FIXED-SHAPE: Read top element via MUX with clamped index
    Variable ctv_is_empty = gadgets::is_zero(cs, stack_size, "ctv_empty");
    Variable ctv_ss_minus_1 = cs.alloc(ss_val - Scalar::one());
    cs.constrain(
        LinearCombination(ctv_ss_minus_1),
        LinearCombination(VAR_ONE),
        LinearCombination(stack_size) - LinearCombination(Scalar::one(), VAR_ONE),
        "ctv_ss_m1"
    );
    Variable ctv_zero = gadgets::constant(cs, Scalar::zero(), "ctv_zero");
    Variable ctv_safe_idx = gadgets::select(cs, ctv_is_empty, ctv_zero, ctv_ss_minus_1, "ctv_ridx");
    Variable ctv_top = stack_read(cs, stack, ctv_safe_idx);

    // Allocate the expected CTV hash as a constrained constant
    Variable expected_hash = gadgets::constant(cs, tx_template_hash_, "ctv_expected");

    // Check equality: top == expected_hash
    Variable hashes_match = gadgets::is_equal(cs, ctv_top, expected_hash, "ctv_match");

    // CTV is NOP-like: it leaves the stack unchanged but fails if hashes don't match
    Variable ctv_success = gadgets::and_bits(cs, success, hashes_match, "ctv_success");

    // CTV doesn't pop the hash (NOP-like behavior per BIP119)
    result.new_stack_size = stack_size;

    result.new_pc = cs.alloc(cs.get_value(pc) + Scalar::one());
    cs.constrain(
        LinearCombination(result.new_pc),
        LinearCombination(VAR_ONE),
        LinearCombination(pc) + LinearCombination(Scalar::one(), VAR_ONE),
        "ctv_pc"
    );

    result.new_success = ctv_success;

    return result;
}

TapscriptStepCircuit::OpcodeResult TapscriptStepCircuit::handle_op_nop(
    R1CS& cs, const std::vector<Variable>& stack,
    Variable stack_size, Variable pc, Variable success)
{
    OpcodeResult result;
    result.new_stack.resize(ZKVM_MAX_STACK);

    for (size_t i = 0; i < ZKVM_MAX_STACK; ++i) {
        result.new_stack[i] = stack[i];
    }

    result.new_stack_size = stack_size;

    // pc += 1
    result.new_pc = cs.alloc(cs.get_value(pc) + Scalar::one());
    cs.constrain(
        LinearCombination(result.new_pc),
        LinearCombination(VAR_ONE),
        LinearCombination(pc) + LinearCombination(Scalar::one(), VAR_ONE),
        "nop_pc"
    );

    // Unknown opcode = failure
    result.new_success = cs.alloc(Scalar::zero());
    gadgets::assert_zero(cs, result.new_success, "nop_fail");

    return result;
}

// ---------------------------------------------------------------------------
// OP_IF / OP_NOTIF (combined handler)
//
// Pops condition from stack. If condition fails the test, sets skip_depth
// to start skipping. IF: skip when condition==0. NOTIF: skip when condition!=0.
// Single stack_read is shared; only skip_depth computation differs.
// ---------------------------------------------------------------------------

TapscriptStepCircuit::IfNotifResults TapscriptStepCircuit::handle_op_if_notif(
    R1CS& cs, const std::vector<Variable>& stack,
    Variable stack_size, Variable pc, Variable success,
    Variable if_depth, Variable skip_depth)
{
    OpcodeResult result;
    result.new_stack.resize(ZKVM_MAX_STACK);

    // Stack contents unchanged (condition is "popped" by decrementing stack_size)
    for (size_t i = 0; i < ZKVM_MAX_STACK; ++i) {
        result.new_stack[i] = stack[i];
    }

    Scalar ss_val = cs.get_value(stack_size);

    // Read condition from top of stack (clamped for empty stack safety)
    Variable if_is_empty = gadgets::is_zero(cs, stack_size, "if_empty");
    Variable if_ss_m1 = cs.alloc(ss_val - Scalar::one());
    cs.constrain(
        LinearCombination(if_ss_m1),
        LinearCombination(VAR_ONE),
        LinearCombination(stack_size) - LinearCombination(Scalar::one(), VAR_ONE),
        "if_ss_m1"
    );
    Variable if_zero = gadgets::constant(cs, Scalar::zero(), "if_zero");
    Variable if_safe_idx = gadgets::select(cs, if_is_empty, if_zero, if_ss_m1, "if_ridx");
    Variable condition = stack_read(cs, stack, if_safe_idx);

    // Condition test
    Variable cond_is_zero = gadgets::is_zero(cs, condition, "if_ciz");
    Variable cond_is_nonzero = gadgets::not_bit(cs, cond_is_zero, "if_cinz");

    // stack_size -= 1 (pop condition)
    result.new_stack_size = cs.alloc(ss_val - Scalar::one());
    cs.constrain(
        LinearCombination(result.new_stack_size),
        LinearCombination(VAR_ONE),
        LinearCombination(stack_size) - LinearCombination(Scalar::one(), VAR_ONE),
        "if_ss"
    );

    // pc += 1
    result.new_pc = cs.alloc(cs.get_value(pc) + Scalar::one());
    cs.constrain(
        LinearCombination(result.new_pc),
        LinearCombination(VAR_ONE),
        LinearCombination(pc) + LinearCombination(Scalar::one(), VAR_ONE),
        "if_pc"
    );

    // success: fail on empty stack
    Variable if_not_empty = gadgets::not_bit(cs, if_is_empty, "if_ne");
    result.new_success = gadgets::and_bits(cs, success, if_not_empty, "if_succ");

    // if_depth += 1 (always, for both IF and NOTIF)
    Variable new_if_depth = gadgets::add(cs, if_depth,
        gadgets::constant(cs, Scalar::one(), "if_one"), "if_ifd");
    result.new_if_depth = new_if_depth;

    // OP_IF skip_depth: if condition==0, start skipping at new_if_depth
    Variable if_skip_zero = gadgets::constant(cs, Scalar::zero(), "if_skz");
    result.new_skip_depth = gadgets::select(cs, cond_is_zero, new_if_depth, if_skip_zero, "if_skd");

    // OP_NOTIF skip_depth: if condition!=0, start skipping at new_if_depth
    Variable notif_skip_depth = gadgets::select(cs, cond_is_nonzero, new_if_depth, if_skip_zero, "nif_skd");

    IfNotifResults out;
    out.if_result = result;
    out.notif_skip_depth = notif_skip_depth;
    return out;
}

// ---------------------------------------------------------------------------
// OP_ELSE (0x67)
//
// When executing (inside true IF branch): start skipping to ENDIF.
// When skipping: handled by skip-mode logic in synthesize(), not here.
// ---------------------------------------------------------------------------

TapscriptStepCircuit::OpcodeResult TapscriptStepCircuit::handle_op_else(
    R1CS& cs, const std::vector<Variable>& stack,
    Variable stack_size, Variable pc, Variable success,
    Variable if_depth, Variable skip_depth)
{
    OpcodeResult result;
    result.new_stack.resize(ZKVM_MAX_STACK);

    // Stack unchanged
    for (size_t i = 0; i < ZKVM_MAX_STACK; ++i) {
        result.new_stack[i] = stack[i];
    }

    result.new_stack_size = stack_size;

    // pc += 1
    result.new_pc = cs.alloc(cs.get_value(pc) + Scalar::one());
    cs.constrain(
        LinearCombination(result.new_pc),
        LinearCombination(VAR_ONE),
        LinearCombination(pc) + LinearCombination(Scalar::one(), VAR_ONE),
        "else_pc"
    );

    result.new_success = success;

    // if_depth: unchanged (ELSE doesn't change nesting depth)
    result.new_if_depth = if_depth;

    // skip_depth: set to if_depth (skip the rest of the true branch to ENDIF)
    result.new_skip_depth = if_depth;

    return result;
}

// ---------------------------------------------------------------------------
// OP_ENDIF (0x68)
//
// When executing: just decrement if_depth, keep executing.
// When skipping: handled by skip-mode logic in synthesize().
// ---------------------------------------------------------------------------

TapscriptStepCircuit::OpcodeResult TapscriptStepCircuit::handle_op_endif(
    R1CS& cs, const std::vector<Variable>& stack,
    Variable stack_size, Variable pc, Variable success,
    Variable if_depth, Variable skip_depth)
{
    OpcodeResult result;
    result.new_stack.resize(ZKVM_MAX_STACK);

    // Stack unchanged
    for (size_t i = 0; i < ZKVM_MAX_STACK; ++i) {
        result.new_stack[i] = stack[i];
    }

    result.new_stack_size = stack_size;

    // pc += 1
    result.new_pc = cs.alloc(cs.get_value(pc) + Scalar::one());
    cs.constrain(
        LinearCombination(result.new_pc),
        LinearCombination(VAR_ONE),
        LinearCombination(pc) + LinearCombination(Scalar::one(), VAR_ONE),
        "endif_pc"
    );

    result.new_success = success;

    // if_depth -= 1
    result.new_if_depth = gadgets::sub(cs, if_depth,
        gadgets::constant(cs, Scalar::one(), "endif_one"), "endif_ifd");

    // skip_depth: stays 0 (we were executing, not skipping)
    result.new_skip_depth = gadgets::constant(cs, Scalar::zero(), "endif_skd");

    return result;
}

// ---------------------------------------------------------------------------
// OP_CHECKSIG (0xAC) — Deferred Schnorr verification
//
// Pops pubkey_x from stack top, pushes 1 (tentative success).
// Records pubkey_x in checksig_pk_x for binding with sub-proof.
// The actual Schnorr verification is done in a separate proof.
// ---------------------------------------------------------------------------

TapscriptStepCircuit::OpcodeResult TapscriptStepCircuit::handle_op_checksig(
    R1CS& cs, const std::vector<Variable>& stack,
    Variable stack_size, Variable pc, Variable success,
    Variable checksig_pk_x)
{
    OpcodeResult result;
    result.new_stack.resize(ZKVM_MAX_STACK);

    for (size_t i = 0; i < ZKVM_MAX_STACK; ++i) {
        result.new_stack[i] = stack[i];
    }

    Scalar ss_val = cs.get_value(stack_size);

    // Read pubkey_x from top of stack (clamped for empty stack safety)
    Variable cs_is_empty = gadgets::is_zero(cs, stack_size, "cs_empty");
    Variable cs_ss_m1 = cs.alloc(ss_val - Scalar::one());
    cs.constrain(
        LinearCombination(cs_ss_m1),
        LinearCombination(VAR_ONE),
        LinearCombination(stack_size) - LinearCombination(Scalar::one(), VAR_ONE),
        "cs_ss_m1"
    );
    Variable cs_zero = gadgets::constant(cs, Scalar::zero(), "cs_zero");
    Variable cs_safe_idx = gadgets::select(cs, cs_is_empty, cs_zero, cs_ss_m1, "cs_ridx");
    Variable pubkey_val = stack_read(cs, stack, cs_safe_idx);

    // Write 1 at top-1 position (replacing the popped pubkey with the result)
    // OP_CHECKSIG: pop pubkey, push result. Net stack change: 0 (replace top).
    // Actually BIP-342: pop sig + pubkey (2 items), push result (1 item). Net: -1.
    // For the deferred ZK version: pop pubkey (1 item), push 1. Net: 0.
    // The signature is NOT on the stack — it's in the auxiliary witness.
    Variable one_val = gadgets::constant(cs, Scalar::one(), "cs_one");
    stack_write(cs, result.new_stack, cs_safe_idx, one_val);

    // stack_size unchanged (pop 1 + push 1 = net 0)
    result.new_stack_size = stack_size;

    // pc += 1
    result.new_pc = cs.alloc(cs.get_value(pc) + Scalar::one());
    cs.constrain(
        LinearCombination(result.new_pc),
        LinearCombination(VAR_ONE),
        LinearCombination(pc) + LinearCombination(Scalar::one(), VAR_ONE),
        "cs_pc"
    );

    // success: fail on empty stack
    Variable cs_not_empty = gadgets::not_bit(cs, cs_is_empty, "cs_ne");
    result.new_success = gadgets::and_bits(cs, success, cs_not_empty, "cs_succ");

    // Record pubkey_x for deferred verification binding
    result.new_checksig_pk_x = pubkey_val;

    return result;
}

// ---------------------------------------------------------------------------
// Top-level prover / verifier
// ---------------------------------------------------------------------------

TapscriptZKProof prove_tapscript(
    const std::vector<uint8_t>& script,
    const std::vector<std::vector<uint8_t>>& witness_stack,
    const Scalar& tx_template_hash,
    secp256k1_context* ctx,
    const std::vector<ChecksigWitness>& checksig_witnesses
) {
    TapscriptZKProof proof;
    proof.num_steps = 0;

    // Commit to the canonical BIP341 TapLeaf hash of the hidden script.
    const auto tapleaf_hash = ComputeCanonicalTapLeafHash(script);
    proof.script_hash = Scalar(tapleaf_hash.data());
    proof.tx_template_hash = tx_template_hash;

    // Initialize VM state
    VMState state;
    state.stack.resize(ZKVM_MAX_STACK, Scalar::zero());
    state.stack_size = Scalar(static_cast<uint64_t>(witness_stack.size()));
    state.pc = Scalar::zero();
    state.script_hash = proof.script_hash;
    state.success = Scalar::one();
    state.if_depth = Scalar::zero();
    state.skip_depth = Scalar::zero();
    state.checksig_pk_x = Scalar::zero();

    // Load witness stack into VM state
    for (size_t i = 0; i < witness_stack.size() && i < ZKVM_MAX_STACK; ++i) {
        // Pack witness element into a field element
        uint8_t padded[32] = {0};
        size_t copy_len = std::min(witness_stack[i].size(), size_t(32));
        std::memcpy(padded + 32 - copy_len, witness_stack[i].data(), copy_len);
        state.stack[i] = Scalar(padded);
    }

    // Create step circuit
    TapscriptStepCircuit step_circuit(script, tx_template_hash);

    // Create Nova prover
    size_t gens_size = 1024; // TODO: compute from circuit size
    NovaProver prover(step_circuit, gens_size, ctx);

    // Execute script step by step
    std::vector<Scalar> z = state.flatten();
    size_t pc_int = 0;

    while (pc_int < script.size()) {
        z = prover.prove_step(z);
        proof.num_steps++;

        // Extract PC from output state
        VMState new_state = VMState::unflatten(z);
        const uint8_t* pc_bytes = new_state.pc.data();
        pc_int = 0;
        for (int i = 0; i < 8; ++i) {
            pc_int = (pc_int << 8) | pc_bytes[24 + i];
        }

        // Check if execution failed
        if (new_state.success.is_zero()) break;
    }

    // Generate Schnorr sub-proofs for any OP_CHECKSIG encountered.
    // Scan the script for OP_CHECKSIG (0xAC) and match with provided witnesses.
    {
        size_t checksig_idx = 0;
        size_t scan_pc = 0;
        while (scan_pc < script.size()) {
            uint8_t op = script[scan_pc];
            if (op == 0xAC && checksig_idx < checksig_witnesses.size()) {
                const auto& cw = checksig_witnesses[checksig_idx];
                if (cw.signature.size() == 64 && cw.message_hash.size() == 32) {
                    // Extract pubkey_x from the final VM state
                    VMState final_vm = VMState::unflatten(z);

                    SchnorrSubProof sub;
                    sub.signature = cw.signature;
                    sub.message_hash = cw.message_hash;
                    // pubkey_x: from the VM state (checksig_pk_x recorded by handler)
                    sub.pubkey_x.assign(final_vm.checksig_pk_x.data(),
                                        final_vm.checksig_pk_x.data() + 32);
                    proof.schnorr_sub_proofs.push_back(sub);
                }
                checksig_idx++;
            }
            // Advance past push data
            if (op >= 0x01 && op <= 0x4b) {
                scan_pc += 1 + op;
            } else {
                scan_pc += 1;
            }
        }
    }

    // Extract Nova proof components
    proof.final_instance = prover.running_instance();
    proof.folding_proofs = prover.folding_proofs();
    proof.final_output_state = z; // Raw (unfolded) output of the last step

    // --- NOVA DECIDER: prove the committed R1CS instance is satisfiable ---
    //
    // The decider proves ∃ W, E such that:
    //   commit_W = <W, H[0..n-1]> + r_W * Q
    //   commit_E = <E, H[n..n+m-1]> + r_E * Q
    //   ∀i: (A_i·z)(B_i·z) - u*(C_i·z) - E_i = 0
    //
    // TWO CASES:
    //
    // Single-step (num_steps == 1): u=1, E=0, commit_E=identity.
    //   Uses the last step's standard witness. This IS a standard R1CS
    //   satisfaction proof.
    //
    // Multi-step (num_steps > 1): u ≠ 1, E ≠ 0.
    //   Uses the FOLDED running witness from Nova's running_witness_.W and
    //   running_witness_.E. This witness satisfies the RELAXED R1CS:
    //     (A·z)(B·z) = u*(C·z) + E
    //   The R1CS STRUCTURE comes from synthesizing the step circuit (same
    //   fixed-shape for all steps), but the WITNESS VALUES are the folded
    //   accumulation. The IPA encoding already handles relaxed R1CS via
    //   the u-scaled C terms and the -E entries in the extended vector.
    //
    // Chain of trust:
    //   1. Nova verifier: fold chain correct -> all steps fold properly
    //   2. Decider IPA: <l, r>=0 for the (relaxed) R1CS
    //   3. Decider Schnorr: commit_W - commit_E opened by r = (W || -E || 0)
    //   4. Fiat-Shamir: IPA bound to Nova instance (prevents decoupling)
    //   5. Nova soundness: folded instance satisfiable -> ALL steps satisfiable
    {
        R1CS decider_cs;
        std::vector<Scalar> decider_W;
        std::vector<Scalar> decider_E;
        Scalar decider_u;
        Scalar decider_r_W;
        Scalar decider_r_E;
        Point decider_commit_W;
        Point decider_commit_E;

        if (proof.num_steps == 1) {
            // --- Single-step decider: standard R1CS (u=1, E=0) ---
            //
            // Synthesize the step circuit using the SAME allocation pattern as
            // Nova's prove_step: alloc_input for z_in, then circuit.synthesize.
            std::vector<Variable> decider_z_in;
            for (const auto& val : prover.last_step_z_in()) {
                decider_z_in.push_back(decider_cs.alloc_input(val));
            }
            step_circuit.synthesize(decider_cs, decider_z_in);

            decider_W = prover.last_step_witness().W;
            decider_u = Scalar::one();
            decider_E.resize(decider_cs.num_constraints(), Scalar::zero());
            decider_r_E = Scalar::zero();
            decider_r_W = prover.last_step_witness().r_W;
            decider_commit_W = prover.running_instance().commit_W;
            decider_commit_E = Point::identity();
        } else {
            // --- Multi-step decider: FOLDED relaxed R1CS (u≠1, E≠0) ---
            //
            // Synthesize the step circuit to get the R1CS STRUCTURE (A, B, C
            // matrices). The constraint wiring is fixed-shape — same for every
            // step and every input. We use any valid z_in for synthesis (the
            // structure doesn't depend on the values).
            //
            // Then REPLACE the synthesized witness with the FOLDED running
            // witness from Nova. The folded witness was accumulated through
            // folding: W' = W_running + r * W_new at each step. It satisfies
            // the relaxed R1CS with the folded u and E.
            std::vector<Variable> decider_z_in;
            for (const auto& val : prover.last_step_z_in()) {
                decider_z_in.push_back(decider_cs.alloc_input(val));
            }
            step_circuit.synthesize(decider_cs, decider_z_in);

            // Replace the synthesized witness with the FOLDED witness.
            // The folded W has the same layout (variable indices) because
            // folding is element-wise: W' = W1 + r*W2. The index positions
            // for z_in (public inputs) and auxiliary variables are preserved.
            decider_cs.set_witness(prover.running_witness().W);

            // Set relaxed R1CS parameters from the folded running instance
            decider_cs.set_relaxed(true);
            decider_cs.set_u(prover.running_instance().u);
            decider_cs.set_error(prover.running_witness().E);

            decider_W = prover.running_witness().W;
            decider_E = prover.running_witness().E;
            decider_u = prover.running_instance().u;
            decider_r_W = prover.running_witness().r_W;
            decider_r_E = prover.running_witness().r_E;

            // For multi-step: use the RUNNING instance commitments directly.
            // These are the folded commitments verified by the Nova fold chain.
            decider_commit_W = prover.running_instance().commit_W;
            decider_commit_E = prover.running_instance().commit_E;
        }

        // R1CS dimensions
        size_t nv = decider_cs.num_variables();
        size_t nc = decider_cs.num_constraints();

        // Generator set sized for the extended IPA vector: next_pow2(n + m)
        size_t n = next_pow2(nv);
        size_t N = next_pow2(n + nc);
        if (N < 4) N = 4;
        const auto& gens = GeneratorSet::cached(N, ctx);

        // Generate the Nova decider proof.
        // Bind final_output_state and the RUNNING Nova instance into the
        // Fiat-Shamir transcript. The running instance binding prevents the
        // decider proof from being decoupled from the fold chain.
        Transcript r1cs_transcript("R1CS_IPA_decider");
        if (proof.num_steps > 1) {
            r1cs_transcript.append_u64("fos_len",
                static_cast<uint64_t>(proof.final_output_state.size()));
            for (const auto& s : proof.final_output_state) {
                r1cs_transcript.append_scalar("fos_elem", s);
            }
        }

        R1CSIPAProof r1cs_proof = r1cs_ipa_prove(
            decider_cs,
            decider_W, decider_E, decider_u,
            decider_commit_W, decider_commit_E,
            decider_r_W, decider_r_E,
            nv, gens, r1cs_transcript, ctx);

        // Store the decider proof.
        proof.decider_commit_W = decider_commit_W;
        proof.ipa_proof = r1cs_proof.ipa;
        proof.ipa_commitment = r1cs_proof.commitment;
        proof.wire_commitment = r1cs_proof.wire_commitment;
        proof.witness_hash = r1cs_proof.witness_hash;
        proof.witness_nonce = r1cs_proof.witness_nonce;
        proof.circuit_hash = r1cs_proof.circuit_hash;
        proof.gen_hash = r1cs_proof.gen_hash;
        proof.r_combined = r1cs_proof.r_combined;
        proof.opening_L = r1cs_proof.opening_proof.L;
        proof.opening_R = r1cs_proof.opening_proof.R;
        proof.opening_a_final = r1cs_proof.opening_proof.a_final;
        proof.opening_blind_final = r1cs_proof.opening_proof.blind_final;
        proof.num_r1cs_constraints = nc;
        proof.num_r1cs_variables = nv;
    }

    return proof;
}

bool verify_tapscript(
    const TapscriptZKProof& proof,
    const Scalar& expected_tx_template_hash,
    secp256k1_context* ctx
) {
    // P2 Fix #3: Verify the CTV template hash matches the transaction context.
    // Without this check, the proof is existential over the template hash --
    // an attacker could generate a valid proof for an arbitrary template and
    // the verifier would accept it regardless of the actual transaction.
    if (proof.tx_template_hash != expected_tx_template_hash) {
        return false;  // Proof for wrong transaction
    }

    // Step 1: Verify Nova IVC (folding was done correctly)
    std::vector<Scalar> initial_state;
    // Reconstruct initial state: empty stack, pc=0, success=1
    VMState init;
    init.stack.resize(ZKVM_MAX_STACK, Scalar::zero());
    init.stack_size = Scalar::zero();
    init.pc = Scalar::zero();
    init.script_hash = proof.script_hash;
    init.success = Scalar::one();
    init.if_depth = Scalar::zero();
    init.skip_depth = Scalar::zero();
    init.checksig_pk_x = Scalar::zero();
    initial_state = init.flatten();

    if (!NovaVerifier::verify(initial_state, proof.final_instance,
                               proof.folding_proofs, proof.num_steps, ctx)) {
        return false;
    }

    // Step 2: Verify Nova decider proof (cryptographic soundness)
    //
    // The decider proves that a committed R1CS instance is satisfiable,
    // using the extended-vector IPA encoding with r = (W || -E || 0)
    // and a combined Schnorr proof opening commit_W - commit_E.
    //
    // TWO CASES:
    //
    // Single-step (num_steps == 1): standard R1CS (u=1, E=0).
    //   commit_W from final_instance (verified by Nova's u==1 check).
    //   commit_E = identity.
    //
    // Multi-step (num_steps > 1): FOLDED relaxed R1CS (u≠1, E≠0).
    //   Uses the RUNNING instance's commit_W, commit_E, and u — all
    //   verified by the Nova fold chain replay in Step 1 above.
    //   The decider proves the ACTUAL folded instance is satisfiable.
    //
    // Nova soundness: folded instance satisfiable -> ALL steps satisfiable.
    {
        // Generator set sized for extended IPA vector: next_pow2(n + m)
        size_t n = next_pow2(proof.num_r1cs_variables > 0 ? proof.num_r1cs_variables : 1);
        size_t N = next_pow2(n + proof.num_r1cs_constraints);
        if (N < 4) N = 4;
        const auto& gens = GeneratorSet::cached(N, ctx);

        // CRITICAL: derive decider commitments from the VERIFIED Nova chain,
        // NOT from proof.decider_commit_W (which is prover-controlled).
        Point decider_commit_W;
        Point decider_commit_E;
        Scalar decider_u;

        if (proof.num_steps == 1) {
            // Single-step: final_instance.commit_W is the only instance
            // (verified by Nova's u==1 check). Standard R1CS: u=1, E=0.
            decider_commit_W = proof.final_instance.commit_W;
            decider_commit_E = Point(); // identity (E=0)
            decider_u = Scalar::one();
        } else {
            // Multi-step: use the FOLDED running instance directly.
            // final_instance.commit_W, commit_E, and u were all verified
            // by NovaVerifier::verify() in Step 1 (fold chain replay checked
            // that they match the honest fold sequence).
            decider_commit_W = proof.final_instance.commit_W;
            decider_commit_E = proof.final_instance.commit_E;
            decider_u = proof.final_instance.u;
        }

        // Independently reconstruct the R1CS to verify the circuit identity hash.
        // MUST use alloc_input (not alloc) to match Nova's prove_step allocation
        // pattern — the variable indices differ, which changes the R1CS structure
        // hash and the constraint wiring.
        R1CS verifier_cs;
        const auto& fos = proof.final_output_state;
        std::vector<Variable> verifier_vars;
        if (fos.size() == VMState::flat_size()) {
            for (const auto& s : fos) {
                verifier_vars.push_back(verifier_cs.alloc_input(s));
            }
        } else {
            std::vector<Scalar> zero_state(VMState::flat_size(), Scalar::zero());
            for (const auto& s : zero_state) {
                verifier_vars.push_back(verifier_cs.alloc_input(s));
            }
        }
        // The fixed-shape proof only lets us ignore the concrete script bytes.
        // Public statement constants like tx_template_hash still affect the
        // synthesized constraint system and must match the prover's circuit.
        TapscriptStepCircuit verifier_circuit({}, expected_tx_template_hash);
        verifier_circuit.synthesize(verifier_cs, verifier_vars);
        auto expected_circuit_hash = hash_r1cs_structure(verifier_cs);

        // Reconstruct the R1CSIPAProof from the serialized proof fields
        R1CSIPAProof r1cs_proof;
        r1cs_proof.wire_commitment = proof.wire_commitment;
        r1cs_proof.witness_hash = proof.witness_hash;
        r1cs_proof.witness_nonce = proof.witness_nonce;
        r1cs_proof.circuit_hash = proof.circuit_hash;
        r1cs_proof.gen_hash = proof.gen_hash;
        r1cs_proof.r_combined = proof.r_combined;
        r1cs_proof.opening_proof.L = proof.opening_L;
        r1cs_proof.opening_proof.R = proof.opening_R;
        r1cs_proof.opening_proof.a_final = proof.opening_a_final;
        r1cs_proof.opening_proof.blind_final = proof.opening_blind_final;
        r1cs_proof.ipa = proof.ipa_proof;
        r1cs_proof.inner_product = Scalar::zero();
        r1cs_proof.commitment = proof.ipa_commitment;

        Transcript r1cs_transcript("R1CS_IPA_decider");

        // For multi-step proofs, replay the final_output_state binding.
        if (proof.num_steps > 1 &&
            proof.final_output_state.size() == VMState::flat_size()) {
            r1cs_transcript.append_u64("fos_len",
                static_cast<uint64_t>(proof.final_output_state.size()));
            for (const auto& s : proof.final_output_state) {
                r1cs_transcript.append_scalar("fos_elem", s);
            }
        }

        // Verify the decider proof.
        if (!r1cs_ipa_verify(r1cs_proof, verifier_cs, proof.num_r1cs_constraints,
                              proof.num_r1cs_variables,
                              expected_circuit_hash,
                              decider_commit_W,
                              decider_commit_E,
                              decider_u,
                              gens, r1cs_transcript, ctx)) {
            return false;
        }
    }

    // Step 3: Check that final state has success=1
    //
    // P1 AUDIT FIX #5: Do NOT trust proof.final_output_state blindly.
    // It is prover-supplied (deserialized from the proof blob) and not
    // authenticated by the Nova or IPA verifier on its own.
    //
    // For single-step proofs (num_steps == 1): the Nova verifier confirmed
    // u == 1, meaning final_instance.x holds the RAW (unfolded) output
    // state.  The last element IS the success flag, directly authenticated.
    //
    // For multi-step proofs: final_instance.x contains linear combinations
    // of step outputs due to folding — they are not raw values.  We still
    // require final_output_state for the success check, but it is now
    // authenticated via Fiat-Shamir binding: the prover injects
    // final_output_state into the R1CS IPA transcript BEFORE proof
    // generation (Step 2 above), and the verifier replays the same
    // injection before verification.  Any mutation of final_output_state
    // causes the IPA transcript to diverge, invalidating the proof.
    //
    // TODO(P1 #2): Full multi-step authentication requires extending the
    // Nova verifier to re-derive the folding challenges and verify that
    // final_instance.x is the correct linear combination of per-step
    // output states, making final_output_state fully redundant.

    if (proof.num_steps == 1) {
        // Single-step: final_instance.x IS the raw output state (u==1).
        // The success flag is at index ZKVM_MAX_STACK + 3.
        if (proof.final_instance.x.size() != VMState::flat_size()) return false;
        if (proof.final_instance.x[ZKVM_MAX_STACK + 3] != Scalar::one()) return false;
    } else {
        // Multi-step: final_output_state was bound into the IPA Fiat-Shamir
        // transcript in Step 2.  If a prover mutated it, the IPA verification
        // already failed above.  Now just read the success flag.
        if (proof.final_output_state.size() != VMState::flat_size()) return false;
        if (proof.final_instance.x.empty()) return false;

        VMState final_state = VMState::unflatten(proof.final_output_state);
        if (final_state.success != Scalar::one()) return false;
    }

    // Step 4: Verify Schnorr sub-proofs (deferred OP_CHECKSIG)
    //
    // Each sub-proof contains a BIP-340 signature + pubkey + message.
    // The verifier checks:
    //   a) The BIP-340 signature is valid (via libsecp256k1)
    //   b) The sub-proof's pubkey_x matches the VM state's checksig_pk_x
    //      (authenticated by Nova IVC + IPA decider)
    for (const auto& sub : proof.schnorr_sub_proofs) {
        // Structural checks
        if (sub.signature.size() != 64) return false;
        if (sub.pubkey_x.size() != 32) return false;
        if (sub.message_hash.size() != 32) return false;

        // Parse x-only pubkey
        secp256k1_xonly_pubkey xonly_pk;
        if (!secp256k1_xonly_pubkey_parse(ctx, &xonly_pk, sub.pubkey_x.data())) {
            return false;
        }

        // Verify BIP-340 Schnorr signature
        if (!secp256k1_schnorrsig_verify(ctx, sub.signature.data(),
                                          sub.message_hash.data(), 32, &xonly_pk)) {
            return false;
        }

        // Binding check: sub-proof pubkey must match VM state's checksig_pk_x.
        // checksig_pk_x is at position ZKVM_MAX_STACK + 6 in the state vector.
        Scalar vm_pk_x;
        if (proof.num_steps == 1) {
            if (proof.final_instance.x.size() > ZKVM_MAX_STACK + 6)
                vm_pk_x = proof.final_instance.x[ZKVM_MAX_STACK + 6];
        } else {
            if (proof.final_output_state.size() > ZKVM_MAX_STACK + 6) {
                VMState fs = VMState::unflatten(proof.final_output_state);
                vm_pk_x = fs.checksig_pk_x;
            }
        }

        // Compare: vm_pk_x (Scalar, big-endian) must equal sub.pubkey_x bytes
        if (std::memcmp(vm_pk_x.data(), sub.pubkey_x.data(), 32) != 0) {
            return false; // Binding mismatch: proof is for wrong pubkey
        }
    }

    // Step 5: Check that all deferred OP_CHECKSIGs have sub-proofs.
    //
    // If checksig_pk_x is non-zero in the final VM state, OP_CHECKSIG was
    // executed and a corresponding sub-proof MUST be present. Without this
    // check, an attacker could omit the sub-proof and the verifier would
    // accept an unverified OP_CHECKSIG.
    {
        Scalar vm_pk_x;
        if (proof.num_steps == 1) {
            if (proof.final_instance.x.size() > ZKVM_MAX_STACK + 6)
                vm_pk_x = proof.final_instance.x[ZKVM_MAX_STACK + 6];
        } else {
            if (proof.final_output_state.size() == VMState::flat_size()) {
                VMState fs = VMState::unflatten(proof.final_output_state);
                vm_pk_x = fs.checksig_pk_x;
            }
        }

        if (!vm_pk_x.is_zero() && proof.schnorr_sub_proofs.empty()) {
            return false; // OP_CHECKSIG ran but no sub-proof provided
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// TapscriptZKProof serialization
// ---------------------------------------------------------------------------

std::vector<uint8_t> TapscriptZKProof::serialize(secp256k1_context* ctx) const {
    std::vector<uint8_t> out;

    // Format: num_steps(4) || script_hash(32) || tx_template_hash(32) ||
    //         num_folding_proofs(4) || [folding proofs] ||
    //         ... existing fields ... ||
    //         num_schnorr_subs(4) || [schnorr sub-proofs]

    // num_steps
    uint32_t ns = static_cast<uint32_t>(num_steps);
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<uint8_t>((ns >> (8*i)) & 0xff));

    // num_r1cs_constraints + num_r1cs_variables
    uint32_t nrc = static_cast<uint32_t>(num_r1cs_constraints);
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<uint8_t>((nrc >> (8*i)) & 0xff));
    uint32_t nrv = static_cast<uint32_t>(num_r1cs_variables);
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<uint8_t>((nrv >> (8*i)) & 0xff));

    // script_hash
    out.insert(out.end(), script_hash.data(), script_hash.data() + 32);

    // tx_template_hash
    out.insert(out.end(), tx_template_hash.data(), tx_template_hash.data() + 32);

    // num_folding_proofs
    uint32_t nfp = static_cast<uint32_t>(folding_proofs.size());
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<uint8_t>((nfp >> (8*i)) & 0xff));

    // folding proofs (each: commit_T + commit_W_running + commit_W_new)
    for (const auto& fp : folding_proofs) {
        // commit_T
        {
            Point::Compressed c;
            if (fp.commit_T.serialize(c, ctx)) {
                out.insert(out.end(), c.begin(), c.end());
            } else {
                out.insert(out.end(), 33, 0);
            }
        }
        // commit_W_running
        {
            Point::Compressed c;
            if (fp.commit_W_running.serialize(c, ctx)) {
                out.insert(out.end(), c.begin(), c.end());
            } else {
                out.insert(out.end(), 33, 0);
            }
        }
        // commit_W_new
        {
            Point::Compressed c;
            if (fp.commit_W_new.serialize(c, ctx)) {
                out.insert(out.end(), c.begin(), c.end());
            } else {
                out.insert(out.end(), 33, 0);
            }
        }
    }

    // committed instance: u scalar
    out.insert(out.end(), final_instance.u.data(), final_instance.u.data() + 32);

    // committed instance: commit_W (33 bytes)
    {
        Point::Compressed c;
        if (final_instance.commit_W.serialize(c, ctx)) {
            out.insert(out.end(), c.begin(), c.end());
        } else {
            out.insert(out.end(), 33, 0);
        }
    }

    // committed instance: commit_E (33 bytes)
    {
        Point::Compressed c;
        if (final_instance.commit_E.serialize(c, ctx)) {
            out.insert(out.end(), c.begin(), c.end());
        } else {
            out.insert(out.end(), 33, 0);
        }
    }

    // committed instance public inputs
    uint32_t nx = static_cast<uint32_t>(final_instance.x.size());
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<uint8_t>((nx >> (8*i)) & 0xff));
    for (const auto& x : final_instance.x) {
        out.insert(out.end(), x.data(), x.data() + 32);
    }

    // IPA commitment (33 bytes compressed point)
    {
        Point::Compressed c;
        if (ipa_commitment.serialize(c, ctx)) {
            out.insert(out.end(), c.begin(), c.end());
        } else {
            out.insert(out.end(), 33, 0);
        }
    }

    // Wire value commitment (33 bytes)
    {
        Point::Compressed c;
        if (wire_commitment.serialize(c, ctx)) {
            out.insert(out.end(), c.begin(), c.end());
        } else {
            out.insert(out.end(), 33, 0);
        }
    }

    // Witness hash (32 bytes) + nonce (32 bytes)
    if (witness_hash.size() == 32) {
        out.insert(out.end(), witness_hash.begin(), witness_hash.end());
    } else {
        out.insert(out.end(), 32, 0);
    }
    // witness_nonce: NOT serialized (verifier never checks it — P2 malleability fix)
    // The nonce was used during proof generation for ZK but is not needed for verification.
    // Serializing it would create freely-mutable bytes in the proof blob.

    // Circuit hash (32 bytes) — P1 Audit Fix #3
    if (circuit_hash.size() == 32) {
        out.insert(out.end(), circuit_hash.begin(), circuit_hash.end());
    } else {
        out.insert(out.end(), 32, 0);
    }

    // Generator hash (32 bytes) — P2 Audit Fix #2
    if (gen_hash.size() == 32) {
        out.insert(out.end(), gen_hash.begin(), gen_hash.end());
    } else {
        out.insert(out.end(), 32, 0);
    }

    // decider_commit_W: NOT serialized (verifier derives from Nova chain — P2 malleability fix)
    // The verifier computes this from final_instance.commit_W or folding_proofs.back().commit_W_new.
    // Serializing it would create freely-mutable bytes that the verifier ignores.

    // Opening proof — Nova decider (IPA-based)
    // r_combined (32 bytes)
    out.insert(out.end(), r_combined.data(), r_combined.data() + 32);
    // opening_L count (4 bytes) + points (33 bytes each compressed)
    {
        uint32_t nL = static_cast<uint32_t>(opening_L.size());
        for (int i = 3; i >= 0; --i) out.push_back(static_cast<uint8_t>((nL >> (8*i)) & 0xff));
        for (const auto& pt : opening_L) {
            Point::Compressed c;
            if (pt.serialize(c, ctx)) {
                out.insert(out.end(), c.begin(), c.end());
            } else {
                out.insert(out.end(), 33, 0);
            }
        }
    }
    // opening_R points (33 bytes each compressed, same count as opening_L)
    {
        for (const auto& pt : opening_R) {
            Point::Compressed c;
            if (pt.serialize(c, ctx)) {
                out.insert(out.end(), c.begin(), c.end());
            } else {
                out.insert(out.end(), 33, 0);
            }
        }
    }
    // opening_a_final (32 bytes)
    out.insert(out.end(), opening_a_final.data(), opening_a_final.data() + 32);
    // opening_blind_final (32 bytes)
    out.insert(out.end(), opening_blind_final.data(), opening_blind_final.data() + 32);

    // Final output state (unfolded)
    uint32_t nfos = static_cast<uint32_t>(final_output_state.size());
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<uint8_t>((nfos >> (8*i)) & 0xff));
    for (const auto& s : final_output_state) {
        out.insert(out.end(), s.data(), s.data() + 32);
    }

    // IPA proof
    auto ipa_data = ipa_proof.serialize(ctx);
    uint32_t ipa_len = static_cast<uint32_t>(ipa_data.size());
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<uint8_t>((ipa_len >> (8*i)) & 0xff));
    out.insert(out.end(), ipa_data.begin(), ipa_data.end());

    // Schnorr sub-proofs (deferred OP_CHECKSIG)
    uint32_t nsub = static_cast<uint32_t>(schnorr_sub_proofs.size());
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<uint8_t>((nsub >> (8*i)) & 0xff));
    for (const auto& sub : schnorr_sub_proofs) {
        auto sub_data = sub.serialize();
        uint32_t sub_len = static_cast<uint32_t>(sub_data.size());
        for (int i = 3; i >= 0; --i) out.push_back(static_cast<uint8_t>((sub_len >> (8*i)) & 0xff));
        out.insert(out.end(), sub_data.begin(), sub_data.end());
    }

    return out;
}

bool TapscriptZKProof::deserialize(const std::vector<uint8_t>& data,
                                    TapscriptZKProof& out,
                                    secp256k1_context* ctx) {
    if (data.size() < 182) return false; // Conservative lower bound for compact proof header

    size_t offset = 0;

    // num_steps
    out.num_steps = (static_cast<uint32_t>(data[offset]) << 24) |
                    (static_cast<uint32_t>(data[offset+1]) << 16) |
                    (static_cast<uint32_t>(data[offset+2]) << 8) |
                    static_cast<uint32_t>(data[offset+3]);
    offset += 4;

    // num_r1cs_constraints
    out.num_r1cs_constraints = (static_cast<uint32_t>(data[offset]) << 24) |
                               (static_cast<uint32_t>(data[offset+1]) << 16) |
                               (static_cast<uint32_t>(data[offset+2]) << 8) |
                               static_cast<uint32_t>(data[offset+3]);
    offset += 4;

    // num_r1cs_variables
    if (offset + 4 > data.size()) return false;
    out.num_r1cs_variables = (static_cast<uint32_t>(data[offset]) << 24) |
                             (static_cast<uint32_t>(data[offset+1]) << 16) |
                             (static_cast<uint32_t>(data[offset+2]) << 8) |
                             static_cast<uint32_t>(data[offset+3]);
    offset += 4;

    // script_hash
    out.script_hash = Scalar(data.data() + offset);
    offset += 32;

    // tx_template_hash
    out.tx_template_hash = Scalar(data.data() + offset);
    offset += 32;

    // num_folding_proofs
    if (offset + 4 > data.size()) return false;
    uint32_t nfp = (static_cast<uint32_t>(data[offset]) << 24) |
                   (static_cast<uint32_t>(data[offset+1]) << 16) |
                   (static_cast<uint32_t>(data[offset+2]) << 8) |
                   static_cast<uint32_t>(data[offset+3]);
    offset += 4;

    // folding proofs (each: commit_T + commit_W_running + commit_W_new = 99 bytes)
    out.folding_proofs.resize(nfp);
    for (uint32_t i = 0; i < nfp; ++i) {
        if (offset + 99 > data.size()) return false;
        Point::parse(data.data() + offset, 33, out.folding_proofs[i].commit_T, ctx);
        offset += 33;
        Point::parse(data.data() + offset, 33, out.folding_proofs[i].commit_W_running, ctx);
        offset += 33;
        Point::parse(data.data() + offset, 33, out.folding_proofs[i].commit_W_new, ctx);
        offset += 33;
    }

    // committed instance: u scalar
    if (offset + 32 > data.size()) return false;
    out.final_instance.u = Scalar(data.data() + offset);
    offset += 32;

    // committed instance: commit_W (33 bytes)
    if (offset + 33 > data.size()) return false;
    Point::parse(data.data() + offset, 33, out.final_instance.commit_W, ctx);
    offset += 33;

    // committed instance: commit_E (33 bytes)
    if (offset + 33 > data.size()) return false;
    Point::parse(data.data() + offset, 33, out.final_instance.commit_E, ctx);
    offset += 33;

    // public inputs
    if (offset + 4 > data.size()) return false;
    uint32_t nx = (static_cast<uint32_t>(data[offset]) << 24) |
                  (static_cast<uint32_t>(data[offset+1]) << 16) |
                  (static_cast<uint32_t>(data[offset+2]) << 8) |
                  static_cast<uint32_t>(data[offset+3]);
    offset += 4;

    out.final_instance.x.resize(nx);
    for (uint32_t i = 0; i < nx; ++i) {
        if (offset + 32 > data.size()) return false;
        out.final_instance.x[i] = Scalar(data.data() + offset);
        offset += 32;
    }

    // IPA commitment (33 bytes)
    if (offset + 33 > data.size()) return false;
    Point::parse(data.data() + offset, 33, out.ipa_commitment, ctx);
    offset += 33;

    // Wire value commitment (33 bytes)
    if (offset + 33 > data.size()) return false;
    Point::parse(data.data() + offset, 33, out.wire_commitment, ctx);
    offset += 33;

    // Witness hash (32 bytes) — nonce no longer serialized (P2 malleability fix)
    if (offset + 32 > data.size()) return false;
    out.witness_hash.assign(data.begin() + offset, data.begin() + offset + 32);
    offset += 32;
    out.witness_nonce.clear();

    // Circuit hash (32 bytes) — P1 Audit Fix #3
    if (offset + 32 > data.size()) return false;
    out.circuit_hash.assign(data.begin() + offset, data.begin() + offset + 32);
    offset += 32;

    // Generator hash (32 bytes) — P2 Audit Fix #2
    if (offset + 32 > data.size()) return false;
    out.gen_hash.assign(data.begin() + offset, data.begin() + offset + 32);
    offset += 32;

    // decider_commit_W: no longer serialized (P2 malleability fix)
    // Verifier derives from Nova chain instead.
    out.decider_commit_W = Point();

    // Opening proof — Nova decider (IPA-based)
    // r_combined (32 bytes)
    if (offset + 32 > data.size()) return false;
    out.r_combined = Scalar(data.data() + offset);
    offset += 32;

    // opening_L count (4 bytes) + points (33 bytes each)
    if (offset + 4 > data.size()) return false;
    uint32_t nL = (static_cast<uint32_t>(data[offset]) << 24) |
                  (static_cast<uint32_t>(data[offset+1]) << 16) |
                  (static_cast<uint32_t>(data[offset+2]) << 8) |
                  static_cast<uint32_t>(data[offset+3]);
    offset += 4;

    out.opening_L.resize(nL);
    for (uint32_t i = 0; i < nL; ++i) {
        if (offset + 33 > data.size()) return false;
        Point::parse(data.data() + offset, 33, out.opening_L[i], ctx);
        offset += 33;
    }

    // opening_R points (33 bytes each, same count as opening_L)
    out.opening_R.resize(nL);
    for (uint32_t i = 0; i < nL; ++i) {
        if (offset + 33 > data.size()) return false;
        Point::parse(data.data() + offset, 33, out.opening_R[i], ctx);
        offset += 33;
    }

    // opening_a_final (32 bytes)
    if (offset + 32 > data.size()) return false;
    out.opening_a_final = Scalar(data.data() + offset);
    offset += 32;

    // opening_blind_final (32 bytes)
    if (offset + 32 > data.size()) return false;
    out.opening_blind_final = Scalar(data.data() + offset);
    offset += 32;

    // Final output state (unfolded)
    if (offset + 4 > data.size()) return false;
    uint32_t nfos = (static_cast<uint32_t>(data[offset]) << 24) |
                    (static_cast<uint32_t>(data[offset+1]) << 16) |
                    (static_cast<uint32_t>(data[offset+2]) << 8) |
                    static_cast<uint32_t>(data[offset+3]);
    offset += 4;

    out.final_output_state.resize(nfos);
    for (uint32_t i = 0; i < nfos; ++i) {
        if (offset + 32 > data.size()) return false;
        out.final_output_state[i] = Scalar(data.data() + offset);
        offset += 32;
    }

    // IPA proof
    if (offset + 4 > data.size()) return false;
    uint32_t ipa_len = (static_cast<uint32_t>(data[offset]) << 24) |
                       (static_cast<uint32_t>(data[offset+1]) << 16) |
                       (static_cast<uint32_t>(data[offset+2]) << 8) |
                       static_cast<uint32_t>(data[offset+3]);
    offset += 4;

    if (offset + ipa_len > data.size()) return false;
    std::vector<uint8_t> ipa_data(data.begin() + offset, data.begin() + offset + ipa_len);
    IPAProof::deserialize(ipa_data, out.ipa_proof, ctx);
    offset += ipa_len;

    // Schnorr sub-proofs (optional — backward compatible with old proofs)
    if (offset + 4 <= data.size()) {
        uint32_t nsub = (static_cast<uint32_t>(data[offset]) << 24) |
                        (static_cast<uint32_t>(data[offset+1]) << 16) |
                        (static_cast<uint32_t>(data[offset+2]) << 8) |
                        static_cast<uint32_t>(data[offset+3]);
        offset += 4;

        for (uint32_t i = 0; i < nsub; ++i) {
            if (offset + 4 > data.size()) return false;
            uint32_t sub_len = (static_cast<uint32_t>(data[offset]) << 24) |
                               (static_cast<uint32_t>(data[offset+1]) << 16) |
                               (static_cast<uint32_t>(data[offset+2]) << 8) |
                               static_cast<uint32_t>(data[offset+3]);
            offset += 4;
            if (offset + sub_len > data.size()) return false;

            SchnorrSubProof sub;
            size_t sub_offset = 0;
            std::vector<uint8_t> sub_data(data.begin() + offset, data.begin() + offset + sub_len);
            if (!SchnorrSubProof::deserialize(sub_data, sub_offset, sub)) return false;
            out.schnorr_sub_proofs.push_back(sub);
            offset += sub_len;
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// SchnorrSubProof serialization
// ---------------------------------------------------------------------------

std::vector<uint8_t> SchnorrSubProof::serialize() const {
    std::vector<uint8_t> out;
    // Format: signature(64) || pubkey_x(32) || message_hash(32) = 128 bytes
    out.insert(out.end(), signature.begin(), signature.end());
    out.insert(out.end(), pubkey_x.begin(), pubkey_x.end());
    out.insert(out.end(), message_hash.begin(), message_hash.end());
    return out;
}

bool SchnorrSubProof::deserialize(const std::vector<uint8_t>& data, size_t& offset,
                                   SchnorrSubProof& out) {
    if (offset + 128 > data.size()) return false;
    out.signature.assign(data.begin() + offset, data.begin() + offset + 64);
    offset += 64;
    out.pubkey_x.assign(data.begin() + offset, data.begin() + offset + 32);
    offset += 32;
    out.message_hash.assign(data.begin() + offset, data.begin() + offset + 32);
    offset += 32;
    return true;
}

} // namespace zkvm
} // namespace zk
} // namespace dinero
