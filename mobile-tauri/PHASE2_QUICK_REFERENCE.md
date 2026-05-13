# 🚀 Phase 2: Payment UX Quick Reference

## 🎯 Available Commands

### Export Transaction History
```typescript
const { exportHistory } = useWallet();

// Export as CSV
await exportHistory('csv', '/Documents/transactions.csv');

// Export as JSON
await exportHistory('json', '/Documents/transactions.json');
```

### Parse QR Payment URI
```typescript
const { parsePaymentURI } = useWallet();

// QR code: dinero:din1q3abc123...?amount=15.25&label=Coffee
const parsed = await parsePaymentURI(qrCodeData);

// Returns: { address: string, amount: number, label: string }
console.log(parsed.address); // "din1q3abc123..."
console.log(parsed.amount);   // 15.25
console.log(parsed.label);   // "Coffee"
```

## 📋 Payment URI Format

### Basic Format
```
dinero:<address>[?amount=<amount>][&label=<label>]
```

### Examples
```
dinero:din1q3abc123...
dinero:din1q3abc123...?amount=15.25
dinero:din1q3abc123...?amount=15.25&label=Coffee
dinero:din1q3abc123...?amount=100&label=Payment%20for%20services
```

### URL Encoding
- Spaces: `%20`
- Special characters: Standard URL encoding
- Example: `Payment for services` → `Payment%20for%20services`

## 📊 Export File Formats

### CSV Fields
- `txid` - Transaction ID
- `address` - Address (quoted)
- `amount` - Amount in DIN
- `confirmations` - Number of confirmations
- `category` - "send", "receive", or "generate"
- `time` - Unix timestamp
- `label` - Address label (quoted)
- `is_coinbase` - true/false

### JSON Structure
```json
[
  {
    "txid": "...",
    "address": "...",
    "amount": 100.5,
    "confirmations": 10,
    "category": "receive",
    "time": 1234567890,
    "label": "Mining reward",
    "is_coinbase": false
  }
]
```

## 🎨 Integration Examples

### QR Scanner Integration
```typescript
import { useWallet } from '@/hooks/useWallet';

function SendScreen() {
  const { parsePaymentURI, sendTransaction } = useWallet();
  
  const handleQRScan = async (qrData: string) => {
    try {
      const parsed = await parsePaymentURI(qrData);
      // Auto-fill form
      setRecipient(parsed.address);
      setAmount(parsed.amount);
      setLabel(parsed.label);
    } catch (error) {
      alert('Invalid QR code');
    }
  };
  
  return <QRScanner onScan={handleQRScan} />;
}
```

### Export Button
```typescript
function SettingsScreen() {
  const { exportHistory } = useWallet();
  
  const handleExport = async (format: 'csv' | 'json') => {
    const timestamp = new Date().toISOString().replace(/[:.]/g, '-');
    const dest = `/Documents/transactions-${timestamp}.${format}`;
    await exportHistory(format, dest);
    alert('✅ Exported transaction history');
  };
  
  return (
    <>
      <button onClick={() => handleExport('csv')}>Export CSV</button>
      <button onClick={() => handleExport('json')}>Export JSON</button>
    </>
  );
}
```

## ✅ Status

- ✅ Transaction export (CSV/JSON)
- ✅ QR payment URI parsing
- ✅ React hooks integrated
- ✅ Tauri commands registered
- ✅ Library builds successfully

---

**Ready for UI integration!** 🎉

