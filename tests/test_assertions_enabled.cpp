#ifdef NDEBUG
#error "test targets must not define NDEBUG"
#endif

// Re-include after an explicit undef so the source ratchet recognizes this as
// the one intentional raw-assert tripwire rather than ordinary assertion debt.
#undef NDEBUG
#include <cassert>

int main() {
    bool expression_was_evaluated = false;
    assert((expression_was_evaluated = true));
    return expression_was_evaluated ? 0 : 1;
}
