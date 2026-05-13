# GUI Phase 1 Features - Implementation Status

**Date**: October 21, 2025
**Target**: Implement 6 high-value GUI features using existing daemon RPCs

---

## ✅ COMPLETED: Header File Declarations

### Files Modified:
- `/Users/haydarevich/Documents/DineroCoin/gui/src/mainwindow.h`

### Changes Made:

#### 1. **Added Method Declarations** (lines 55-67):
```cpp
// Peer table actions
void onDisconnectPeer();
void onBanPeer();

// Template viewer
void onRefreshTemplate();

// QR Code generator
void onGenerateQR();

// Address book CSV
void onImportAddresses();
void onExportAddresses();
```

#### 2. **Added Update Methods** (lines 77-79):
```cpp
void updateNodeStatus(const QJsonObject& blockchainInfo, const QJsonObject& networkInfo, const QJsonObject& mempoolInfo);
void updatePeerTable(const QJsonArray& peers);
void updateBlockTemplate(const QJsonObject& blockTemplate);
```

#### 3. **Added Member Variables** (lines 196-224):
- **Node Status Pill**: 5 QLabel widgets for chain, height, peers, mempool, sync status
- **Peer Table**: QTableWidget + 3 QPushButton widgets
- **Block Template Viewer**: QTextEdit + 4 QLabel + 1 QPushButton
- **QR Code Generator**: QLabel + QPushButton + QLineEdit
- **Address Book CSV**: 2 QPushButton widgets for import/export

---

## ⏳ IN PROGRESS: Implementation in mainwindow.cpp

### Remaining Tasks:

#### Feature 1: Node Status Pill Widget
**RPCs Used**: `getblockchaininfo`, `getnetworkinfo`, `getmempoolinfo`

**Implementation Needed**:
1. ✅ Add widget declarations to header
2. ⏳ Create UI widgets in `setupUI()`:
   - Create horizontal layout for status pill
   - Add 5 QLabels with icons/styling
   - Position prominently at top of window
3. ⏳ Add RPC calls in `refresh()`:
   ```cpp
   rpc_->call("getblockchaininfo", QJsonArray{});
   rpc_->call("getnetworkinfo", QJsonArray{});
   rpc_->call("getmempoolinfo", QJsonArray{});
   ```
4. ⏳ Implement `updateNodeStatus()` method
5. ⏳ Add handlers in `onRpcResult()` for the 3 RPCs

**UI Design**:
```
┌─────────────────────────────────────────────────────────────┐
│ Chain: MAIN | Height: 12,345 | Peers: 8 | Mempool: 42 | ✅ SYNCED │
└─────────────────────────────────────────────────────────────┘
```

---

#### Feature 2: Dual Network Verification
**Status**: ✅ ALREADY IMPLEMENTED!
- Genesis hash verification: ✅ Done
- Chain name detection: ✅ Done
- Warning banner: ✅ Done

**No additional work needed** - verified in `/Users/haydarevich/Documents/DineroCoin/NETWORK_VERIFICATION_STATUS.md`

---

#### Feature 3: Basic Peer Table
**RPC Used**: `getpeerinfo`

**Implementation Needed**:
1. ✅ Add widget declarations to header
2. ⏳ Create new "Peers" tab in `setupUI()`:
   - Create QTableWidget with columns: IP, Port, Direction (In/Out), Connection Time
   - Add Refresh/Disconnect/Ban buttons
   - Wire up button signals
3. ⏳ Add RPC call in `refresh()`:
   ```cpp
   rpc_->call("getpeerinfo", QJsonArray{});
   ```
4. ⏳ Implement `updatePeerTable()` method to populate table
5. ⏳ Implement action slots: `onDisconnectPeer()`, `onBanPeer()`
6. ⏳ Add handler in `onRpcResult()` for `getpeerinfo`

**UI Design**:
```
┌─────────────────────────────────────────────────────────────┐
│ IP Address        │ Port  │ Direction │ Connected           │
├───────────────────┼───────┼───────────┼─────────────────────┤
│ 172.93.160.131    │ 20999 │ Outbound  │ 2h 34m ago          │
│ 173.249.195.59    │ 20999 │ Outbound  │ 1h 22m ago          │
│ 203.0.113.45      │ 20999 │ Inbound   │ 45m ago             │
└─────────────────────────────────────────────────────────────┘
   [Refresh]  [Disconnect Selected]  [Ban Selected]
```

---

#### Feature 4: Template Viewer Tab
**RPC Used**: `getblocktemplate`

**Implementation Needed**:
1. ✅ Add widget declarations to header
2. ⏳ Create new "Template" tab in `setupUI()`:
   - Add summary labels (height, tx count, fees, difficulty)
   - Add QTextEdit for full JSON display
   - Add Refresh button
   - Wire up button signal
3. ⏳ Add RPC call (on-demand, not in refresh):
   ```cpp
   rpc_->call("getblocktemplate", QJsonArray{});
   ```
4. ⏳ Implement `updateBlockTemplate()` method
5. ⏳ Implement `onRefreshTemplate()` slot
6. ⏳ Add handler in `onRpcResult()` for `getblocktemplate`

**UI Design**:
```
┌─────────────────────────────────────────────────────────────┐
│ Height: 12,346 | Transactions: 25 | Fees: 0.05 DIN | Diff: 1.23e+08 │
│ [Refresh Template]                                          │
├─────────────────────────────────────────────────────────────┤
│ {                                                           │
│   "version": 536870912,                                     │
│   "previousblockhash": "abc123...",                         │
│   "transactions": [                                         │
│     {                                                       │
│       "txid": "def456...",                                  │
│       "fee": 1000,                                          │
│       ...                                                   │
│     }                                                       │
│   ],                                                        │
│   "coinbasevalue": 5000000000,                              │
│   ...                                                       │
│ }                                                           │
└─────────────────────────────────────────────────────────────┘
```

---

#### Feature 5: QR Code Generator
**Dependencies**: Qt QImage, QPainter (no external library needed!)

**Implementation Needed**:
1. ✅ Add widget declarations to header
2. ⏳ Add QR generator section to "Receive" tab in `setupUI()`:
   - Add QLineEdit for address input
   - Add QPushButton "Generate QR"
   - Add QLabel to display QR image
   - Wire up button signal
3. ⏳ Implement `onGenerateQR()` method:
   - Use simple text-based QR encoding OR
   - Include `qrencode` library (lightweight, 50KB)
4. **No RPC needed** - pure GUI feature

**Options for QR Generation**:
- **Option A**: Use `libqrencode` (recommended, ~50KB, production-quality)
- **Option B**: Pure Qt solution using custom encoding (more complex)

**UI Design**:
```
┌─────────────────────────────────────────────────────────────┐
│ Address: [din1q7d3ztduxteydgqhrshrksnflznuvc9dtpwx054_____] │
│          [Generate QR Code]                                 │
│                                                             │
│          ┌─────────────┐                                    │
│          │  ▄▄▄  ▄ ▄▄  │  <- QR Code                       │
│          │ █   █ █ █  █│                                    │
│          │ █▄▄▄█ ▄▄  ▄ │                                    │
│          │ ▄▄▄▄▄ █ █ ▄ │                                    │
│          │  ▄  ▄ ▄▄▄▄█ │                                    │
│          └─────────────┘                                    │
│          (Click to save)                                    │
└─────────────────────────────────────────────────────────────┘
```

---

#### Feature 6: Address Book CSV Import/Export
**Dependencies**: Qt QFile, QTextStream

**Implementation Needed**:
1. ✅ Add widget declarations to header
2. ⏳ Add buttons to "Receive" tab in `setupUI()`:
   - Add "Import CSV" button below address table
   - Add "Export CSV" button
   - Wire up button signals
3. ⏳ Implement `onImportAddresses()` method:
   - Open file dialog (QFileDialog)
   - Parse CSV format: `address,label,amount`
   - Add addresses to table
4. ⏳ Implement `onExportAddresses()` method:
   - Save file dialog
   - Export table to CSV format
5. **No RPC needed** - pure GUI feature

**CSV Format**:
```csv
address,label,amount
din1q7d3ztduxteydgqhrshrksnflznuvc9dtpwx054,Mining Wallet,125.50
din1qe8lk2fn9xj5wqp3yhnr8vgm0c4kzx7p2a5t24f,Savings,0.00
```

**UI Design**:
```
┌─────────────────────────────────────────────────────────────┐
│ Address List                                                │
├─────────────────────────────────────────────────────────────┤
│ [address table - already exists in GUI]                     │
└─────────────────────────────────────────────────────────────┘
   [Import CSV]  [Export CSV]
```

---

## 📋 Implementation Roadmap

### Step 1: Node Status Pill (30 minutes)
- Add UI widgets to setupUI()
- Add 3 RPC calls to refresh()
- Implement updateNodeStatus()
- Add RPC handlers

### Step 2: Verify Network Verification (5 minutes)
- Test existing implementation
- Mark as complete

### Step 3: Peer Table (45 minutes)
- Create new tab
- Add table widget
- Implement RPC call and handler
- Add disconnect/ban functionality

### Step 4: Template Viewer (30 minutes)
- Create new tab
- Add template display
- Implement RPC call and handler

### Step 5: QR Code Generator (60 minutes)
- Add libqrencode dependency to CMakeLists.txt
- Implement QR generation logic
- Add UI widgets

### Step 6: CSV Import/Export (45 minutes)
- Add CSV parsing logic
- Implement import/export methods
- Add UI buttons

### Step 7: Testing (30 minutes)
- Test each feature individually
- Test on Mac build
- Fix any bugs

### Step 8: Build & Package (15 minutes)
- Clean build
- Package for distribution

---

## Estimated Total Time
**~4 hours** for complete implementation of all 6 features

---

## Next Steps

**Option A**: Continue with full implementation now (4 hours of work)
**Option B**: Create detailed implementation scaffolding/pseudocode document
**Option C**: Implement features one-by-one with testing between each

**Recommendation**: **Option A or C** - Implement features incrementally, test each one before moving to the next.

---

## Dependencies to Add

### CMakeLists.txt Changes:
```cmake
# QR Code generation (optional but recommended)
find_package(QREncode)
if(QRENCODE_FOUND)
  target_link_libraries(dinero-qt PRIVATE qrencode)
  add_compile_definitions(HAVE_QRENCODE)
endif()
```

### Alternative (Pure Qt QR):
If qrencode is not available, we can implement a simple QR encoder in pure C++/Qt (more complex but no external dependency).

---

## Files to Modify

1. ✅ `/Users/haydarevich/Documents/DineroCoin/gui/src/mainwindow.h` - COMPLETE
2. ⏳ `/Users/haydarevich/Documents/DineroCoin/gui/src/mainwindow.cpp` - IN PROGRESS
3. ⏳ `/Users/haydarevich/Documents/DineroCoin/gui/CMakeLists.txt` - Need to add qrencode if using

---

## Success Criteria

All features must:
- ✅ Use existing daemon RPCs (no daemon changes)
- ✅ Compile without errors
- ✅ Run on Mac without crashes
- ✅ Display data correctly
- ✅ Handle RPC errors gracefully
- ✅ Follow existing GUI code style

---

**Status**: Header file complete. Ready to implement features in mainwindow.cpp.

Would you like me to:
1. Continue with full implementation now?
2. Implement one feature at a time with testing?
3. Create detailed pseudocode/scaffolding first?
