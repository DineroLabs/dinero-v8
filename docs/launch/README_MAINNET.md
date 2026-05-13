# 🚀 DineroCoin Mainnet Launch — Genesis + Premine Frozen

**Version:** v0.1.0-consensus-lock  

**Date:** 2025-11-01  

**Status:** ✅ Ready for Deployment  

---

## 🔐 Consensus Integrity

- **Genesis hash:** 173fe6da2ccc8a671380c8845a2c15e70cc8b84132fa2ab61108425c85412a33  

- **Premine hash:** 0000002bd3fa677b43cc4efcb82078db53131f191ad72afb9d463ddd33c7f79a  

- **Consensus checksum:** ff279196e33c326f61191c368726f924c6728e5d36c227844195e653650ef430  

- **Premine merkle:** 2501ed56dda21d4f23bc510f0447f3674dd42ca1d1d8b244aa2bf1c988786640  

- **Timestamp:** 1760472513 (Genesis +180s)

---

## 🌍 Node Deployment Steps

### 1. Sync Latest Code

```bash
git fetch --all
git checkout v0.1.0-consensus-lock
cmake --build build --target dinerod -j8
```

### 2. Deploy to Nodes

```bash
scp build/dinerod build/dinero-cli root@173.249.195.59:/root/DineroCoin/
scp build/dinerod build/dinero-cli root@172.93.160.131:/root/DineroCoin/
```

### 3. Start Daemons

```bash
ssh root@173.249.195.59 "./dinerod --datadir=/root/.dinero --printtoconsole"
ssh root@172.93.160.131 "./dinerod --datadir=/root/.dinero --printtoconsole"
```

**Expected startup output:**

```
✅ Genesis block verification PASSED
✅ Premine block verification PASSED
🔐 Consensus checksum: ff279196...
```

### 🔎 Verify Chain Integrity

Run the script:

```bash
./docs/launch/verify_consensus.sh
```

It checks:

- Genesis + Premine verification
- Consensus checksum alignment
- Node RPC response parity

---

## 🧾 Security Checklist

- ✅ Genesis and premine verified
- ✅ Same source commit on all nodes
- ✅ Matching SHA256 binaries
- ✅ Ports open and secured (20999, 20998, 21001)
- ✅ Nginx reverse proxy TLS verified
- ✅ Log retention enabled

---

**"Dinero: Real Money for Free People"**

Mainnet launch — November 2025

Immutable. Deterministic. Verified.

