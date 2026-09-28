# Roll back incomplete mempool admission publication

After validation and policy checks, actual admission prepares a complete copy of the existing pool state before making its first change. The owner includes transaction entries, spent-output tracking, fee/time/reverse-dependency indexes, template exclusions, the coin overlay and admission/removal counters. If publication or local eviction throws, nonthrowing swaps restore the prior pool while the existing pool lock still excludes readers. Copy failure happens before any pool mutation.

Local eviction completes before the commit point. Fee-estimator, scanner and external callback observations occur after that point; an observer exception retains the published admission, so the wallet must retain its signed pending owner and reservations for an ambiguous outcome. Test-only admission returns before constructing the owner.

Three patched-path fixtures inject ordinary C++ exceptions through a fixture logger during insertion, replacement and local eviction, then check entries, fee/dependency state, exclusions, spent outputs, overlay and retry. No unsafe original, synchronization-removal, allocation-exhaustion or exploit reproduction is run. These checks do not establish every allocator failure or rollback for other mutation APIs.

The full-state copy adds memory and CPU cost proportional to the pool. Load/resident qualification remains open, as do global observer ownership, durable all-consumer delivery, Orchard admission/readmission/relay/mining and release readiness. Mainnet activation remains unset.
