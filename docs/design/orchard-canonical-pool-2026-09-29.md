# Canonical Orchard pool publication

The typed canonical connect/disconnect path retains the configured mempool service and prepares the existing reversible pool/bridge/relay update after complete chainstate preparation. Connect extracts confirmed IDs, exact transparent inputs and every Orchard action nullifier from the validated mixed body; disconnect selects the exact parent root. The durable chainstate write and active tip precede nonthrowing local pool/cache publication. Remaining provider notifications follow. Outbound proof refresh failures leave the committed canonical result intact.

The existing RuntimeBlockNotifications provider remains required and owns the remaining consumers and durable reorg/readmission plan. It must not duplicate the local pool/cache update now owned by ChainstateService. A standalone service with no configured daemon pool remains a scoped component; configured bridge/relay without a pool refuses. Service ownership mismatch or stopped pool refuses before canonical durability.

Four patched-path component cases exercise successive actual shield/historical blocks and an ambiguous postcommit refresh failure, precommit pool-preparation refusal/retry, wrong context/provider refusal, and disconnect/database reopen/explicit readmission/reconnect. No unsafe-original controls. Production provider installation, automatic reorg readmission, whole-node/process/power-loss recovery, wallet completion, Orchard note send/unshield, load and release qualification remain open. Mainnet remains unset.

## Test registration

The original replay CTest selects its replay, selected-wallet-history and origin-projection suites. Newer suites keep their separate CTest registrations and existing deadlines. CI enumerates every compiled case in this binary, checks that exactly one enabled CTest owns it, and requires each case's successful execution marker. This removes duplicate execution that caused the original unfiltered CTest to exceed its 120-second deadline on Linux. Operational-refusal fixtures observe the existing activation retry cooldown before trying the retained body again.
