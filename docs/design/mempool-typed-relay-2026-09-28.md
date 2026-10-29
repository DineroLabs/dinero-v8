# Typed transaction retrieval for relay

The existing TxRelayManager retrieval owner now returns an optional immutable mempool body. HandleGetData captures retrieval and send callbacks together, obtains the owned body, requires its transaction ID to equal the requested ID, and serializes its actual family. Empty or missing bodies and mismatched IDs are not sent. Callback invocation and destruction remain outside the callback mutex. The historical callback setter adapts to the same owner, so replacing either setter leaves no stale fallback.

DaemonApp installs the typed callback using MempoolService's operation owner and one existing MempoolEntry capture. The copied body remains available after the entry or pool changes. The service owner covers retrieval; the returned body owns its own lifetime. This changes ordinary outbound transaction serving. Incoming transaction parsing, validation, orphan handling, Orchard admission, Utreexo proof relay, selection and persistence retain their current scope.

## Qualification

Fresh enabled/disabled full daemon builds plus 39 declared targets passed. Each configuration passed 43 component CTests and six daemon CTests. Four enabled and three disabled cases exercise real signed historical admission followed by pool removal and callback replacement during retrieval, empty/missing/mismatched callback results, historical setter compatibility, and exact canonical Orchard wire bytes through the actual send callback. The Orchard callback fixture does not establish admission, proof verification or network transport.

All 75 linked project C++ translation units were freshly instrumented with ASan/UBSan; all four cases passed with 1,304 stable source/header input hashes. The final link map contains no project C++ archive members. External libraries, Rust, C/PQClean, DaemonApp wiring, OFF binaries and network transport are outside this sanitizer scope; macOS leak detection was disabled. The prior ARM dependency qualification remains separate. Normal link-map discovery intentionally uses archives.

The 1,214 other prior fixture files and 174 prior Orchard CTest commands remain unchanged. The earlier callback-owner fixture now requests the actual ID of its returned Tx(3), rather than an unrelated numeric label. Its existing callback-replacement and completion assertions remain unchanged. No unsafe-original, race or synchronization-removal controls are used. Mainnet remains unset; release qualification is incomplete.
