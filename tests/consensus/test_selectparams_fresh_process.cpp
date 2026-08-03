// SelectParams() must not mark a chain selected when it rejects one.
//
// WHY THIS IS A SEPARATE EXECUTABLE
// ---------------------------------
// The interesting state is "no chain has ever been selected in this process".
// A gtest case cannot observe it: by the time any test body runs, some earlier
// test (or the fixture) has already called SelectParams() successfully, so
// g_paramsSelected is already true and a leaked commit is indistinguishable
// from the legitimate prior one. A gtest death test does not help either --
// fork() copies the parent's already-selected globals into the child.
//
// So this runs as its own process, and the very first consensus call it makes
// is the rejecting one.
//
// WHAT IT PINS
// ------------
// SelectParams() used to set g_paramsSelected = true as its FIRST statement,
// before the switch that resolves the chain. An unknown chain therefore threw
// std::invalid_argument while leaving IsChainSelected() == true: the process
// claimed a chain was selected when none had been. Callers that catch would
// then trust Params(), which still pointed at whatever was active before.
//
// NO assert() HERE, DELIBERATELY
// ------------------------------
// assert() compiles to ((void)0) under NDEBUG, and CI builds Release. A test
// gated on assert() would pass without checking anything -- which is the exact
// defect this file exists to rule out. Every check below is an explicit
// comparison with a distinct non-zero exit code, so it gates in every build
// configuration.

#include "consensus/chainparams.h"

#include <cstdio>
#include <stdexcept>

namespace {

// Distinct codes so a CI failure names the broken step without needing output.
enum ExitCode : int {
    kOk = 0,
    kAlreadySelectedAtStartup = 10,
    kDidNotThrow = 11,
    kWrongExceptionType = 12,
    kSelectedAfterRejection = 13,
    kActiveChainReadable = 14,
    kValidSelectionBroken = 15,
};

}  // namespace

int main() {
    // Precondition: this process has selected nothing. If this fails, the test
    // is not measuring what it claims and must not be treated as a pass.
    if (dinero::IsChainSelected()) {
        std::fprintf(stderr,
                     "precondition failed: a chain was already selected before "
                     "main() ran; this test can no longer observe the "
                     "never-selected state\n");
        return kAlreadySelectedAtStartup;
    }

    // Reject an unknown chain. Chain's underlying type is uint8_t, so 99 is a
    // representable value -- this is a well-defined enum value, not UB.
    try {
        dinero::SelectParams(static_cast<dinero::Chain>(99));
        std::fprintf(stderr, "SelectParams() accepted an unknown chain\n");
        return kDidNotThrow;
    } catch (const std::invalid_argument&) {
        // expected
    } catch (const std::exception& e) {
        std::fprintf(stderr, "wrong exception type: %s\n", e.what());
        return kWrongExceptionType;
    }

    // THE ASSERTION THIS FILE EXISTS FOR. A rejected selection must leave the
    // process in its original never-selected state.
    if (dinero::IsChainSelected()) {
        std::fprintf(stderr,
                     "SelectParams() marked a chain selected while rejecting "
                     "it; IsChainSelected() is now true with no valid chain\n");
        return kSelectedAfterRejection;
    }

    // ...and GetActiveChain() must still refuse to answer, rather than
    // reporting a chain that was never successfully selected.
    try {
        const dinero::Chain chain = dinero::GetActiveChain();
        std::fprintf(stderr,
                     "GetActiveChain() returned %d after a rejected selection\n",
                     static_cast<int>(chain));
        return kActiveChainReadable;
    } catch (const std::runtime_error&) {
        // expected: "Chain parameters not selected"
    }

    // A valid selection must still work afterwards -- the rejection must not
    // have wedged the process. Deliberately asserted WITHOUT reference to any
    // particular activation height, so this file stays valid across activation
    // changes and does not silently become a covenant test.
    dinero::SelectParams(dinero::Chain::MAINNET);
    if (!dinero::IsChainSelected() ||
        dinero::GetActiveChain() != dinero::Chain::MAINNET) {
        std::fprintf(stderr,
                     "a valid selection after a rejected one did not take "
                     "effect\n");
        return kValidSelectionBroken;
    }

    // And a second, different valid selection must also take effect, so the
    // check above cannot pass merely because mainnet happened to be active
    // already.
    dinero::SelectParams(dinero::Chain::REGTEST);
    if (dinero::GetActiveChain() != dinero::Chain::REGTEST) {
        std::fprintf(stderr, "switching to a second valid chain failed\n");
        return kValidSelectionBroken;
    }

    std::printf("SelectParams fresh-process rejection is atomic\n");
    return kOk;
}
