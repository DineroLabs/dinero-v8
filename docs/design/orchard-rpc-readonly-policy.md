# Orchard HTTP read-only policy

Unqualified candidate: add the six Orchard mutation methods to the existing HTTP admin classification before handler lookup. They create accounts or addresses, reserve payments, queue proofs, or finish/submits stored transactions. Existing generated aliases (the suffix after the first dot) and explicit registry aliases inherit the same refusal. Observation methods remain available under their existing wallet-binding and authentication checks.

The new four-case test target calls the real HTTP dispatch function in-process through the existing test friend. It never starts the listener, opens sockets, constructs wallets, submits transactions, or changes activation. Sentinel handlers establish refusal before invocation and preservation of writable/read behavior; a separate case uses the actual Orchard registrations with no backend or wallet. This does not qualify network authentication, desktop transport, full payment execution, platform, or release readiness.

No original-source RED, successful build, or test execution is claimed at preparation. Fresh ON/OFF builds and actual tests, a copied original-policy negative control, and a measured sanitizer scope are required before applying this candidate. Public publication and CI remain held.
