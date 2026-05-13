# Dinero Branch Layout

## Overview

This document describes the branch structure for the Dinero project ecosystem.

## Branch Hierarchy

```
main                    ← Sacred ground (consensus only)
│
├── infra/stratum       ← Stratum mining server integration
├── ui/dinero-qt        ← Desktop wallet GUI integration  
└── lightning-main      ← Lightning Network (L2)
```

## Branch Policies

### `main` - Consensus Only

**What belongs here:**
- dinerod daemon core
- Consensus rules
- Block validation
- P2P networking
- Minimal RPC (for node operation)
- Wallet backend (BIP32/39/86)

**What does NOT belong here:**
- GUI code
- Stratum protocol
- Lightning Network
- Pool accounting
- Experimental features

**Merge policy:**
- Requires review
- Must pass all consensus tests
- No breaking changes to existing behavior

### `infra/stratum` - Mining Infrastructure

**Purpose:** Track integration points between daemon and stratum server.

**Sibling project:** `~/src/stratum/`

**What belongs here:**
- Stratum V1 protocol changes
- Mining RPC interface changes
- Pool compatibility testing
- SSL/TLS for miners

**Merge policy:**
- Never touches consensus code
- Can evolve independently
- Rebased against main as needed

### `ui/dinero-qt` - Desktop GUI

**Purpose:** Track RPC interface changes needed by GUI.

**Sibling project:** `~/src/dinero-qt/`

**What belongs here:**
- RPC additions for wallet UI
- Display-only data endpoints
- User preference storage
- Platform-specific packaging

**Merge policy:**
- RPC only (no consensus changes)
- Can change frequently
- UI bugs isolated from node

### `lightning-main` - Layer 2

**Purpose:** Lightning Network protocol implementation.

**Sibling project:** `~/src/lightning/`

**What belongs here:**
- Payment channels
- HTLC logic
- Channel state management
- Routing

**Merge policy:**
- Long-lived branch
- Complex protocol changes
- Thorough review required

## Project Layout

```
~/src/
├── dinero/          # C++ daemon (this repo)
│   └── main         # Consensus-only code
│
├── dinero-qt/       # Desktop wallet GUI (separate repo)
│   └── main         # Qt6 application
│
├── dinero-rust/     # Rust tier-3 verifier (separate repo)
│   └── main         # Light client verification
│
├── stratum/         # Mining server (separate repo)
│   └── main         # Stratum V1 protocol
│
├── lightning/       # Lightning Network (separate repo)
│   └── main         # L2 protocol
│
└── third_party/     # Shared dependencies
```

## Rules

### DO

- Keep `main` pristine (consensus only)
- Use feature branches for experiments
- Create PRs for review before merging
- Test consensus changes exhaustively

### DON'T

- Merge GUI changes to main
- Share feature flags between branches
- Treat branches as temporary
- Skip review for "small" changes

## Integration Workflow

When sibling projects need daemon changes:

1. Create feature branch from relevant infra/ui branch
2. Make minimal RPC/interface changes
3. PR to the infra/ui branch
4. Test integration
5. Cherry-pick consensus-safe changes to main (if any)

## Branch Protection (Recommended)

```yaml
# .github/settings.yml
branches:
  main:
    protection:
      required_reviews: 2
      required_checks:
        - consensus-tests
        - asan-ubsan
      enforce_admins: true
      
  infra/stratum:
    protection:
      required_reviews: 1
      required_checks:
        - stratum-tests
        
  ui/dinero-qt:
    protection:
      required_reviews: 1
      required_checks:
        - build-qt
```

## Summary

| Branch | Purpose | Frequency | Risk |
|--------|---------|-----------|------|
| `main` | Consensus | Rare | High |
| `infra/stratum` | Mining | Medium | Low |
| `ui/dinero-qt` | GUI | Frequent | None |
| `lightning-main` | L2 | Medium | Medium |

Branches are your **blast-radius control**.
