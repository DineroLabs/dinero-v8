# Marketplace Contracts - Quick Start Guide

## Prerequisites

- DineroCoin daemon running (`dinerod`)
- RPC access configured
- Database directory exists (`./data/contracts.db`)

## Quick Start

### 1. Escrow Contract

```bash
# Create escrow
CONTRACT=$(dinero-cli contract.createescrowwithcommitment \
  '{"seller_address":"din1q...","buyer_address":"din1q...","amount":100.0}')

# Extract contract_id
CONTRACT_ID=$(echo $CONTRACT | jq -r '.contract_id')

# Record commitment (after broadcasting)
dinero-cli contract.recordcommitment \
  "{\"contract_id\":\"$CONTRACT_ID\",\"commitment_txid\":\"abc123...\"}"

# Update state
dinero-cli contract.updateescrowstate \
  "{\"contract_id\":\"$CONTRACT_ID\",\"status\":\"locked\",\"transitioned_by\":\"din1q...\"}"

# Verify state
dinero-cli contract.verifyescrowstate $CONTRACT_ID
```

### 2. Lending Contract

```bash
# Create lending contract
LOAN=$(dinero-cli contract.createlending \
  '{"lender_address":"din1q...","borrower_address":"din1q...","principal":1000.0,"interest_rate":5.0,"term_months":12}')

LOAN_ID=$(echo $LOAN | jq -r '.contract_id')

# Activate loan
dinero-cli contract.activateloan \
  "{\"contract_id\":\"$LOAN_ID\",\"funding_txid\":\"fund123...\"}"

# Record payment
dinero-cli contract.recordpayment \
  "{\"contract_id\":\"$LOAN_ID\",\"payment_number\":1,\"payment_txid\":\"pay123...\",\"amount_paid\":85.61}"

# Get schedule
dinero-cli contract.getrepaymentschedule $LOAN_ID
```

### 3. DAO Governance

```bash
# Create DAO
DAO=$(dinero-cli contract.createdao \
  '{"creator_address":"din1q...","dao_name":"MyDAO","quorum_threshold":1000,"approval_threshold":0.51}')

DAO_ID=$(echo $DAO | jq -r '.dao_id')

# Create proposal
PROPOSAL=$(dinero-cli contract.createproposal \
  "{\"dao_id\":\"$DAO_ID\",\"proposer_address\":\"din1q...\",\"title\":\"Spend 100 DIN\",\"description\":\"Description\"}")

PROPOSAL_ID=$(echo $PROPOSAL | jq -r '.proposal_id')

# Submit proposal
dinero-cli contract.submitproposal $PROPOSAL_ID

# Vote
dinero-cli contract.voteproposal \
  "{\"proposal_id\":\"$PROPOSAL_ID\",\"voter_address\":\"din1q...\",\"choice\":\"yes\",\"voting_power\":500}"

# Execute (if passed)
dinero-cli contract.executeproposal \
  "{\"proposal_id\":\"$PROPOSAL_ID\",\"executor_address\":\"din1q...\",\"execution_txid\":\"exec123...\"}"
```

## Common Operations

### Get Contract State
```bash
# Escrow
dinero-cli contract.getescrowcontract $CONTRACT_ID

# Lending
dinero-cli contract.getlendingcontract $LOAN_ID

# DAO
dinero-cli contract.getdao $DAO_ID
```

### Verify State
```bash
# Escrow
dinero-cli contract.verifyescrowstate $CONTRACT_ID

# Lending (via getlendingcontract)
dinero-cli contract.getlendingcontract $LOAN_ID | jq '.state_hash'

# DAO (via getdao)
dinero-cli contract.getdao $DAO_ID | jq '.state_hash'
```

## Troubleshooting

### Database Not Found
```bash
# Create database directory
mkdir -p ./data
```

### RPC Connection Error
```bash
# Check daemon is running
ps aux | grep dinerod

# Check RPC port
netstat -an | grep 20998
```

### Contract Not Found
```bash
# Verify contract ID
echo $CONTRACT_ID

# Check database
sqlite3 ./data/contracts.db "SELECT contract_id FROM contracts;"
```

## Best Practices

1. **Always record commitments** after broadcasting transactions
2. **Verify state** before critical operations
3. **Check confirmations** for commitment transactions
4. **Backup database** regularly
5. **Monitor state changes** via state history

## Support

For issues or questions:
- Check logs: `tail -f ~/.dinero/debug.log`
- Verify RPC: `dinero-cli blockchain.getinfo`
- Check database: `sqlite3 ./data/contracts.db ".tables"`

