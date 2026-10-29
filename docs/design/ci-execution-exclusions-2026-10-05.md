# Explicit CI execution exclusions

`GenerationToctouBarrier` and `RestartChurnBoringnessGate` are explicitly
excluded from execution. Their test bodies, registrations, assertions and
deadlines remain unchanged. Exclusion is not passing qualification and provides
no evidence for their race, synchronization or churn scenarios.

The full-test serial loop no longer selects either control. The Orchard CSN
proof receipt lane selects its four remaining tests and requires exactly those
four enabled registrations and four completed CTests. Its receipt assertion
is unchanged. The dynamic real-mempool selector refuses any excluded name
before writing its selection; it cannot silently discard a test and claim a
complete inventory.

The coverage gate keeps exact-name execution policy separate from its existing
unexecuted-test debt baseline. It reports policy exclusions as `NOT RUN`, never
as executed tests. A modeled lane selecting either excluded test fails, even
in another build directory, before `--explain`, `--update` or secondary-build
scoping can bypass the check. Policy names cannot be inserted into the debt
baseline. Unrelated new omissions, revived baseline entries and stale entries
continue to fail. No existing debt baseline is enlarged.

The Python self-tests use synthetic inventories and prohibit child processes.
They do not launch CTest or either excluded control. The generated-selector
test exercises the actual workflow guard with names only. Static selection
analysis proves which modeled names a command selects; it does not prove test
execution, a passing Linux run, or arbitrary transitive shell safety. Review
the complete workflow and script surface before resuming publication or CI.
