// Copyright (c) 2026 Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "zk/zkvm/gadgets.h"
#include "crypto/evp_secp256k1.h"
#include <cassert>

namespace dinero {
namespace zk {
namespace zkvm {
namespace gadgets {

// ---------------------------------------------------------------------------
// Basic gadgets
// ---------------------------------------------------------------------------

void enforce_boolean(R1CS& cs, Variable x, const std::string& label) {
    // x * (1 - x) = 0
    cs.constrain(
        LinearCombination(x),
        LinearCombination(Scalar::one(), VAR_ONE) - LinearCombination(x),
        LinearCombination::constant(Scalar::zero()),
        label
    );
}

Variable mul(R1CS& cs, Variable a, Variable b, const std::string& label) {
    Scalar val = cs.get_value(a) * cs.get_value(b);
    Variable z = cs.alloc(val);
    // a * b = z
    cs.constrain(LinearCombination(a), LinearCombination(b), LinearCombination(z), label);
    return z;
}

Variable add(R1CS& cs, Variable a, Variable b, const std::string& label) {
    Scalar val = cs.get_value(a) + cs.get_value(b);
    Variable z = cs.alloc(val);
    // (a + b) * 1 = z
    cs.constrain(
        LinearCombination(a) + LinearCombination(b),
        LinearCombination(VAR_ONE),
        LinearCombination(z),
        label
    );
    return z;
}

Variable sub(R1CS& cs, Variable a, Variable b, const std::string& label) {
    Scalar val = cs.get_value(a) - cs.get_value(b);
    Variable z = cs.alloc(val);
    // (a - b) * 1 = z
    cs.constrain(
        LinearCombination(a) - LinearCombination(b),
        LinearCombination(VAR_ONE),
        LinearCombination(z),
        label
    );
    return z;
}

Variable select(R1CS& cs, Variable condition, Variable a, Variable b,
                const std::string& label) {
    // z = cond * a + (1 - cond) * b = cond * (a - b) + b
    // Rearranged: cond * (a - b) = z - b
    Scalar cond_val = cs.get_value(condition);
    Scalar a_val = cs.get_value(a);
    Scalar b_val = cs.get_value(b);
    Scalar z_val = cond_val.is_zero() ? b_val : a_val;
    Variable z = cs.alloc(z_val);

    // condition * (a - b) = (z - b)
    cs.constrain(
        LinearCombination(condition),
        LinearCombination(a) - LinearCombination(b),
        LinearCombination(z) - LinearCombination(b),
        label
    );
    return z;
}

void assert_equal(R1CS& cs, Variable a, Variable b, const std::string& label) {
    cs.enforce_equal(LinearCombination(a), LinearCombination(b), label);
}

void assert_zero(R1CS& cs, Variable a, const std::string& label) {
    cs.enforce_zero(LinearCombination(a), label);
}

Variable constant(R1CS& cs, const Scalar& val, const std::string& label) {
    Variable v = cs.alloc(val);
    // v * 1 = val * ONE
    cs.constrain(
        LinearCombination(v),
        LinearCombination(VAR_ONE),
        LinearCombination(val, VAR_ONE),
        label
    );
    return v;
}

// ---------------------------------------------------------------------------
// Bitwise gadgets
// ---------------------------------------------------------------------------

std::vector<Variable> to_bits(R1CS& cs, Variable value, size_t num_bits,
                               const std::string& label) {
    assert(num_bits <= 256);
    Scalar val = cs.get_value(value);
    const uint8_t* bytes = val.data(); // Big-endian

    std::vector<Variable> bits(num_bits);

    // Extract bits from big-endian representation
    for (size_t i = 0; i < num_bits; ++i) {
        // Bit i (LSB = bit 0)
        size_t byte_idx = 31 - (i / 8);
        size_t bit_idx = i % 8;
        bool bit = (bytes[byte_idx] >> bit_idx) & 1;
        bits[i] = cs.alloc(bit ? Scalar::one() : Scalar::zero());
        enforce_boolean(cs, bits[i], label + "_bit_" + std::to_string(i));
    }

    // Enforce: value = sum(bits[i] * 2^i)
    LinearCombination sum;
    Scalar power = Scalar::one();
    Scalar two(uint64_t(2));
    for (size_t i = 0; i < num_bits; ++i) {
        sum = sum + LinearCombination(power, bits[i]);
        power = power * two;
    }

    cs.enforce_equal(LinearCombination(value), sum, label + "_recompose");

    return bits;
}

Variable from_bits(R1CS& cs, const std::vector<Variable>& bits,
                    const std::string& label) {
    // Compute value = sum(bits[i] * 2^i)
    Scalar val = Scalar::zero();
    Scalar power = Scalar::one();
    Scalar two(uint64_t(2));
    for (size_t i = 0; i < bits.size(); ++i) {
        if (!cs.get_value(bits[i]).is_zero()) {
            val += power;
        }
        power = power * two;
    }

    Variable result = cs.alloc(val);

    // Constrain: result = sum(bits[i] * 2^i)
    LinearCombination sum;
    power = Scalar::one();
    for (size_t i = 0; i < bits.size(); ++i) {
        sum = sum + LinearCombination(power, bits[i]);
        power = power * two;
    }

    cs.enforce_equal(LinearCombination(result), sum, label);
    return result;
}

void range_check(R1CS& cs, Variable value, size_t num_bits,
                  const std::string& label) {
    // Decompose into bits — this automatically constrains the range
    to_bits(cs, value, num_bits, label);
}

Variable xor_bits(R1CS& cs, Variable a, Variable b, const std::string& label) {
    // a XOR b = a + b - 2*a*b
    Scalar a_val = cs.get_value(a);
    Scalar b_val = cs.get_value(b);
    Scalar ab = a_val * b_val;
    Scalar result_val = a_val + b_val - Scalar(uint64_t(2)) * ab;

    Variable result = cs.alloc(result_val);

    // We need: a + b - 2*a*b = result
    // Rearranged: 2*a*b = a + b - result
    // So: (2*a) * b = (a + b - result)
    cs.constrain(
        LinearCombination(Scalar(uint64_t(2)), a),
        LinearCombination(b),
        LinearCombination(a) + LinearCombination(b) - LinearCombination(result),
        label
    );

    return result;
}

Variable and_bits(R1CS& cs, Variable a, Variable b, const std::string& label) {
    // a AND b = a * b
    return mul(cs, a, b, label);
}

Variable or_bits(R1CS& cs, Variable a, Variable b, const std::string& label) {
    // a OR b = a + b - a*b
    Scalar a_val = cs.get_value(a);
    Scalar b_val = cs.get_value(b);
    Scalar result_val = a_val + b_val - (a_val * b_val);

    Variable result = cs.alloc(result_val);

    // a * b = a + b - result
    cs.constrain(
        LinearCombination(a),
        LinearCombination(b),
        LinearCombination(a) + LinearCombination(b) - LinearCombination(result),
        label
    );

    return result;
}

Variable not_bit(R1CS& cs, Variable a, const std::string& label) {
    Scalar a_val = cs.get_value(a);
    Scalar result_val = Scalar::one() - a_val;
    Variable result = cs.alloc(result_val);

    // result = 1 - a
    // result * 1 = 1 - a
    cs.constrain(
        LinearCombination(result),
        LinearCombination(VAR_ONE),
        LinearCombination(Scalar::one(), VAR_ONE) - LinearCombination(a),
        label
    );

    return result;
}

// ---------------------------------------------------------------------------
// Comparison gadgets
// ---------------------------------------------------------------------------

Variable is_zero(R1CS& cs, Variable value, const std::string& label) {
    Scalar val = cs.get_value(value);
    bool val_is_zero = val.is_zero();

    // Allocate the boolean result
    Variable result = cs.alloc(val_is_zero ? Scalar::one() : Scalar::zero());
    enforce_boolean(cs, result, label + "_bool");

    // Allocate the inverse (or 0 if value is 0)
    Scalar inv_val = val_is_zero ? Scalar::zero()
        : val.inverse(dinero::crypto::GetSecp256k1ContextSignVerify());
    // The trick: value * inv = 1 - result
    //            value * result = 0
    // If value != 0: inv = value^{-1}, result = 0
    // If value == 0: inv = anything, result = 1
    Variable inv = cs.alloc(inv_val);

    // value * result = 0 (if value != 0, result must be 0)
    cs.constrain(
        LinearCombination(value),
        LinearCombination(result),
        LinearCombination::constant(Scalar::zero()),
        label + "_vr"
    );

    // value * inv = 1 - result (if value == 0, then 1 - result = 0, so result = 1)
    cs.constrain(
        LinearCombination(value),
        LinearCombination(inv),
        LinearCombination(Scalar::one(), VAR_ONE) - LinearCombination(result),
        label + "_vinv"
    );

    return result;
}

Variable is_equal(R1CS& cs, Variable a, Variable b, const std::string& label) {
    Variable diff = sub(cs, a, b, label + "_diff");
    return is_zero(cs, diff, label);
}

// ---------------------------------------------------------------------------
// Memory / lookup gadgets
// ---------------------------------------------------------------------------

Variable mux(R1CS& cs, Variable selector, const std::vector<Variable>& values,
             const std::string& label) {
    size_t n = values.size();
    assert(n > 0);
    if (n == 1) return values[0];

    Scalar sel_val = cs.get_value(selector);

    // Create indicator variables: ind_i = 1 if selector == i, else 0
    std::vector<Variable> indicators(n);
    for (size_t i = 0; i < n; ++i) {
        Scalar idx(static_cast<uint64_t>(i));
        bool is_selected = (sel_val == idx);
        indicators[i] = cs.alloc(is_selected ? Scalar::one() : Scalar::zero());
        enforce_boolean(cs, indicators[i], label + "_ind_" + std::to_string(i));
    }

    // Enforce: sum(indicators) = 1 (exactly one is selected)
    LinearCombination ind_sum;
    for (size_t i = 0; i < n; ++i) {
        ind_sum = ind_sum + LinearCombination(indicators[i]);
    }
    cs.enforce_equal(ind_sum, LinearCombination(Scalar::one(), VAR_ONE), label + "_sum");

    // Enforce: selector = sum(i * indicators[i])
    LinearCombination sel_sum;
    for (size_t i = 0; i < n; ++i) {
        sel_sum = sel_sum + LinearCombination(Scalar(static_cast<uint64_t>(i)), indicators[i]);
    }
    cs.enforce_equal(LinearCombination(selector), sel_sum, label + "_sel");

    // Output: result = sum(indicators[i] * values[i])
    // Build incrementally to stay within R1CS (one mul per indicator-value pair)
    Scalar result_val = Scalar::zero();
    for (size_t i = 0; i < n; ++i) {
        if (!cs.get_value(indicators[i]).is_zero()) {
            result_val = cs.get_value(values[i]);
        }
    }
    Variable result = cs.alloc(result_val);

    // Enforce: result = sum(indicators[i] * values[i])
    // This requires auxiliary multiplication variables
    Variable acc = constant(cs, Scalar::zero(), label + "_acc_init");
    for (size_t i = 0; i < n; ++i) {
        Variable term = mul(cs, indicators[i], values[i], label + "_term_" + std::to_string(i));
        acc = add(cs, acc, term, label + "_acc_" + std::to_string(i));
    }

    assert_equal(cs, result, acc, label + "_mux_check");
    return result;
}

std::vector<Variable> demux(R1CS& cs, Variable selector, Variable input,
                             size_t n, const std::string& label) {
    Scalar sel_val = cs.get_value(selector);
    Scalar in_val = cs.get_value(input);

    std::vector<Variable> outputs(n);
    for (size_t i = 0; i < n; ++i) {
        Scalar idx(static_cast<uint64_t>(i));
        bool selected = (sel_val == idx);
        outputs[i] = cs.alloc(selected ? in_val : Scalar::zero());
    }

    // Enforce: exactly one output is non-zero, equals input
    // outputs[i] = indicator_i * input
    // sum(indicator_i) = 1
    // indicator_i * (selector - i) = 0

    // Reuse indicator variables from mux logic
    std::vector<Variable> indicators(n);
    for (size_t i = 0; i < n; ++i) {
        Scalar idx(static_cast<uint64_t>(i));
        bool is_selected = (sel_val == idx);
        indicators[i] = cs.alloc(is_selected ? Scalar::one() : Scalar::zero());
        enforce_boolean(cs, indicators[i], label + "_ind_" + std::to_string(i));

        // outputs[i] = indicators[i] * input
        cs.constrain(
            LinearCombination(indicators[i]),
            LinearCombination(input),
            LinearCombination(outputs[i]),
            label + "_out_" + std::to_string(i)
        );
    }

    // sum(indicators) = 1
    LinearCombination ind_sum;
    for (size_t i = 0; i < n; ++i) {
        ind_sum = ind_sum + LinearCombination(indicators[i]);
    }
    cs.enforce_equal(ind_sum, LinearCombination(Scalar::one(), VAR_ONE), label + "_ind_sum");

    return outputs;
}

} // namespace gadgets
} // namespace zkvm
} // namespace zk
} // namespace dinero
