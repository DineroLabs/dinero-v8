// Copyright (c) 2026 Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

/**
 * R1CS Gadgets — Reusable Circuit Building Blocks
 *
 * Gadgets are composable circuit components that add R1CS constraints
 * to enforce specific relationships. They are the "instruction set"
 * for building arithmetic circuits.
 *
 * Each gadget:
 *   - Takes Variable references (already allocated in the R1CS)
 *   - Adds constraints that enforce the desired relationship
 *   - May allocate new intermediate variables
 *   - Returns output Variable(s)
 *
 * Gadgets form a hierarchy:
 *   Basic: boolean, equality, add, mul, conditional select
 *   Bitwise: decomposition, range check, XOR, AND, OR
 *   Comparison: less than, less than or equal
 *   Crypto: SHA256, Schnorr verify (in separate files)
 *   VM: stack push/pop, opcode dispatch (in tapscript_step)
 */

#include "zk/zkvm/r1cs.h"
#include <vector>

namespace dinero {
namespace zk {
namespace zkvm {
namespace gadgets {

// ---------------------------------------------------------------------------
// Basic gadgets
// ---------------------------------------------------------------------------

/**
 * Boolean constraint: x * (1 - x) = 0
 * Enforces that x is either 0 or 1.
 */
void enforce_boolean(R1CS& cs, Variable x, const std::string& label = "boolean");

/**
 * Multiplication: allocate and constrain z = a * b
 * Returns the output variable z.
 */
Variable mul(R1CS& cs, Variable a, Variable b, const std::string& label = "mul");

/**
 * Addition: allocate and constrain z = a + b
 * Returns the output variable z.
 */
Variable add(R1CS& cs, Variable a, Variable b, const std::string& label = "add");

/**
 * Subtraction: allocate and constrain z = a - b
 */
Variable sub(R1CS& cs, Variable a, Variable b, const std::string& label = "sub");

/**
 * Conditional select: z = condition ? a : b
 * condition must be boolean (0 or 1).
 * Enforces: z = condition * a + (1 - condition) * b
 *         = condition * (a - b) + b
 */
Variable select(R1CS& cs, Variable condition, Variable a, Variable b,
                const std::string& label = "select");

/**
 * Assert equal: a == b
 * Adds constraint: (a - b) * 1 = 0
 */
void assert_equal(R1CS& cs, Variable a, Variable b, const std::string& label = "eq");

/**
 * Assert zero: a == 0
 */
void assert_zero(R1CS& cs, Variable a, const std::string& label = "zero");

/**
 * Constant: allocate a variable constrained to a specific value.
 * Returns a variable whose value is fixed to `val`.
 */
Variable constant(R1CS& cs, const Scalar& val, const std::string& label = "const");

// ---------------------------------------------------------------------------
// Bitwise gadgets
// ---------------------------------------------------------------------------

/**
 * Bit decomposition: decompose a scalar into `num_bits` boolean variables.
 * Returns a vector of boolean variables [bit_0, bit_1, ..., bit_{n-1}]
 * where bit_0 is the least significant bit.
 *
 * Enforces: value = sum(bit_i * 2^i)
 * Enforces: each bit_i is boolean
 */
std::vector<Variable> to_bits(R1CS& cs, Variable value, size_t num_bits,
                               const std::string& label = "to_bits");

/**
 * Reconstruct a scalar from boolean bit variables.
 * Returns a variable whose value = sum(bits[i] * 2^i)
 */
Variable from_bits(R1CS& cs, const std::vector<Variable>& bits,
                    const std::string& label = "from_bits");

/**
 * Range check: enforce 0 <= value < 2^num_bits
 * Uses bit decomposition internally.
 */
void range_check(R1CS& cs, Variable value, size_t num_bits,
                  const std::string& label = "range");

/**
 * XOR: z = a XOR b (a, b must be boolean)
 * a XOR b = a + b - 2*a*b
 */
Variable xor_bits(R1CS& cs, Variable a, Variable b, const std::string& label = "xor");

/**
 * AND: z = a AND b (a, b must be boolean)
 * a AND b = a * b
 */
Variable and_bits(R1CS& cs, Variable a, Variable b, const std::string& label = "and");

/**
 * OR: z = a OR b (a, b must be boolean)
 * a OR b = a + b - a*b
 */
Variable or_bits(R1CS& cs, Variable a, Variable b, const std::string& label = "or");

/**
 * NOT: z = 1 - a (a must be boolean)
 */
Variable not_bit(R1CS& cs, Variable a, const std::string& label = "not");

// ---------------------------------------------------------------------------
// Comparison gadgets
// ---------------------------------------------------------------------------

/**
 * Is zero: returns a boolean variable that is 1 if value == 0, else 0.
 * Uses the inverse trick: allocate inv, enforce value * inv = 1 - is_zero.
 */
Variable is_zero(R1CS& cs, Variable value, const std::string& label = "is_zero");

/**
 * Equality check: returns a boolean variable, 1 if a == b, else 0.
 */
Variable is_equal(R1CS& cs, Variable a, Variable b, const std::string& label = "is_eq");

// ---------------------------------------------------------------------------
// Memory / lookup gadgets (for VM stack operations)
// ---------------------------------------------------------------------------

/**
 * Multiplexer: given a selector index and N values, output values[selector].
 * selector must be in range [0, N).
 *
 * Uses N boolean indicator variables and conditional selection.
 * Cost: ~2N constraints.
 */
Variable mux(R1CS& cs, Variable selector, const std::vector<Variable>& values,
             const std::string& label = "mux");

/**
 * Demultiplexer: given a selector index and input value, produce N outputs
 * where output[selector] = input and all others = 0.
 */
std::vector<Variable> demux(R1CS& cs, Variable selector, Variable input,
                             size_t n, const std::string& label = "demux");

} // namespace gadgets
} // namespace zkvm
} // namespace zk
} // namespace dinero
