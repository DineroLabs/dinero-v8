# Lightning Network Module

This directory contains Lightning Network protocol components that are
**separate from the on-chain wallet**.

## Architecture

```
src/lightning/
├── keys/
│   ├── lightning_key_deriver.cpp   # BIP32-based key derivation
│   └── (future: key rotation, etc.)
│
├── (future: channel/, htlc/, gossip/, etc.)
│
└── README.md
```

## Why Lightning Keys Are Separate From Wallet

Lightning keys are fundamentally different from wallet keys:

| Property | Wallet Keys | Lightning Keys |
|----------|-------------|----------------|
| UTXO-backed | Yes | No |
| Spendable via wallet | Yes | No |
| Subject to ownership invariants | Yes | No |
| Imported/exported as addresses | Yes | No |
| Purpose | On-chain ownership | Protocol state machine |

## Key Derivation Paths

All Lightning keys derive from the wallet seed using BIP32, but use
dedicated chain indices (3-7) that are never registered as wallet addresses:

```
Channel Keys (per-channel):
  m/84'/1448'/0'/3/idx  - Funding keys (MuSig2)
  m/84'/1448'/0'/4/idx  - Revocation base keys
  m/84'/1448'/0'/5/idx  - Payment base keys
  m/84'/1448'/0'/6/idx  - Delayed payment base keys
  m/84'/1448'/0'/7/idx  - HTLC base keys

Node Identity (all hardened):
  m/84'/1448'/9735'/account'/key'

Revocation Secrets (HMAC-based):
  HMAC-SHA256(seed, "dinero-lightning-revocation" || channel_id)
```

## Usage

```cpp
#include "lightning/keys/lightning_key_deriver.h"

// Create deriver from wallet seed
dinero::lightning::LightningKeyDeriver deriver(seed, 64, DINERO_COIN_TYPE);

// Get channel keys
auto funding_key = deriver.GetFundingKey(channel_index);
auto revocation_key = deriver.GetRevocationBaseKey(channel_index);

// Get node identity
auto identity = deriver.GetNodeIdentity();
// identity.privkey, identity.pubkey
```

## Security

- All intermediate key material is zeroized via `OPENSSL_cleanse`
- Seed is zeroized on `LightningKeyDeriver` destruction
- Keys are deterministic and recoverable from mnemonic

## Future: Standalone Lightning Daemon

When the Lightning daemon is extracted to its own repository:

1. Move `src/lightning/` directory to the standalone repo
2. Keep the same `ILightningKeyProvider` interface
3. Keep the same derivation paths (test vector compatibility)
4. Zero code rewrite required
