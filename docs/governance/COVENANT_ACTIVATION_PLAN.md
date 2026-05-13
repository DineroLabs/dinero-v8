# Covenant Activation Plan - Network Deployment Governance

**Document Type:** MANDATORY NETWORK GOVERNANCE
**Status:** PENDING ACTIVATION
**Authority:** Network Consensus Rules
**Last Updated:** 2025-12-24

---

## CRITICAL: This is NOT Optional

This document defines **when and how** covenant opcodes become enforceable on mainnet. Failure to follow this plan will result in **chain splits** and **loss of funds**.

---

## 1️⃣ Activation Semantics

### Activation Method: HEIGHT-BASED SOFT FORK

**Decision:** Covenants activate at a specific block height via soft fork.

**Rationale:**
- **Soft fork:** Stricter validation rules (tightening consensus)
- **Height-based:** Deterministic, not dependent on wall-clock time
- **Gradual:** Gives operators time to upgrade before enforcement

### Activation Parameters

```
COVENANT_ACTIVATION_HEIGHT = TBD (To Be Determined by network operators)

Recommended: Current tip + 2016 blocks (~2 weeks at 10-min blocks)
Example: If current height = 850000, activation = 852016
```

**Before Activation Height:**
- Covenant opcodes: NOT ENFORCED
- Transactions with covenant opcodes: ACCEPTED but not validated
- Node behavior: Legacy validation only

**At Activation Height:**
- Covenant opcodes: FULLY ENFORCED
- All nodes MUST validate covenant transactions
- Invalid covenant transactions: REJECTED

**After Activation Height:**
- Covenant opcodes: PERMANENTLY ENFORCED
- No rollback possible (consensus rule)
- Network assumes all nodes validate covenants

---

## Activation States

### State 1: PRE-ACTIVATION (Current)
**Block Height:** < COVENANT_ACTIVATION_HEIGHT
**Covenant Validation:** DISABLED
**Node Requirement:** None (upgrade recommended)

**Behavior:**
- Covenant transactions accepted into mempool
- Block validation does NOT enforce covenant rules
- Users CAN create covenant UTXOs (but should not yet)

**Risk:** Creating covenant UTXOs before activation = **FUNDS AT RISK**
- Old nodes: Accept without validation
- Post-activation: May be invalid and unspendable

**WARNING TO USERS:**
```
DO NOT create covenant transactions before activation height.
Wait until network confirms activation.
```

### State 2: ACTIVATION HEIGHT
**Block Height:** = COVENANT_ACTIVATION_HEIGHT
**Covenant Validation:** ENABLED
**Node Requirement:** MUST be upgraded

**Behavior:**
- First block where covenant rules are enforced
- All covenant transactions validated against consensus rules
- Invalid covenant transactions rejected

**Critical:** Miners MUST upgrade before this height or risk mining invalid blocks.

### State 3: POST-ACTIVATION (Mainnet)
**Block Height:** > COVENANT_ACTIVATION_HEIGHT
**Covenant Validation:** ENFORCED PERMANENTLY
**Node Requirement:** MUST validate covenants

**Behavior:**
- All covenant opcodes active and enforced
- Users can safely create covenant transactions
- Network assumes covenant validation

---

## 2️⃣ Node Upgrade Requirements

### MANDATORY Upgrade Timeline

**T-2016 blocks (~2 weeks before activation):**
- ⚠️ **FINAL WARNING**: Operators must upgrade NOW
- Node operators: Download and install upgraded software
- Miners: Test covenant validation
- Exchanges: Prepare for covenant transaction support

**T-1008 blocks (~1 week before activation):**
- ⚠️ **CRITICAL**: Last chance to upgrade
- Non-upgraded nodes: Will diverge at activation height
- Miners: Switch to upgraded software

**T-0 (Activation Height):**
- ✅ **COVENANTS ACTIVE**: All nodes must validate covenants
- Non-upgraded nodes: WILL FORK OFF (consensus divergence)

### Upgrade Checklist for Node Operators

```
[ ] Download DineroCoin version X.X.X or later
[ ] Verify release signatures (if available)
[ ] Test on testnet first
[ ] Backup wallet and blockchain data
[ ] Stop node
[ ] Install upgraded software
[ ] Start node
[ ] Verify sync to current tip
[ ] Verify covenant validation enabled:
    dinero-cli getblockchaininfo | grep "covenants"
[ ] Monitor logs for covenant-related messages
```

### Upgrade Checklist for Miners

```
[ ] Upgrade node software (see above)
[ ] Test mining on testnet
[ ] Verify block templates include covenant validation
[ ] Test covenant transaction acceptance/rejection
[ ] Switch mainnet mining to upgraded software
[ ] Monitor for invalid block warnings
[ ] Ensure pool software supports covenant transactions
```

### Upgrade Checklist for Exchanges

```
[ ] Upgrade nodes (all: deposit, withdrawal, monitoring)
[ ] Update deposit address generation (if using covenant scripts)
[ ] Test covenant transaction processing
[ ] Update withdrawal validation
[ ] Monitor for covenant-related deposit issues
[ ] Prepare customer support for covenant questions
```

---

## 3️⃣ Activation Signaling (Optional Enhancement)

**Current Plan:** No signaling (simple height activation)

**Alternative (BIP9-style):**
If network coordination is needed, implement miner signaling:
- Miners signal readiness via block version bits
- Activation occurs when 95% of last 2016 blocks signal support
- Timeout: Activation fails if not reached by height X

**Recommendation:** Start with simple height activation. Add signaling only if coordination issues arise.

---

## 4️⃣ Emergency Procedures

### Scenario: Critical Bug Found Before Activation

**Action:**
1. **IMMEDIATELY** announce activation delay
2. Publish bug details (responsible disclosure)
3. Release patched software
4. Set new COVENANT_ACTIVATION_HEIGHT (current + 4032 blocks minimum)
5. Coordinate with miners and exchanges

**Communication Channels:**
- GitHub release notes
- Discord/Telegram announcements
- Email to known operators
- Website banner

### Scenario: Network Split at Activation

**Symptoms:**
- Multiple competing chains at activation height
- Hashrate split between forks

**Action:**
1. Identify cause (software bug vs upgrade failure)
2. If bug: Emergency patch release
3. If upgrade failure: Coordinate with miners to switch
4. Monitor for longest chain convergence
5. Post-mortem analysis

**Critical:** Exchanges MUST halt deposits/withdrawals until convergence.

---

## 5️⃣ User Protection Measures

### Before Activation Height

**DO NOT:**
- ❌ Create covenant transactions on mainnet
- ❌ Send funds to covenant addresses
- ❌ Assume covenant validation is active

**DO:**
- ✅ Wait for activation confirmation
- ✅ Test on testnet
- ✅ Monitor activation status: `dinero-cli getblockchaininfo`

### At Activation Height

**Wait for Confirmation:**
- Activation height reached: Wait 6 confirmations minimum
- Verify no chain split: Check multiple block explorers
- Confirm covenant validation active

### After Activation Height

**Safe to Use Covenants:**
- ✅ Create covenant transactions
- ✅ Use CTV vaults
- ✅ Build covenant-based contracts
- ✅ Develop covenant applications

---

## 6️⃣ Communication Plan

### Pre-Activation Announcements (T-2016 blocks)

**Message to Node Operators:**
```
MANDATORY UPGRADE REQUIRED

Covenant soft fork activates at block XXXXXX (approximately 2 weeks).

Action Required:
1. Upgrade to DineroCoin vX.X.X or later
2. Restart your node
3. Verify sync completes

Failure to upgrade will result in consensus divergence.
Your node will fork off the network.

Download: https://github.com/dinerocoin/releases
```

**Message to Miners:**
```
CRITICAL: Covenant Activation in 2 Weeks

Block Height: XXXXXX
Estimated Date: YYYY-MM-DD

Miners MUST upgrade before activation or risk mining invalid blocks.

Upgraded software enforces:
- OP_CHECKTEMPLATEVERIFY
- OP_CHECKSIGFROMSTACK
- OP_TXHASH
- OP_CHECKCONTRACTVERIFY

Test on testnet before switching mainnet mining.
```

**Message to Users:**
```
Covenant Soft Fork Activation Notice

DineroCoin will activate covenant opcodes at block XXXXXX.

What this means:
- Advanced smart contract features become available
- No action required if you're not using covenants
- DO NOT create covenant transactions until activation completes

Users creating covenant transactions before activation risk loss of funds.
```

### Activation Day Announcements (T-0)

**Message:**
```
COVENANTS NOW ACTIVE

Block XXXXXX has been mined.
Covenant opcodes are now enforced on mainnet.

Users can now safely:
- Create covenant transactions
- Use CTV vaults
- Build covenant-based applications

Verify activation: dinero-cli getblockchaininfo
```

### Post-Activation Monitoring (T+1008 blocks)

**Message:**
```
Covenant Activation: 1 Week Stability Report

Activation Height: XXXXXX
Blocks Since Activation: 1008
Network Consensus: STABLE

Statistics:
- Covenant transactions processed: X
- Invalid covenant txs rejected: Y
- Chain splits: NONE
- Miner participation: 100%

Covenant features are stable and production-ready.
```

---

## 7️⃣ Configuration Options

### Node Configuration (`dinero.conf`)

```ini
# Covenant activation (DO NOT CHANGE - consensus rule)
covenant_activation_height=XXXXXX

# Testing only - allows covenant validation before activation
# WARNING: Only for testnet/regtest. DO NOT use on mainnet.
# early_covenant_activation=1

# Mempool policy - accept covenant txs before activation (risky)
# Default: 0 (reject covenant txs until activation)
# accept_covenant_txs_pre_activation=0
```

### RPC Commands

**Check Activation Status:**
```bash
dinero-cli getblockchaininfo

Response:
{
  "chain": "main",
  "blocks": 851000,
  "covenants": {
    "activation_height": 852016,
    "status": "pre-activation",  // or "active"
    "blocks_until_activation": 1016
  }
}
```

**Check if Covenant Transaction is Valid:**
```bash
dinero-cli testcovenantvalidation <hex_transaction>

Response:
{
  "valid": true,
  "covenant_opcodes_used": ["OP_CHECKTEMPLATEVERIFY"],
  "validation_flags": "SCRIPT_VERIFY_STANDARD | SCRIPT_VERIFY_COVENANTS"
}
```

---

## 8️⃣ Rollback Policy

### Can Covenants Be Deactivated?

**Answer:** NO (once activated, permanent)

**Rationale:**
- Soft forks are forward-compatible, not backward-compatible
- Deactivation would require hard fork (network split)
- Users depend on covenant immutability

**Emergency Bug Response:**
- Patch the bug (preserve semantics if possible)
- If semantics change required: New soft fork with bug fix

---

## 9️⃣ Testnet Activation

**Testnet Activation Height:** ALREADY ACTIVE (for testing)

**Purpose:**
- Allow developers to test covenant transactions
- Verify node upgrade procedures
- Practice covenant application development

**Testnet Configuration:**
```ini
# testnet.conf
covenant_activation_height=0  # Active from genesis on testnet
```

---

## 🔟 Deployment Checklist

**Before Setting Activation Height:**
- [ ] All Phase L0, 2, 3, 4 complete
- [ ] Security audit passed (38/38 adversarial tests)
- [ ] Code frozen (no semantic changes)
- [ ] Testnet stable for 1 month minimum
- [ ] Documentation complete
- [ ] Operator communication plan ready
- [ ] Emergency response plan in place

**Setting Activation Height:**
- [ ] Calculate: current_tip + 2016 blocks
- [ ] Publish activation height in release notes
- [ ] Update node configuration defaults
- [ ] Announce to operators (T-2016)
- [ ] Monitor upgrade progress
- [ ] Final reminder (T-1008)

**At Activation:**
- [ ] Verify activation block mined
- [ ] Monitor for chain splits (none expected)
- [ ] Confirm covenant validation active
- [ ] Announce success
- [ ] Monitor stability (1 week)

**Post-Activation:**
- [ ] Publish stability report
- [ ] Document any issues encountered
- [ ] Update user guides for covenant usage
- [ ] Begin Layer-2 integration (Lightning, etc.)

---

## Summary

**Activation Method:** Height-based soft fork
**Operator Action Required:** MANDATORY upgrade before activation
**User Protection:** Do not create covenant txs until activation
**Rollback Policy:** Not possible (permanent consensus change)
**Emergency Plan:** Delay activation if critical bug found

**Timeline Example:**
```
Block 850000 (Current):  Pre-activation, upgrade available
Block 851000 (T-1008):   Final warning, must upgrade
Block 852016 (T-0):      COVENANTS ACTIVATE
Block 853024 (T+1008):   Stability report, production-ready
```

---

**This document is mandatory reading for all network operators.**

Failure to upgrade before activation will result in consensus divergence and potential loss of mining rewards or funds.

**Status:** DRAFT - Awaiting activation height selection by network governance

**Next Action:** Set COVENANT_ACTIVATION_HEIGHT and announce to network
