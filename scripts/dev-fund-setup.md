# Developer Fund Setup Guide

## 🔐 Secure Developer Fund Management

### 1. Generate Developer Fund Address

```bash
# Create a separate wallet for the developer fund
./dinerod -datadir=/secure/dev-fund-wallet -daemon

# Generate the developer fund address
./dinero-cli -datadir=/secure/dev-fund-wallet getnewaddress "developer_fund"
# Output: din1qXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX

# Get the private key (STORE OFFLINE!)
./dinero-cli -datadir=/secure/dev-fund-wallet dumpprivkey "din1qXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
# Output: L1XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX

# Get the hash160 for the address
./dinero-cli -datadir=/secure/dev-fund-wallet validateaddress "din1qXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
# Extract the "scriptPubKey" field and get the 20-byte hash160
```

### 2. Update Chain Facts

Edit `src/consensus/chain_facts.hpp`:

```cpp
// Replace the placeholder with your real hash160
static constexpr uint8_t DEV_FUND_P2WPKH[22] = {
    0x00, 0x14,
    // YOUR 20-byte hash160 goes here:
    0xXX, 0xXX, 0xXX, 0xXX, 0xXX, 0xXX, 0xXX, 0xXX, 0xXX, 0xXX,
    0xXX, 0xXX, 0xXX, 0xXX, 0xXX, 0xXX, 0xXX, 0xXX, 0xXX, 0xXX
};
```

### 3. Security Best Practices

**Private Key Storage:**
- Store the developer fund private key on an air-gapped machine
- Use hardware wallet for additional security
- Create multiple encrypted backups
- Never store the private key on the same machine as the node

**Operational Security:**
- Use a separate wallet instance for the developer fund
- Import the private key only when spending is needed
- Monitor the developer fund address for unexpected transactions
- Set up alerts for developer fund movements

### 4. Spending Developer Fund

```bash
# Import private key when needed (temporary)
./dinero-cli -datadir=/secure/dev-fund-wallet importprivkey "L1XXXXXXXXX" "dev_fund" false

# Check balance
./dinero-cli -datadir=/secure/dev-fund-wallet getbalance "dev_fund"

# Send funds (example)
./dinero-cli -datadir=/secure/dev-fund-wallet sendtoaddress "din1qrecipient..." 1000.0 "Development expenses"

# Remove private key after use
./dinero-cli -datadir=/secure/dev-fund-wallet dumpprivkey "din1qXXXXXX" > /secure/backup.key
./dinero-cli -datadir=/secure/dev-fund-wallet removeprunedfunds "din1qXXXXXX"
```

### 5. Verification

**Verify Premine Block:**
```bash
# Check that block 1 contains exactly 2M DIN to your address
./dinero-cli getblock $(./dinero-cli getblockhash 1) true

# Verify the coinbase transaction
./dinero-cli getrawtransaction "COINBASE_TXID" true

# Confirm the output goes to your dev fund address
```

**Network Integrity:**
```bash
# Verify all nodes have the same premine block
./dinero-cli getblockhash 1
# Should be identical across all nodes

# Check total supply includes premine
./dinero-cli gettxoutsetinfo
```

## ⚠️ CRITICAL SECURITY NOTES

1. **Never commit private keys to version control**
2. **Test everything on testnet first**
3. **Verify premine block hash is deterministic**
4. **Monitor developer fund address continuously**
5. **Use multi-signature for large expenditures**

## 🚀 Deployment Checklist

- [ ] Generate secure developer fund address
- [ ] Update `DEV_FUND_P2WPKH` in chain_facts.hpp
- [ ] Set `PREMINE_BYPASS_POW = false`
- [ ] Build with `scripts/build-mainnet.sh`
- [ ] Test on testnet
- [ ] Verify premine block creation
- [ ] Deploy to mainnet
- [ ] Monitor developer fund balance
