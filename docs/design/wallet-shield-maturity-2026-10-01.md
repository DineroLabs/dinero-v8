# Shield coinbase maturity boundary

Preparation only: two cases written, not compiled or executed. Depends on the frozen shield reservation/normal inventory recovery implementation. Existing production, fixtures and deadlines are unchanged.

The fixture mines an actual coinbase at height 103 to an existing wallet-issued BIP86 address, advances the real selected chain, and calls normal canonical wallet recovery to discover the coin and advance enrolled accounts. It does not synthesize height/maturity metadata or insert that coin with addUTXO. At selected parent 201, both queued and immediate reservation must refuse the precise immature-coin cause with unchanged wallet snapshots and no proof job. At selected parent 202, the actual owner/prover/signer must retain Ready, reopen with identical bytes, admit and mine at height 203, then recover 20,000 una into account 17.

The hundred-block threshold is tested literally at 99/100 next-block age. This is a deterministic isolated chain boundary, not churn/race/synchronization-removal testing. No deployment, activation or production datadir changes. Full ON/OFF source and linked-project sanitizer qualification, actual runtime duration, required independent CI markers and successful source-proved wallet recovery remain unverified. Do not claim completeness from written assertions or substitute direct account-only replay for the normal recovery assertions.

A future independent CI lane requires the enabled 180-second registration and both exact execution markers, and retains the inventory and execution log. The workflow is prepared only. Public publication and dispatch remain held because existing unrelated selectors include excluded tests; this addition does not remove or waive any existing gate.
