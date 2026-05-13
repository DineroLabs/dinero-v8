# Branch Protection Rules

## Required Status Checks for `main` branch

The following CI jobs must pass before merging PRs to `main`:

### Required Checks
- **A2Z End-to-End Validation** (all matrix combinations)
  - `a2z (ubuntu-latest, off)`
  - `a2z (ubuntu-latest, asan-ubsan)`
  - `a2z (macos-latest, off)`
  - `a2z (macos-latest, asan-ubsan)`

### Additional Protection Rules
- Require branches to be up to date before merging
- Require pull request reviews before merging
- Dismiss stale reviews when new commits are pushed
- Require conversation resolution before merging

## Required Status Checks for `develop` branch

The following CI jobs must pass before merging PRs to `develop`:

### Required Checks
- **A2Z End-to-End Validation** (at least one matrix combination)
  - `a2z (ubuntu-latest, off)` (minimum requirement)

## Failure Indicators (Auto-fail)

The A→Z job will automatically fail if any of these are detected:

### Code Quality
- Duplicate RPC method registrations
- Placeholder script usage beyond premine
- Legacy symbols in vNext-only builds

### Runtime Issues
- Unhandled exceptions or crashes
- SIGABRT or terminate calls
- Memory corruption (ASan/UBSan)

### Address API
- Invalid Bech32 decoding
- HRP mismatches
- Mixed-case acceptance
- Bech32m acceptance (witver >= 1)

### PSBT/Wallet
- PSBT round-trip failures
- UTXO placeholder detection
- Wallet-chainstate misalignment

## Manual Setup

To enable these protections in GitHub:

1. Go to Settings → Branches
2. Add rule for `main` branch
3. Enable "Require status checks to pass before merging"
4. Select all A→Z matrix jobs as required
5. Enable "Require branches to be up to date before merging"
6. Enable "Require pull request reviews before merging"

## Emergency Override

In case of emergency, branch protection can be temporarily disabled by:
1. Repository admin access
2. Temporarily disabling the rule
3. Merging the PR
4. Re-enabling the rule immediately

**Note**: This should only be used for critical security fixes or hotfixes.
