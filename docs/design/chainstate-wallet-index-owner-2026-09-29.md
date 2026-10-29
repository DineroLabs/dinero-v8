# Chainstate wallet-index lifetime ownership

Locally qualified.

The thread-affine WalletIndexUse retains the actual chainstate service and exact initialized UTXOIndex for a synchronous operation. It prevents Stop from destroying that index while the operation finishes. Stop refuses same-thread active ownership or an already-held selected-chain lock before effects, closes new independent acquisition, drains existing ownership, then runs the existing shutdown. Nested acquisition on an already owned thread can finish during drain. Failed shutdown retains closed admission and permits a later retry. Init cannot replace a present index.

WalletService Start, existing script binding and snapshot recovery now hold this index owner across their synchronous operations. Binding/recovery require the wallet's existing index pointer to equal the retained source index. No pointer repair, wallet selection, baseline adoption or readiness acknowledgment is inferred.

The new owner protects index lifetime only. It does not freeze chain state or preserve externally owned ChainDB/BlockStorage through teardown. Legacy raw index access, concurrent Init/Start, other chainstate operations and worker shutdown still require the established daemon order. It does not install a production notification provider or establish complete authenticated inventory. An initialized but never started service still retains its index until destruction after Stop closes acquisition, matching the existing index destruction scope. Mainnet remains unset.

Planned validation includes four sequential patched-path component cases and the actual full/CSN daemon service-release check. No race, unsafe-original or synchronization-removal controls are used.

The wallet-service shutdown waiter likewise rechecks the retained manager after another stop attempt finishes, so a prerequisite failure can be retried rather than treating the end of that attempt as completed shutdown. This does not add a concurrent failure reproduction claim.
