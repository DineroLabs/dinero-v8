# Dinero Explorers

Two production-ready web dashboards for the Dinero blockchain, demonstrating the new RPC improvements (input amount lookup and fee calculation).

## ⚠️ Explorer Notice

**This explorer is a read-only inspection tool.**

- It does **not** define consensus rules
- All consensus rules are enforced by `dinerod`
- This tool queries the blockchain via RPC for display purposes only
- No transactions are created or broadcast by this explorer
- Safe for public deployment and external use

## Components

### 1. Block Explorer (`app.py`)
General-purpose blockchain explorer for viewing blocks and transactions.

### 2. Miner Dashboard (`miner.py`)
Specialized dashboard for miners to verify rewards and track mining performance.

---

# Block Explorer

A minimal, lightweight block explorer for the Dinero blockchain built with Flask and the Dinero RPC client.

**Access:** `http://localhost:8000` (default)

## Features

✅ **Latest Blocks** - View the most recent 20 blocks on the chain
✅ **Block Details** - Detailed view of blocks including all transactions
✅ **Transaction Details** - Full transaction breakdown with:
  - Input amounts (via prevout lookup)
  - Output amounts and types
  - Transaction fees (Total Inputs - Total Outputs)
  - Taproot/SegWit detection
  - Coinbase identification

✅ **Search** - Search by block height, block hash, or transaction ID
✅ **Responsive Design** - Mobile-friendly Bootstrap 5 UI
✅ **JSON API** - Programmatic access to blockchain data

## Installation

### Prerequisites

- Python 3.11+ (tested with 3.11.13 and 3.14.2)
- Running Dinero daemon (`dinerod`)

### Setup

```bash
# Install Flask
pip3 install flask --break-system-packages

# Or for Python 3.11 specifically (iOS compatibility)
python3.11 -m pip install flask --break-system-packages

# Navigate to explorer directory
cd ~/Documents/DineroCoin/explorer
```

## Usage

### Basic Usage

```bash
# Run with default settings (port 5000)
python3 app.py

# Or with Python 3.11 for iOS compatibility
python3.11 app.py
```

### Custom Configuration

```bash
# Specify custom datadir
python3 app.py --datadir /path/to/dinero/data

# Run on different port (recommended: 8000)
python3 app.py --port 8000

# Run on all interfaces (accessible from network)
python3 app.py --host 0.0.0.0 --port 8000

# Enable debug mode
python3 app.py --debug
```

### Access the Explorer

Once running, open your browser to:
```
http://localhost:8000
```

## Architecture

The explorer is built on the new RPC improvements:

### Uses `blockchain.gettransaction`
- Resolves input amounts via `GetPrevout()` helper
- Calculates transaction fees automatically
- Detects output types (Taproot, SegWit v0, Witness, Legacy)
- Gracefully handles missing prevouts

### Uses `dinero_rpc.py`
- Cookie-based authentication (reads `.cookie` file)
- Clean Python API with namespaced methods
- Automatic connection handling

### File Structure

```
explorer/
├── app.py                  # Block Explorer Flask application
├── miner.py                # Miner Dashboard Flask application
├── templates/
│   ├── base.html           # Shared base template with navigation
│   ├── index.html          # Latest blocks page
│   ├── block.html          # Block detail page
│   ├── transaction.html    # Transaction detail page
│   ├── error.html          # Error page
│   ├── miner_dashboard.html # Miner stats overview
│   └── miner_block.html    # Miner block verification
└── README.md               # This file
```

## API Endpoints

### GET /api/info

Returns blockchain information:

```json
{
  "blocks": 105,
  "chain": "main",
  "bestblockhash": "000068ccd68e3f17ef71baa1ae2409102fa694d8d2412bd365c74761fa3e5e83",
  "difficulty": 1023.9846191369579,
  "moneysupply": "2638300.00000000",
  "verificationprogress": 1.0
}
```

### GET /block/{height_or_hash}

Returns block details with all transactions.

### GET /tx/{txid}

Returns transaction details with input amounts and fees.

## Features Showcase

### Input Amount Resolution
The explorer demonstrates the new input amount lookup:
- Shows exact input values (in DIN and una)
- Displays prevout scriptPubKey
- Resolves amounts by searching blockchain history
- Gracefully handles missing prevouts

### Fee Calculation
Accurate fee calculation for non-coinbase transactions:
```
Fee = Total Inputs - Total Outputs
```

Displayed in both DIN and una units.

### Output Type Detection
- **Taproot** (5120...) - Green badge
- **SegWit v0** - Info badge
- **Witness** - Secondary badge
- **Coinbase** - Warning badge
- **Legacy** - Default badge

## Development

### Debug Mode
```bash
python3 app.py --debug
```

Debug mode enables:
- Auto-reload on code changes
- Detailed error pages
- Stack traces in browser

### Template Filters

Custom Jinja2 filters available:
- `{{ value | din }}` - Format DIN amounts with 8 decimals
- `{{ timestamp | timestamp }}` - Convert Unix time to readable format
- `{{ hash | shorthash }}` - Shorten hashes for display
- `{{ number | number_format }}` - Add thousand separators

### Adding New Features

1. Create route in `app.py`
2. Create template in `templates/`
3. Add navigation link in `base.html`
4. Update this README

## Production Deployment

For production use:

### 1. Use a production WSGI server

```bash
# Install Gunicorn
pip3 install gunicorn

# Run with 4 workers
gunicorn -w 4 -b 0.0.0.0:8000 app:app

# Or with specific Python version
python3.11 -m gunicorn -w 4 -b 0.0.0.0:8000 app:app
```

### 2. Use a reverse proxy (nginx)

```nginx
server {
    listen 80;
    server_name explorer.dinero.xyz;

    location / {
        proxy_pass http://127.0.0.1:8000;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    }
}
```

### 3. Enable HTTPS with Let's Encrypt

```bash
certbot --nginx -d explorer.dinero.xyz
```

### 4. Run as systemd service

```ini
# /etc/systemd/system/dinero-explorer.service
[Unit]
Description=Dinero Block Explorer
After=network.target dinerod.service

[Service]
Type=simple
User=dinero
WorkingDirectory=/home/dinero/DineroCoin/explorer
ExecStart=/usr/bin/python3.11 app.py --port 8000
Restart=always

[Install]
WantedBy=multi-user.target
```

## Troubleshooting

### "Failed to connect to Dinero daemon"
- Ensure `dinerod` is running
- Check that RPC port (20998) is accessible
- Verify datadir path is correct
- Check `.cookie` file exists in datadir

### "Block/Transaction not found"
- Verify the daemon has synced
- Check if the block height/hash is valid
- For transactions, ensure they're in confirmed blocks

### Slow Performance
- Input amount lookup requires blockchain scanning
- First transaction views may be slower
- Consider caching frequently accessed data
- Use pagination for large block ranges

### Template Filter Errors
- Ensure all custom filters are defined in `app.py`
- Check `number_format`, `din`, `timestamp`, `shorthash` filters exist

## Python Version Compatibility

Tested and working with:
- **Python 3.11.13** ✅ (iOS compatible)
- **Python 3.14.2** ✅ (macOS)

Both versions work identically with Flask 3.1.2.

## Built With

- [Flask 3.1.2](https://flask.palletsprojects.com/) - Web framework
- [Bootstrap 5](https://getbootstrap.com/) - UI framework
- [Dinero RPC Client](../scripts/dinero_rpc.py) - RPC communication
- [Jinja2 3.1.6](https://jinja.palletsprojects.com/) - Template engine

---

# Miner Dashboard

A specialized dashboard for miners to verify block rewards, track fees, and monitor mining performance.

**Access:** `http://localhost:5001` (default)

## Features

✅ **Mining Statistics**
  - Total blocks mined
  - Total rewards earned
  - Total fees collected
  - Average reward per block
  - Average fees per block
  - Blocks per day calculation
  - Fee percentage of total rewards

✅ **Block Verification**
  - Reward breakdown (coinbase = base reward + fees)
  - Coinbase output inspection
  - Transaction fee totals
  - Reward verification status

✅ **Recent Blocks View**
  - Last 20 blocks mined
  - Confirmation status with color-coded badges
  - Fee and reward breakdown per block
  - Quick access to block details

✅ **JSON API** - Mining stats endpoint for monitoring tools
✅ **Auto-refresh** - Dashboard updates every 30 seconds
✅ **Address Filtering** - Optional filter by mining address

## Installation

Same as Block Explorer (see above).

## Usage

### Basic Usage

```bash
# Show all blocks (no address filter)
python3 miner.py

# Or with Python 3.11
python3.11 miner.py
```

### Filter by Mining Address

```bash
# Only show blocks mined to specific address
python3 miner.py --address din1yourminingaddress...
```

### Custom Configuration

```bash
# Specify custom datadir
python3 miner.py --datadir /path/to/dinero/data

# Run on different port (default: 5001)
python3 miner.py --port 5001

# Run on all interfaces
python3 miner.py --host 0.0.0.0 --port 5001

# Enable debug mode
python3 miner.py --debug
```

### Access the Dashboard

Once running, open your browser to:
```
http://localhost:5001
```

## API Endpoints

### GET /api/stats

Returns mining statistics:

```json
{
  "mining_stats": {
    "total_blocks": 35,
    "total_rewards": 2631300.0,
    "total_fees": 0.0,
    "avg_reward": 75180.0,
    "avg_fees": 0.0,
    "blocks_per_day": 1.31,
    "fee_percentage": 0.0
  },
  "chain_height": 105,
  "difficulty": 1023.9846191369579,
  "recent_blocks": 35
}
```

### GET /block/{height}

Returns detailed block view with miner-specific information:
- Coinbase amount
- Transaction fees breakdown
- Expected vs actual reward
- Verification status

### GET /verify/{height}

Returns reward verification data:

```json
{
  "height": 1,
  "actual_reward": 100.0,
  "expected_base_reward": 100.0,
  "total_fees": 0.0,
  "expected_total": 100.0,
  "matches": true,
  "difference": 0.0
}
```

## Features Showcase

### Mining Statistics

Comprehensive stats calculated from blockchain data:
- **Total Blocks**: Count of blocks found
- **Total Rewards**: Sum of all coinbase amounts
- **Total Fees**: Sum of all transaction fees collected
- **Blocks per Day**: Mining rate based on time range
- **Fee Percentage**: Fees as percentage of total rewards

### Reward Verification

Validates that coinbase output matches expected reward:
```
Expected Reward = Base Reward + Total Fees
Actual Reward = Coinbase Output Value

Status = ✓ if match, ✗ if mismatch
```

### Confirmation Tracking

Color-coded badges for block maturity:
- **< 6 confirmations** - Warning (yellow)
- **6-99 confirmations** - Info (blue)
- **100+ confirmations** - Success (green)

## Use Cases

### For Solo Miners
- Verify you're receiving correct block rewards
- Track fee collection over time
- Monitor mining rate (blocks per day)
- Ensure coinbase outputs are correct

### For Pool Operators
- Verify pool blocks and payouts
- Track total fees collected
- Monitor pool mining rate
- Provide transparency to pool members

### For Mining Monitoring
- API endpoint for automated monitoring
- Auto-refresh for real-time tracking
- Historical performance metrics

## Production Deployment

Same as Block Explorer. Use Gunicorn with systemd service.

## Troubleshooting

### "No blocks found yet"
- If using `--address` filter, ensure the address is correct
- Verify blocks have been mined to your address
- Check daemon is synced

### Reward Verification Mismatch
- This indicates a potential issue with reward calculation
- Verify daemon is using correct subsidy rules
- Check for consensus issues

### Slow Block Loading
- Scanning for miner blocks requires iterating recent blocks
- Performance depends on chain height
- Consider reducing `max_blocks` parameter in code

## Built With

Same stack as Block Explorer:
- Flask 3.1.2
- Bootstrap 5
- Dinero RPC Client
- Jinja2 3.1.6

---

## Running Both Dashboards

You can run both explorers simultaneously:

```bash
# Terminal 1: Block Explorer
python3.11 app.py --port 8000

# Terminal 2: Miner Dashboard
python3.11 miner.py --port 5001
```

Access:
- Block Explorer: http://localhost:8000
- Miner Dashboard: http://localhost:5001

## License

Same as Dinero project

## Credits

Built with [Claude Code](https://claude.com/claude-code)

Demonstrates the new RPC input amount lookup and fee calculation features:
- GetPrevout() helper for input resolution
- Automatic fee calculation
- Cookie-based RPC authentication
