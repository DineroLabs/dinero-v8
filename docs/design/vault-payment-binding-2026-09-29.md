# Vault retained payment references

Locally qualified in the combined durable-vault batch. 

Every present WithdrawalPaymentRetained now requires its matching authenticated wallet payment owner during vault OpenExisting and before Stage writes a successor. The vault state, full wallet payment envelope and bound history rows are read inside the same checked FULL SQLite transaction with the same selected wallet lease and recovery-seed owner. A new read-only lease method participates in that caller transaction without beginning, committing, rolling back, writing or copying another seed.

The check binds the exact vault/request identity, explicit fee/audit terms, recorded network address, single recipient amount/script, signed body, txid, actual output index, SHA256 and fee. Missing, malformed, unreadable or mismatched owners refuse before state writes or service publication. An empty/missing payment envelope cannot justify a retained state. Pending/Signing states remain explicit recovery boundaries; missing bodies never authorize generation. This does not establish wallet/vault catalog or backup/deletion completeness, signing-state adoption, canonical inclusion, cancellation, fee accounting or readiness.

Four mandatory patched component cases cover actual retained payment/reopen without writes; malformed/missing/denied wallet owner refusing publication; replay-valid candidate body hash/fee/term mismatches refusing Stage with original ciphertext preserved; and one existing seed/caller transaction retained through the new reader. All existing fixtures and deadlines are preserved. Fresh full ON/OFF and both complete linked-project sanitizer graphs passed the combined qualification. Mainnet remains unset; production provider/runtime installation is still open.

## Combined qualification scope

This change was locally qualified together with wallet observation, durable state, retained withdrawals, dispatch lifetime, payment binding, runtime restore refusal, and reservation metrics. The earlier per-component pipeline was superseded before source application. Locally qualified in the combined durable-vault batch. The combined lane retains every case and deadline: fresh full ON/OFF declared targets,119/114 component CTests plus10daemon and2combined each, and separate full linked-project replay155 and wallet-RPC50 ASan/UBSan case groups. No per-component intermediate-source qualification is implied. Production durable runtime attachment and remaining release gates stay open.
