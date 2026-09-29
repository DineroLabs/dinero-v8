# Typed block and wallet service integration

Locally qualified as part of the combined typed block and wallet service integration batch.

This batch composes the prepared typed block relay, network routing, canonical announcement, retained reorg readmission and wallet service operation ownership changes. Each component design remains beside this document. All five enabled CTest registrations and all25 new enabled-backend cases are retained; disabled-backend execution retains each explicit unavailable path and all four wallet service cases.

Qualification uses the final combined source in fresh enabled and disabled builds, real component and daemon checks, combined migration tests, and all linked project C++ instrumentation. Component qualification is not installed notification-provider or release readiness. Exact committed-prefix reorg recovery is explicit and leaves its source retained; wallet lifetime ownership does not establish wallet/session/key authority or chain-index lifetime. Mainnet activation remains unset.

The wallet-service reopen case also corrects primary-address lookup to the existing addresses.account/idx columns. The old column names failed to find the original index-zero address; the exact address preservation assertion is retained. Reorg readmission caps each reader page at its existing16MiB ceiling within the separate64MiB plan budget.

Linux download-drain qualification exposed a self-containment failure in util/hex.h after its new direct inclusion by the daemon. The header now includes cstdint for its existing uint8_t declarations. No encoding or decoding behavior changes.
