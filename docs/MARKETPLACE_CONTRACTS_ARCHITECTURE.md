# Marketplace Contracts Architecture - UTXO-Compatible State Management

**Week 7: Complex Feature Implementation** 🏗️

## 🎯 Challenge: Bitcoin UTXO Model Limitations

Bitcoin's UTXO model is **stateless** - each transaction is independent. This creates challenges for:
- **Contract State**: Escrow, lending, DAO governance require persistent state
- **State Transitions**: Multi-step contracts need state tracking
- **Dispute Resolution**: Mediation requires state history

## 💡 Solution: Auxiliary State Database + On-Chain Commitments

### Architecture Overview

```
┌─────────────────────────────────────────────────────────────┐
│              ON-CHAIN (UTXO Layer)                          │
│  ┌──────────────────────────────────────────────────────┐   │
│  │  Commitment Transactions                            │   │
│  │  ├── Contract Creation (P2SH commitment)          │   │
│  │  ├── State Updates (OP_RETURN commitments)        │   │
│  │  └── Settlement (Multi-sig spend)                  │   │
│  └──────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
                            │
                    Merkle Root Hash
                            │
┌─────────────────────────────────────────────────────────────┐
│         AUXILIARY STATE DATABASE (SQLite)                  │
│  ┌──────────────────────────────────────────────────────┐   │
│  │  Contract State                                      │   │
│  │  ├── Escrow contracts (status, parties, amounts)    │   │
│  │  ├── Lending contracts (terms, repayments)          │   │
│  │  └── DAO governance (proposals, votes)              │   │
│  │                                                      │   │
│  │  State History                                      │   │
│  │  ├── State transitions (timeline)                   │   │
│  │  ├── Dispute records                                │   │
│  │  └── Settlement proofs                              │   │
│  │                                                      │   │
│  │  On-Chain Commitments                               │   │
│  │  ├── Commitment TXID → State Hash mapping          │   │
│  │  └── Merkle tree roots                             │   │
│  └──────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

## 🔐 Core Design Principles

### 1. **On-Chain Commitments**
- Every state change creates an **OP_RETURN commitment** transaction
- Commitment contains: `merkle_root(state_hash, contract_id, nonce)`
- Provides cryptographic proof of state at any point in time

### 2. **Auxiliary State Database**
- Stores full contract state (not just commitments)
- Enables fast queries and state transitions
- Can be rebuilt from on-chain commitments (verification)

### 3. **State Verification**
- Any node can verify state by:
  1. Reading on-chain commitments
  2. Rebuilding state database
  3. Comparing with auxiliary database

## 📊 Database Schema

### Contracts Table
```sql
CREATE TABLE contracts (
    contract_id TEXT PRIMARY KEY,
    contract_type TEXT NOT NULL,  -- 'escrow', 'lending', 'dao'
    state_hash TEXT NOT NULL,    -- SHA256(current_state)
    merkle_root TEXT,             -- Merkle root of state tree
    commitment_txid TEXT,         -- Latest commitment TXID
    status TEXT NOT NULL,          -- 'pending', 'active', 'disputed', 'settled'
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    
    -- Contract-specific data (JSON)
    contract_data TEXT NOT NULL,
    
    -- Parties
    party_a_address TEXT NOT NULL,
    party_b_address TEXT,
    mediator_address TEXT,
    
    -- On-chain references
    lock_txid TEXT,               -- P2SH lock transaction
    settlement_txid TEXT           -- Final settlement transaction
);

CREATE INDEX idx_contracts_status ON contracts(status);
CREATE INDEX idx_contracts_type ON contracts(contract_type);
CREATE INDEX idx_contracts_commitment ON contracts(commitment_txid);
```

### State History Table
```sql
CREATE TABLE contract_state_history (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    contract_id TEXT NOT NULL,
    state_hash TEXT NOT NULL,
    commitment_txid TEXT NOT NULL,
    state_data TEXT NOT NULL,     -- JSON state snapshot
    transition_type TEXT NOT NULL, -- 'create', 'update', 'dispute', 'settle'
    transitioned_by TEXT,          -- Address that triggered transition
    block_height INTEGER,
    timestamp INTEGER NOT NULL,
    
    FOREIGN KEY (contract_id) REFERENCES contracts(contract_id)
);

CREATE INDEX idx_state_history_contract ON contract_state_history(contract_id);
CREATE INDEX idx_state_history_txid ON contract_state_history(commitment_txid);
```

### Commitments Table
```sql
CREATE TABLE onchain_commitments (
    commitment_txid TEXT PRIMARY KEY,
    contract_id TEXT NOT NULL,
    state_hash TEXT NOT NULL,
    merkle_root TEXT NOT NULL,
    block_height INTEGER,
    block_hash TEXT,
    confirmations INTEGER DEFAULT 0,
    commitment_data TEXT,         -- OP_RETURN data (hex)
    created_at INTEGER NOT NULL,
    
    FOREIGN KEY (contract_id) REFERENCES contracts(contract_id)
);

CREATE INDEX idx_commitments_contract ON onchain_commitments(contract_id);
CREATE INDEX idx_commitments_height ON onchain_commitments(block_height);
```

## 🔄 State Transition Flow

### 1. Contract Creation

```
User Request
    ↓
Create Contract State (DB)
    ↓
Generate P2SH Lock Script
    ↓
Create Lock Transaction (on-chain)
    ↓
Create Commitment Transaction (OP_RETURN)
    ↓
Update Contract (commitment_txid, state_hash)
```

### 2. State Update

```
User Request (e.g., "accept offer")
    ↓
Validate Current State
    ↓
Calculate New State Hash
    ↓
Create Commitment Transaction (OP_RETURN)
    ↓
Update Contract State (DB)
    ↓
Record State History Entry
```

### 3. Settlement

```
Settlement Triggered
    ↓
Validate All Commitments
    ↓
Build Multi-Sig Spend Transaction
    ↓
Broadcast Settlement TX
    ↓
Update Contract Status = 'settled'
    ↓
Record Final Commitment
```

## 💼 Contract Types

### 1. Escrow Contracts

**State Fields**:
- `amount`: Locked DIN amount
- `buyer_address`: Buyer's address
- `seller_address`: Seller's address
- `mediator_address`: Mediator's address
- `status`: 'pending', 'locked', 'released', 'refunded', 'disputed'
- `expiry_time`: Auto-refund timestamp
- `dispute_reason`: If disputed

**State Transitions**:
- `create` → `pending`
- `lock` → `locked` (funds locked in P2SH)
- `accept` → `active` (buyer accepts)
- `release` → `released` (seller releases)
- `refund` → `refunded` (auto-refund or manual)
- `dispute` → `disputed` (requires mediator)

### 2. Lending Contracts

**State Fields**:
- `principal`: Loan amount
- `interest_rate`: Annual interest rate
- `term_days`: Loan term in days
- `lender_address`: Lender's address
- `borrower_address`: Borrower's address
- `collateral_txid`: Collateral lock transaction
- `repayment_schedule`: JSON array of repayment dates/amounts
- `status`: 'pending', 'active', 'repaid', 'defaulted'

**State Transitions**:
- `create` → `pending`
- `fund` → `active` (loan funded)
- `repay` → `repaid` (partial or full)
- `default` → `defaulted` (collateral seized)

### 3. DAO Governance Contracts

**State Fields**:
- `dao_id`: DAO identifier
- `proposal_id`: Current proposal ID
- `voting_period_end`: Timestamp
- `votes`: JSON map of {address: vote_weight}
- `quorum`: Minimum votes required
- `status`: 'draft', 'voting', 'passed', 'rejected', 'executed'

**State Transitions**:
- `create_proposal` → `draft`
- `start_voting` → `voting`
- `vote` → `voting` (state updated with vote)
- `end_voting` → `passed` or `rejected`
- `execute` → `executed`

## 🔍 On-Chain Commitment Format

### OP_RETURN Data Structure

```
OP_RETURN <version> <contract_id> <state_hash> <merkle_root> <nonce>
```

**Fields**:
- `version` (1 byte): Protocol version (0x01)
- `contract_id` (32 bytes): SHA256 hash of contract creation data
- `state_hash` (32 bytes): SHA256 of current state JSON
- `merkle_root` (32 bytes): Merkle root of state tree
- `nonce` (8 bytes): Random nonce for uniqueness

**Total**: 105 bytes (fits in OP_RETURN limit)

### Commitment Transaction Structure

```json
{
  "inputs": [
    {
      "txid": "<previous_utxo>",
      "vout": 0
    }
  ],
  "outputs": [
    {
      "value": 0,
      "scriptPubKey": "6a<105 bytes commitment data>"
    },
    {
      "value": <dust_amount>,
      "scriptPubKey": "<change_address>"
    }
  ]
}
```

## 🛡️ Security & Verification

### State Verification Process

1. **Read On-Chain Commitments**
   ```sql
   SELECT * FROM onchain_commitments 
   WHERE contract_id = ? 
   ORDER BY block_height ASC;
   ```

2. **Rebuild State from Commitments**
   - Start with contract creation commitment
   - Apply each state transition in order
   - Calculate state hash at each step

3. **Compare with Database**
   - Rebuilt state hash == database state_hash
   - All commitments verified on-chain

### Dispute Resolution

1. **Mediator Queries State**
   - Reads full state history from database
   - Verifies against on-chain commitments
   - Reviews dispute evidence

2. **Mediator Creates Settlement**
   - Builds multi-sig spend transaction
   - Signs with mediator key
   - Broadcasts settlement

3. **State Updated**
   - New commitment created
   - Contract status = 'settled'
   - Final state recorded

## 📝 Implementation Plan

### Phase 1: Core Infrastructure (Week 1-2)
- [ ] Create `ContractStateDB` class
- [ ] Implement database schema
- [ ] Create commitment transaction builder
- [ ] Implement state hash calculation
- [ ] Add state verification logic

### Phase 2: Escrow Contracts (Week 2-3)
- [ ] Extend existing escrow system
- [ ] Add on-chain commitments
- [ ] Implement state transitions
- [ ] Add dispute resolution

### Phase 3: Lending Contracts (Week 3-4)
- [ ] Design lending contract schema
- [ ] Implement repayment tracking
- [ ] Add collateral management
- [ ] Create default handling

### Phase 4: DAO Governance (Week 4-5)
- [ ] Design DAO contract schema
- [ ] Implement voting system
- [ ] Add proposal management
- [ ] Create execution mechanism

### Phase 5: Integration & Testing (Week 5-6)
- [ ] Integrate with existing marketplace
- [ ] Add RPC endpoints
- [ ] Create GUI widgets
- [ ] Comprehensive testing

## 🎯 Key Benefits

1. **UTXO Compatible**: Works with Bitcoin's stateless model
2. **Cryptographically Verifiable**: State can be verified from on-chain data
3. **Fast Queries**: Database enables efficient state lookups
4. **Dispute Resolution**: Full state history enables mediation
5. **Extensible**: Easy to add new contract types

## ⚠️ Considerations

1. **Database Trust**: Auxiliary DB is trusted for queries, but state can be verified
2. **Commitment Costs**: Each state update requires transaction fee
3. **State Size**: Large contracts may need state compression
4. **Reorg Handling**: Commitments must handle blockchain reorganizations

## 📚 References

- Bitcoin OP_RETURN: 80 bytes standard, up to 83 bytes possible
- P2SH Multi-Sig: Standard Bitcoin feature
- Merkle Trees: Standard cryptographic structure
- State Channels: Similar concept (Lightning Network)

---

**Status**: 🏗️ **Architecture Design Complete**

This architecture provides a robust foundation for implementing complex contracts on a UTXO-based blockchain.

