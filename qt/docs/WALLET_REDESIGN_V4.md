# Dinero-Qt Wallet Redesign — Contracts x Privacy Matrix

## Core Principle

**Contract semantics = one layer. Privacy/visibility = another layer. Not fused.**

A user makes two independent choices:
1. **What kind of transaction?** — Simple transfer or Contract
2. **How visible?** — Transparent, Confidential, or Private

## The Matrix

| | Transparent | Confidential | Private |
|-|-------------|-------------|---------|
| **Simple Transfer** | Normal payment. Everything visible. | Amount hidden. Sender/rules visible. | Amount + sender hidden. Ring-16. |
| **Contract** | Vault/timelock/escrow. Rules visible on-chain. Auditable. | Amount hidden. Rules visible. Auditable value-privacy. | Amount + sender + rules hidden. Full ZK. |

Six modes. Each has a real use case:

| Mode | Amount | Rules | Sender | Use Case | Tx Version |
|------|--------|-------|--------|----------|------------|
| Transparent transfer | visible | none | visible | Merchant payments, exchanges | v2 |
| Transparent contract | visible | visible | visible | Auditable vaults, public escrow, DAO treasuries | v2 (Taproot script-path) |
| Confidential transfer | hidden | none | visible | Private balances, salary | v2 (CT outputs) |
| Confidential contract | hidden | visible | visible | Corporate escrow: hide value, keep rules auditable | v2 (CT + Taproot script-path) |
| Private transfer | hidden | none | hidden | Personal wealth, donations | v3 (ring) |
| Private contract | hidden | hidden | hidden | Private vaults, hidden payroll, sealed escrow | v4 (ring-covenant + ZK) |

## Why This Matters

- **Businesses** want transparent contracts (auditable rules) with confidential amounts
- **Individuals** want private contracts (hidden everything)
- **DAOs** want transparent contracts with transparent amounts (full accountability)
- **Payroll** wants confidential contracts (hidden amounts, auditable distribution rules)

Forcing all contracts into "private" would lose the auditable use cases.

## Send Tab Design

```
┌─────────────────────────────────────────────────────┐
│  📤 Send                                            │
│                                                      │
│  Transaction Type:                                   │
│  ┌──────────────────┐  ┌──────────────────┐         │
│  │   💸 Transfer    │  │   📜 Contract    │         │
│  │   (selected)     │  │                  │         │
│  └──────────────────┘  └──────────────────┘         │
│                                                      │
│  Visibility:                                         │
│  ┌────────────┐ ┌──────────────┐ ┌─────────┐       │
│  │ 🌐 Public  │ │ 🔒 Confidential │ │ 🛡️ Private │   │
│  │ (selected) │ │              │ │         │       │
│  └────────────┘ └──────────────┘ └─────────┘       │
│                                                      │
│  Destination: [ din1... or dina1...             ]    │
│  Amount:      [ 1.0                        ] DIN     │
│                                                      │
│  Fee: 0.001 DIN                                      │
│  [ Send ]                                            │
└─────────────────────────────────────────────────────┘
```

When "Contract" is selected:

```
┌─────────────────────────────────────────────────────┐
│  📤 Send                                            │
│                                                      │
│  Transaction Type:                                   │
│  ┌──────────────────┐  ┌──────────────────┐         │
│  │   💸 Transfer    │  │   📜 Contract    │         │
│  │                  │  │   (selected)     │         │
│  └──────────────────┘  └──────────────────┘         │
│                                                      │
│  Visibility:                                         │
│  ┌────────────┐ ┌──────────────┐ ┌─────────┐       │
│  │ 🌐 Public  │ │ 🔒 Confidential │ │ 🛡️ Private │   │
│  │            │ │              │ │(selected)│       │
│  └────────────┘ └──────────────┘ └─────────┘       │
│                                                      │
│  ┌─ Contract Template ──────────────────────────┐   │
│  │                                               │   │
│  │  Template: [ 🔐 CTV Vault ▼ ]                │   │
│  │            ┌──────────────────────┐           │   │
│  │            │ 🔐 CTV Vault         │           │   │
│  │            │ ⏰ Time-Lock          │           │   │
│  │            │ 👥 Multi-Approval     │           │   │
│  │            │ 📅 Scheduled Payment  │           │   │
│  │            │ 🔧 Custom Script      │           │   │
│  │            └──────────────────────┘           │   │
│  │                                               │   │
│  │  Lock Duration: [ 24 hours ▼ ]                │   │
│  │                                               │   │
│  │  Privacy note:                                │   │
│  │  🛡️ Private: contract rules are hidden.       │   │
│  │  Only the ZK proof of satisfaction is visible.│   │
│  │                                               │   │
│  └───────────────────────────────────────────────┘   │
│                                                      │
│  Destination: [ dina1...                        ]    │
│  Amount:      [ 10.0                       ] DIN     │
│                                                      │
│  Fee: 0.002 DIN   Ring: 16   ZK proof: ~6KB         │
│  [ Send ]                                            │
└─────────────────────────────────────────────────────┘
```

## Visibility selector behavior

The visibility selector changes what's available:

| Visibility | Transfer RPC | Contract RPC | Address Format |
|-----------|-------------|-------------|----------------|
| Public | `wallet.sendtoaddress` | (Taproot script-path spend) | `din1...` |
| Confidential | `shieldcoins` + `sendconfidential` | (CT + Taproot script-path) | `dina1...` |
| Private | `sendprivate` | `sendprivatecovenant` | `dina1...` |

Some combinations may not be available yet:

| Combination | Status |
|-------------|--------|
| Public transfer | ✅ Shipping |
| Public contract | ✅ Shipping (Taproot covenants, height 20000) |
| Confidential transfer | ✅ Shipping (CT + ring, height 15000) |
| Confidential contract | 🟡 Possible (CT outputs + visible Tapscript) — wire existing covenant RPCs |
| Private transfer | ✅ Shipping (sendprivate, ring-16) |
| Private contract | ✅ Ready (sendprivatecovenant, v4, height 25000) |

## Wallet Tab — Balance Breakdown

```
┌──────────────────────────────────────────────────────┐
│  💰 Wallet                                           │
│                                                       │
│  Total Balance: 378,100.00 DIN                        │
│                                                       │
│  By Visibility:                                       │
│  🌐 Public:       377,800.00 DIN  (45 outputs)       │
│  🔒 Confidential:    200.00 DIN  (5 outputs)         │
│  🛡️ Private:         100.00 DIN  (3 outputs)         │
│                                                       │
│  Active Contracts:                                    │
│  📜 2 vaults (1 public, 1 private)                    │
│     Total locked: ~110.00 DIN                         │
│                                                       │
│  [ Shield → Confidential ] [ → Private ]              │
│  [ Unshield → Public ]     [ Create Vault ]           │
│                                                       │
└──────────────────────────────────────────────────────┘
```

## History Tab — Transaction Labels

| Icon | Label | What happened |
|------|-------|---------------|
| 🌐 | Sent / Received | Public transfer |
| 🌐📜 | Contract Created / Spent | Public contract (auditable) |
| 🔒 | Confidential Send / Receive | Confidential transfer |
| 🔒📜 | Confidential Contract | Confidential contract |
| 🛡️ | Private Send / Receive | Ring-anonymous transfer |
| 🛡️📜 | Private Contract | Ring-covenant (v4, ZK) |
| 🛡️→🌐 | Unshield | Private → Public |
| 🌐→🛡️ | Shield | Public → Private |

## Contract Management Tab

```
┌──────────────────────────────────────────────────────┐
│  📜 Contracts                                        │
│                                                       │
│  Active:                                              │
│  ┌──────────────────────────────────────────────────┐│
│  │ 🌐 Public CTV Vault                              ││
│  │ Amount: 50.00 DIN       Block: 20650             ││
│  │ Type: Time-lock (48h)   Status: 🟡 Locked (36h)  ││
│  │ Rules: visible on-chain                           ││
│  │ [ Withdraw (in 12h) ]                             ││
│  └──────────────────────────────────────────────────┘│
│                                                       │
│  ┌──────────────────────────────────────────────────┐│
│  │ 🛡️ Private CTV Vault                             ││
│  │ Amount: confidential    Block: 20665             ││
│  │ Type: CTV Vault         Status: 🟢 Spendable     ││
│  │ Rules: hidden (ZK proof)                          ││
│  │ [ Withdraw ]                                      ││
│  └──────────────────────────────────────────────────┘│
│                                                       │
│  [ + New Contract ]                                   │
│                                                       │
└──────────────────────────────────────────────────────┘
```

## Implementation Phases

### Phase 1: Rename + Layout (no new features)
- "Standard" → "Public", "Confidential" → remove as user-facing term
- Add visibility selector (Public / Confidential / Private) to Send
- Add transaction type selector (Transfer / Contract) to Send
- Contract section hidden until "Contract" selected
- Disable unavailable combinations (grey out with tooltip)
- **Time: ~4 hours**

### Phase 2: Wire existing RPCs
- Public transfer → `wallet.sendtoaddress` (already works)
- Public contract → existing covenant RPC (Taproot script-path)
- Confidential transfer → `shieldcoins` / `sendconfidential`
- Private transfer → `sendprivate`
- Private contract → `sendprivatecovenant`
- **Time: ~4 hours**

### Phase 3: Contract templates
- CTV Vault template: duration picker → script builder
- Time-lock template: block height or timestamp
- Multi-approval template: N-of-M key entry
- Custom script: hex input with validation
- **Time: ~8 hours**

### Phase 4: Contract management
- List active contract UTXOs from wallet
- Show type, status, lock conditions
- Withdrawal flow (construct + broadcast the spending tx)
- **Time: ~12 hours**

### Phase 5: History + filters
- Detect v2/v3/v4 in transaction history
- Show appropriate icons and labels
- Filter by visibility and transaction type
- **Time: ~4 hours**

## Design Rules

1. **Two independent axes** — Transaction type (transfer/contract) and visibility (public/confidential/private) are separate choices.

2. **Users never see "v2/v3/v4"** — those are implementation details. Users see "Public Transfer" or "Private Vault."

3. **Contracts don't imply privacy** — a public vault is just as valid as a private vault. The user chooses.

4. **Private means hidden rules** — in Private mode, the contract logic is proven in ZK. Nobody sees the script. In Public/Confidential mode, the rules are on-chain and auditable.

5. **"Confidential" means hidden amounts only** — the sender identity and contract rules are still visible. This is the middle ground between public and private.

6. **Grey out what's not ready** — Confidential contracts are technically possible but not yet wired. Show the option greyed out with "Coming soon" tooltip rather than hiding it.
