# DineroCoin Notarization Records

This file contains cryptographic notarization records for critical chain events.

---

## Premine Block Re-Mining (January 2025) - FINAL

```
DineroCoin Premine Block 1 Notarization Record (FINAL)
Date: 2025-11-01T19:20:44.009089
Block Height: 1
Block Hash: 0000002bd3fa677b43cc4efcb82078db53131f191ad72afb9d463ddd33c7f79a
Nonce: 0x0007d3e6 (512,998)
Merkle Root: 2501ed56dda21d4f23bc510f0447f3674dd42ca1d1d8b244aa2bf1c988786640
Premine Amount: 2,627,900 DIN (262,790,000,000,000 una)
Previous Block: 173fe6da2ccc8a671380c8845a2c15e70cc8b84132fa2ab61108425c85412a33
Timestamp: 1760472513 (2025-10-14T16:08:33Z) - Genesis time (1760472333) + 180s
Timestamp Correction: Fixed from 1700000154 (2023) to satisfy consensus rule block.nTime > medianTimePast
Consensus Checksum: ff279196e33c326f61191c368726f924c6728e5d36c227844195e653650ef430
Mining Tool: tools/premine_reminer.py
Verification Tool: tools/premine_verify.py
Status: VERIFIED AND READY FOR DEPLOYMENT - FINAL CONSENSUS-SAFE PREMINE
```

### Notarization Hash (SHA256)

`edce1e9de79441250ea78fa6c2e2e71432c879d09e8b2919dfb372b7b46ec9e2`

This hash can be independently verified by:
```bash
echo 'DineroCoin Premine Block 1 Notarization Record (FINAL)
Date: 2025-11-01T19:20:44.009089
Block Height: 1
Block Hash: 0000002bd3fa677b43cc4efcb82078db53131f191ad72afb9d463ddd33c7f79a
Nonce: 0x0007d3e6 (512,998)
Merkle Root: 2501ed56dda21d4f23bc510f0447f3674dd42ca1d1d8b244aa2bf1c988786640
Premine Amount: 2,627,900 DIN (262,790,000,000,000 una)
Previous Block: 173fe6da2ccc8a671380c8845a2c15e70cc8b84132fa2ab61108425c85412a33
Timestamp: 1760472513 (2025-10-14T16:08:33Z) - Genesis time (1760472333) + 180s
Timestamp Correction: Fixed from 1700000154 (2023) to satisfy consensus rule block.nTime > medianTimePast
Consensus Checksum: ff279196e33c326f61191c368726f924c6728e5d36c227844195e653650ef430
Mining Tool: tools/premine_reminer.py
Verification Tool: tools/premine_verify.py
Status: VERIFIED AND READY FOR DEPLOYMENT - FINAL CONSENSUS-SAFE PREMINE
' | sha256sum
```
