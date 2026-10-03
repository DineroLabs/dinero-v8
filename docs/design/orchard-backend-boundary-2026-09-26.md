# Orchard optional-backend build boundary

The default Linux full and QUIC builds at 5b431 failed before running CTests.
ChainstateService unconditionally included the runtime origin projection and
index delivery headers. Those headers reach `orchard_transaction.h`, which is
available only when the optional Orchard backend target supplies its include
paths. The replay test also included and used the backend-only projection API
unconditionally.

Both service headers now live inside the existing runtime-reader compile
boundary. The replay target receives its own test definition and link dependency
from the optional Orchard implementation. Its two projection tests and access
shim are compiled when that implementation is present. Both remain required in
the Orchard-enabled lane. In the disabled build, an additional test requires the
origin service API to refuse without opening a wallet or chain. The eight replay
cases and selected-history case run in both configurations.

The default Tests workflow now verifies that its generated service/replay compile
commands contain neither the runtime-reader definition nor Orchard backend include
paths. It explicitly executes and retains the disabled-backend replay/refusal
case, in addition to the existing broad suite. No deadline, assertion or existing
Orchard-enabled execution requirement was relaxed.

## Correction to earlier qualification scope

Earlier local checks removed the runtime-reader macro from an Orchard-enabled
compile command but retained that command's include paths. Those checks verified
conditional implementation compilation only; they did not establish a complete
backend-OFF configuration. Their original receipts remain retained. This change
uses a fresh CMake build configured with the backend OFF and the actual generated
compile commands. Copies of the preceding service and replay-test source fail in
that environment with the missing-header diagnostic, while the corrected source
builds the daemon and replay test.

## Qualification

Two fresh CMake configurations built the actual daemon and declared affected
test targets. Backend OFF passed AssumeUtxoReplay in 2.11 seconds with ten internal
cases. Backend ON passed AssumeUtxoReplay (4.06 seconds, eleven cases) and
OrchardServiceDeliverySource (7.50 seconds), including actual origin capture and
adoption. Both unchanged Orchard workflow selectors still enumerate 46 enabled
root registrations; these were not all executed locally. This is not a local
build of every default target or the complete default CTest suite.

All 203 linked project C++ translation units of the service fixture were freshly
instrumented with ASan/UBSan. The service fixture passed and its maps contain no
project C++ archive members. The final replay-test-only correction leaves those
203 source files and generated commands unchanged; the service sanitizer was
rerun afterward. The replay test translation unit and backend-OFF executable
are outside that sanitizer scope. Rust/external dependencies are uninstrumented,
macOS leak detection is disabled, and the ARM dependency gate remains open.
Inherited version labels and prebuilt OpenSSL are not release provenance.

An intermediate enabled replay run selected the disabled case because the service
reader definition is source-local. The case-inventory check caught this; the
final test-specific CMake definition restores both enabled projection cases.
That intermediate ten-case enabled run does not qualify them. No new runtime
recovery/provider, activation, deployment, CT-history or platform gate is closed
by this compilation-boundary fix.
