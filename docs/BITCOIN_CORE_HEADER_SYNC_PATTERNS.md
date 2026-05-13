# Bitcoin Core: Header Sync Stall Detection & Peer Lie Handling

**Research Date**: 2025-12-21
**Purpose**: Extract battle-tested patterns from Bitcoin Core for DineroCoin Phase N.2

---

## 1. Timeout Policies

### Header Sync Timeout Constants

Bitcoin Core defines two critical constants in [`net_processing.cpp`](https://github.com/bitcoin/bitcoin/blob/master/src/net_processing.cpp):

```cpp
HEADERS_DOWNLOAD_TIMEOUT_BASE = 15min
HEADERS_DOWNLOAD_TIMEOUT_PER_HEADER = 1ms
```

**Calculation**:
```
Timeout = HEADERS_DOWNLOAD_TIMEOUT_BASE + (expected_headers * HEADERS_DOWNLOAD_TIMEOUT_PER_HEADER)
       = 15 minutes + (num_missing_headers * 1ms)
```

For a typical Initial Block Download (IBD) scenario with ~800,000 headers:
- Timeout ≈ 15 min + 800 sec ≈ 28 minutes

**Source**: [Bitcoin Core PR Review Club #25720](https://bitcoincore.reviews/25720)

### Timeout Trigger Conditions

A peer is considered **stalled** if:
1. Best header's timestamp is >24 hours behind current time
2. Current time > peer's `m_headers_sync_timeout`
3. Peer is the designated sync peer

**Source**: [Bitcoin Core 0.11 (ch 5): Initial Block Download](https://en.bitcoin.it/wiki/Bitcoin_Core_0.11_(ch_5):_Initial_Block_Download)

### Adaptive Timeout (Post-IBD)

During block download, Bitcoin Core uses an **adaptive stalling timeout**:

- **Initial timeout**: 2 seconds (default)
- **Increase**: When stall detected, timeout can increase temporarily
- **Decrease**: Timeout slowly reduces back to default by multiplying by 0.85

**Why**: Balance protection against malicious peers while accommodating varying network speeds.

**Source**: [Bitcoin Core PR Review Club #25880](https://bitcoincore.reviews/25880)

---

## 2. Peer Switching Logic

### Single Sync Peer Strategy

Bitcoin Core uses a **single designated sync peer** for header download:

1. **On startup**: If headers chain is >24 hours behind, pick ONE peer to sync with
2. **Stick with peer**: Continue requesting headers from same peer until:
   - Peer delivers headers successfully
   - Peer stalls (timeout exceeded)
   - Peer sends invalid headers

**Rationale**: Prevents bandwidth waste from requesting same headers from multiple peers

### When to Switch Peers

Bitcoin Core switches sync peer when:

1. **Timeout exceeded**: Current peer stalled for >15min + headers_expected * 1ms
2. **Invalid headers**: Peer sent headers that failed validation
3. **Disconnect**: Peer disconnected voluntarily
4. **Caught up**: Received <2000 headers (partial batch = end of peer's chain)

**Protection**: Won't disconnect sync peer if it's the only peer and no other preferred download peers available

**Source**: [Bitcoin Core PR #10345 - Timeout for headers sync](https://github.com/bitcoin/bitcoin/pull/10345)

### Peer Selection Algorithm

After disconnecting stalled peer, Bitcoin Core selects new sync peer:

1. **Prefer outbound peers** over inbound (eclipse attack resistance)
2. **Prefer full-relay peers** over block-relay-only (for tx relay later)
3. **Don't use feeler connections** for sustained header sync
4. **Check all outbound peers**: After receiving <2000 headers, query ALL outbound peers to verify best chain

**Source**: [Bitcoin Core PR Review Club #19858](https://bitcoincore.reviews/19858)

---

## 3. Ban Criteria & Misbehavior Tracking

### Ban Score System (Legacy → Modern)

**Legacy System** (pre-2019):
- Each misbehavior increased peer's ban score
- Ban score ≥ 100 → peer banned for 24 hours
- Different severities: minor transgressions (small increase) vs major (instant ban)

**Modern System** (current):
- Ban score replaced with **discouraged** flag
- Peers accumulating score ≥ 100 → disconnected and discouraged
- Inbound connections from discouraged addresses allowed but **preferred for eviction**

**Source**: [Bitcoin Core 0.11 (ch 4): P2P Network](https://en.bitcoin.it/wiki/Bitcoin_Core_0.11_(ch_4):_P2P_Network)

### Invalid Header Punishment

**Old approach** (pre-PR #10593):
- Invalid block/header → instant ban

**Current approach** (post-PR #10593):
- **Disconnect only** (no ban) for invalid headers from outbound peers
- **Rationale**: Peers running old nodes after softfork shouldn't be banned permanently
- **Still punish**: Only for outbound non-feeler connections

**Exception**: Headers larger than `MAX_BLOCK_SIZE` still result in immediate ban

**Source**: [Bitcoin Core PR #10593 - Relax punishment for peers relaying invalid blocks and headers](https://github.com/bitcoin/bitcoin/pull/10593)

### Misbehavior Categories

| Misbehavior Type | Punishment | Rationale |
|------------------|------------|-----------|
| Extra 'version' message | Minor (usually tolerated) | Harmless protocol violation |
| Sending block > MAX_BLOCK_SIZE | Major (instant ban) | Potential DOS attack |
| Invalid header (wrong PoW) | Disconnect only | Could be old node after softfork |
| Headers not linking to known chain | Disconnect + discourage | Peer likely on wrong fork |
| Timeout during header sync | Disconnect only | Could be slow connection, not malicious |

---

## 4. Recovery Patterns

### After Stall Detection

When sync peer stalls, Bitcoin Core:

1. **Wait for timeout** (15 min + headers * 1ms)
2. **Disconnect stalled peer** (if other peers available)
3. **Select new sync peer** from available outbound peers
4. **Resume from last known header** using block locator
5. **Verify with all outbound peers** after catching up

**No rollback**: Already-accepted headers remain in chain (validated = permanent)

### After Invalid Headers

When peer sends invalid headers, Bitcoin Core:

1. **Immediately stop processing** headers from that peer
2. **Disconnect peer** (and optionally discourage)
3. **Do NOT revert** already-accepted headers
4. **Select new peer** and request from last valid header

**Key insight**: Invalid headers from one peer don't invalidate previously accepted headers from other peers

**Source**: [Bitcoin Core commit fc966bb - moveonly: factor out headers processing](https://github.com/bitcoin/bitcoin/commit/fc966bb)

### Verification After Catch-Up

Once sync peer delivers <2000 headers (indicating caught up):

1. **Query all outbound peers** with getheaders
2. **Compare responses** to detect if sync peer was on wrong fork
3. **Switch to better chain** if another peer has higher chainwork
4. **Only then proceed** to block download phase

**Rationale**: Prevents accepting headers from minority fork during IBD

**Source**: [Bitcoin Core 0.11 (ch 5): Initial Block Download](https://en.bitcoin.it/wiki/Bitcoin_Core_0.11_(ch_5):_Initial_Block_Download)

---

## 5. DOS Mitigations

### Resource Bounds

Bitcoin Core enforces strict limits:

```cpp
MAX_HEADERS_SIZE = 2000        // Max headers per message
MAX_MESSAGE_SIZE = 4MB         // Max P2P message size
MAX_BLOCK_SIZE = 4MB          // Max block size
MAX_INV_SIZE = 50000          // Max inventory items
MAX_ADDR_SIZE = 1000          // Max addresses per addr message
```

**Enforcement**: Messages exceeding limits rejected immediately, peer disconnected

### Rate Limiting Headers Requests

Bitcoin Core prevents request spam:

1. **Single sync peer**: Only one peer actively downloading headers at a time
2. **No parallel requests**: Don't request same headers from multiple peers
3. **Timeout before retry**: Wait for full timeout before switching peers
4. **Exponential backoff** in block locator: Prevents sending huge locators

### Protection Against Slow Drip Attack

**Attack**: Malicious peer sends headers very slowly (just under timeout threshold)

**Defense**:
1. Timeout includes per-header component (1ms per expected header)
2. If peer takes >15min for small number of headers, still times out
3. Adaptive timeout during block download (can increase/decrease based on network)

**Source**: [Bitcoin Core PR Review Club #25880](https://bitcoincore.reviews/25880)

### Eclipse Attack Resistance

Bitcoin Core mitigates eclipse attacks through:

1. **Prefer outbound connections** for sync (user initiates, not attacker)
2. **Multiple connection types**: full-relay (8), block-relay-only (2), feeler (1 temp)
3. **Regular peer rotation**: Periodic new connections to test network view
4. **Verify with multiple peers**: After initial sync, query all outbound peers
5. **Periodic block-relay connections**: Connect to new peer every 5 min, rotate if new block learned

**Source**: [Bitcoin Core PR #19858 - Periodically make block-relay connections and sync headers](https://github.com/bitcoin/bitcoin/pull/19858)

### Defamation Attack Vulnerability

**Issue**: Research identified that ban-score mechanism is vulnerable to **defamation attacks**:
- Network adversaries can exploit ban scores to defame innocent peers
- Peer A can be framed by adversary to appear misbehaving to Peer B

**Current status**: Known issue, not fully resolved in current codebase

**Source**: [Security Analyses of Misbehavior Tracking in Bitcoin Network](https://cwssp.uccs.edu/sites/g/files/kjihxj2466/files/2021-09/5_Security%20Analyses%20of%20Misbehavior%20Tracking%20in%20Bitcoin%20Network.pdf)

---

## Key Takeaways for DineroCoin Phase N.2

### 1. Timeout Strategy
✅ Use base timeout (15 min) + per-header component (1ms)
✅ Adaptive timeout for post-IBD block download
✅ Don't disconnect if no alternative peers available

### 2. Peer Management
✅ Single sync peer at a time (no parallel header download)
✅ Prefer outbound over inbound peers
✅ Verify with all outbound peers after initial sync

### 3. Punishment Policy
✅ Disconnect (don't ban) for invalid headers from outbound peers
✅ Ban only for severe violations (oversized messages, DOS patterns)
✅ Distinguish between malicious and outdated/slow peers

### 4. Recovery
✅ No rollback of already-accepted headers
✅ Resume from last valid header using block locator
✅ Cross-check with multiple peers before proceeding to blocks

### 5. DOS Protection
✅ Strict message size limits (2000 headers max)
✅ Single sync peer prevents bandwidth waste
✅ Timeout prevents slow drip attacks
✅ Prefer outbound connections for eclipse resistance

---

## Implementation Checklist for Phase N.2 Step 2

- [ ] Add `HEADERS_DOWNLOAD_TIMEOUT_BASE` constant (15 min)
- [ ] Add `HEADERS_DOWNLOAD_TIMEOUT_PER_HEADER` constant (1 ms)
- [ ] Implement timeout calculation: base + (expected_headers * per_header)
- [ ] Add `m_headers_sync_timeout` to PeerHeaderInfo
- [ ] Implement stall detection in Tick() method
- [ ] Add peer switching logic when timeout exceeded
- [ ] Implement "verify with all peers" after receiving <2000 headers
- [ ] Add message size validation (reject >2000 headers)
- [ ] Distinguish between disconnect (timeout) and ban (DOS)
- [ ] Add block locator generation for resume after stall
- [ ] Implement outbound peer preference for sync peer selection
- [ ] Add protection: don't disconnect if no alternative peers

---

**References**:

1. [Bitcoin Core 0.11 (ch 5): Initial Block Download](https://en.bitcoin.it/wiki/Bitcoin_Core_0.11_(ch_5):_Initial_Block_Download)
2. [Bitcoin Core PR Review Club #25720 - Reduce bandwidth during initial headers sync](https://bitcoincore.reviews/25720)
3. [Bitcoin Core PR Review Club #25880 - Make stalling timeout adaptive during IBD](https://bitcoincore.reviews/25880)
4. [Bitcoin Core PR #10345 - Timeout for headers sync](https://github.com/bitcoin/bitcoin/pull/10345)
5. [Bitcoin Core PR #10593 - Relax punishment for peers relaying invalid blocks and headers](https://github.com/bitcoin/bitcoin/pull/10593)
6. [Bitcoin Core PR #19858 - Periodically make block-relay connections and sync headers](https://github.com/bitcoin/bitcoin/pull/19858)
7. [Bitcoin Core PR Review Club #19858 - Discussion](https://bitcoincore.reviews/19858)
8. [Bitcoin Core 0.11 (ch 4): P2P Network - Misbehavior tracking](https://en.bitcoin.it/wiki/Bitcoin_Core_0.11_(ch_4):_P2P_Network)
9. [Security Analyses of Misbehavior Tracking in Bitcoin Network](https://cwssp.uccs.edu/sites/g/files/kjihxj2466/files/2021-09/5_Security%20Analyses%20of%20Misbehavior%20Tracking%20in%20Bitcoin%20Network.pdf)
10. [Bitcoin Core net_processing.cpp source](https://github.com/bitcoin/bitcoin/blob/master/src/net_processing.cpp)
