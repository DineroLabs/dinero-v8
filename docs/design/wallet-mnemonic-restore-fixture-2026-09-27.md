# Authoritative mnemonic restore integration contract

A newly started destination daemon already owns its default wallet. The authoritative-mnemonic integration test now requires an attempt to restore over that name to fail and verifies that its exported recovery identity and address inventory remain unchanged.

The test then restores the source mnemonic under the new name recovered. All existing checks for exact multi-index external addresses, the twenty-address change window, authoritative export, backup acknowledgment and source restart remain. After restoration, it switches back to the destination default wallet and verifies its original identity and addresses, then reopens the recovered wallet and verifies its source mnemonic.

Production restore behavior, registrations, timeout and daemon readiness waits are unchanged. This implements the existing new-name-only product contract in the daemon integration fixture. It does not qualify imported-key/account backup completeness, production deployment or whole-node recovery. Exact execution evidence is retained privately.

The root Orchard workflow independently requires and executes this existing daemon test and preserves its enabled inventory and completion log. Existing root selectors remain unchanged; this adds one executed CTest only when its actual run is verified.
