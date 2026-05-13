# DineroCoin Functional Test Framework

Bitcoin Core-style functional test framework for DineroCoin.

## Quick Start

```bash
# Run all tests
python run_tests.py

# Run specific test
python run_tests.py test_basic

# Run with custom dinerod path
python run_tests.py --dinerod=./build/dinerod

# List available tests
python run_tests.py --list

# Run tests in parallel
python run_tests.py -j 4
```

## Writing Tests

Create a new file `test_myfeature.py`:

```python
#!/usr/bin/env python3
from framework import DineroTestFramework, assert_equal

class MyFeatureTest(DineroTestFramework):

    def set_test_params(self):
        self.num_nodes = 2  # How many nodes to start

    def run_test(self):
        # Mine some blocks
        self.nodes[0].generate(10)

        # Wait for sync
        self.sync_blocks()

        # Check result
        assert_equal(self.nodes[1].getblockcount(), 10)

if __name__ == '__main__':
    MyFeatureTest().main()
```

## Framework API

### DineroTestFramework

Base class for all tests. Override these methods:

| Method | Purpose |
|--------|---------|
| `set_test_params()` | Set `self.num_nodes`, `self.extra_args`, etc. |
| `setup_network()` | Custom network topology (optional) |
| `run_test()` | Your actual test logic |

### Node Access

```python
self.nodes[0].getblockcount()       # RPC call
self.nodes[0].generate(10)          # Mine blocks
self.nodes[0].getnewaddress()       # Get address
self.nodes[0].sendtoaddress(addr, 1.0)  # Send coins
```

### Utilities

```python
from framework import (
    connect_nodes,      # Connect two nodes
    disconnect_nodes,   # Disconnect two nodes
    sync_blocks,        # Wait for same tip
    sync_mempools,      # Wait for same mempool
    wait_until,         # Wait for condition
    assert_equal,       # Assert values equal
    assert_raises_rpc_error,  # Assert RPC error
)

# Examples
connect_nodes(self.nodes[0], self.nodes[1])
sync_blocks(self.nodes)
wait_until(lambda: self.nodes[0].getblockcount() > 10)
assert_equal(balance, 50.0)
```

## Test Categories

| Test | Purpose |
|------|---------|
| `test_basic.py` | Node startup, mining, sync |
| `test_reorg.py` | Chain reorganization |
| (add more as needed) |

## Directory Structure

```
tools/functional/
├── framework/
│   ├── __init__.py
│   ├── dinero_test_framework.py  # Base test class
│   ├── dinero_node.py            # Node management
│   ├── rpc.py                    # RPC client
│   └── util.py                   # Assertions & utilities
├── test_basic.py
├── test_reorg.py
├── run_tests.py                  # Test runner
└── README.md
```

## Differences from Bitcoin Core

| Bitcoin Core | DineroCoin |
|--------------|------------|
| `BitcoinTestFramework` | `DineroTestFramework` |
| `TestNode` | `DineroNode` |
| `self.nodes[0].cli.getblockcount()` | `self.nodes[0].getblockcount()` |
| Wallet separate process | Integrated wallet |

## Tips

1. **Always sync after mining**: Use `self.sync_blocks()` after `generate()`
2. **Use timeouts**: `wait_until(pred, timeout=30)` prevents hangs
3. **Check logs on failure**: `node.read_log()` for debugging
4. **Clean up**: Framework auto-cleans, use `--nocleanup` to preserve
