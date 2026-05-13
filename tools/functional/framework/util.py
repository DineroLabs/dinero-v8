"""
DineroCoin Functional Test Utilities

Helper functions for node synchronization, assertions, and common operations.
"""

import time
from typing import List, Callable, Any, Optional, TYPE_CHECKING

if TYPE_CHECKING:
    from .dinero_node import DineroNode


class AssertionError(Exception):
    """Test assertion failed"""
    pass


def assert_equal(thing1: Any, thing2: Any, message: str = "") -> None:
    """Assert two values are equal"""
    if thing1 != thing2:
        msg = f"not({thing1} == {thing2})"
        if message:
            msg = f"{message}: {msg}"
        raise AssertionError(msg)


def assert_not_equal(thing1: Any, thing2: Any, message: str = "") -> None:
    """Assert two values are not equal"""
    if thing1 == thing2:
        msg = f"not({thing1} != {thing2})"
        if message:
            msg = f"{message}: {msg}"
        raise AssertionError(msg)


def assert_greater_than(thing1: Any, thing2: Any, message: str = "") -> None:
    """Assert thing1 > thing2"""
    if not thing1 > thing2:
        msg = f"not({thing1} > {thing2})"
        if message:
            msg = f"{message}: {msg}"
        raise AssertionError(msg)


def assert_greater_than_or_equal(thing1: Any, thing2: Any, message: str = "") -> None:
    """Assert thing1 >= thing2"""
    if not thing1 >= thing2:
        msg = f"not({thing1} >= {thing2})"
        if message:
            msg = f"{message}: {msg}"
        raise AssertionError(msg)


def assert_raises_rpc_error(
    code: Optional[int],
    message: Optional[str],
    func: Callable,
    *args,
    **kwargs
) -> None:
    """Assert that an RPC call raises an error with specified code/message"""
    from .rpc import RPCError

    try:
        func(*args, **kwargs)
    except RPCError as e:
        if code is not None and e.code != code:
            raise AssertionError(f"Expected RPC error code {code}, got {e.code}")
        if message is not None and message not in e.message:
            raise AssertionError(f"Expected '{message}' in error message, got '{e.message}'")
        return  # Success - exception was raised as expected
    except Exception as e:
        raise AssertionError(f"Expected RPCError, got {type(e).__name__}: {e}")

    raise AssertionError(f"Expected RPCError but no exception was raised")


def wait_until(
    predicate: Callable[[], bool],
    timeout: float = 60,
    interval: float = 0.5,
    message: str = ""
) -> None:
    """
    Wait until predicate returns True, or timeout.

    Usage:
        wait_until(lambda: node.getblockcount() > 10)
    """
    start = time.time()
    while time.time() - start < timeout:
        try:
            if predicate():
                return
        except Exception:
            pass
        time.sleep(interval)

    msg = message or "Timeout waiting for condition"
    raise AssertionError(f"{msg} (waited {timeout}s)")


def connect_nodes(node_a: "DineroNode", node_b: "DineroNode", wait: bool = True) -> None:
    """
    Connect two nodes via P2P.

    node_a will connect to node_b.
    """
    node_a.addnode(f"127.0.0.1:{node_b.p2pport}", "onetry")

    if wait:
        # Wait for connection to establish
        def connected():
            peers_a = node_a.getpeerinfo()
            peers_b = node_b.getpeerinfo()
            # Check if they see each other
            a_sees_b = any(str(node_b.p2pport) in p.get("addr", "") for p in peers_a)
            b_sees_a = any(str(node_a.p2pport) in p.get("addr", "") for p in peers_b)
            return a_sees_b or b_sees_a

        wait_until(connected, timeout=10, message=f"Nodes {node_a.index} and {node_b.index} failed to connect")


def disconnect_nodes(node_a: "DineroNode", node_b: "DineroNode", wait: bool = True) -> None:
    """Disconnect two nodes using disconnectnode RPC (may not work on all implementations)"""
    try:
        node_a.disconnectnode(address=f"127.0.0.1:{node_b.p2pport}")
    except:
        pass
    try:
        node_b.disconnectnode(address=f"127.0.0.1:{node_a.p2pport}")
    except:
        pass

    if wait:
        time.sleep(0.5)  # Give some time for disconnect to process


def isolate_node(node: "DineroNode") -> None:
    """Isolate a node from the network by banning all current peers"""
    # Get current peers and ban them
    peers = node.getpeerinfo()
    for peer in peers:
        addr = peer.get("addr", "")
        if addr:
            # Extract IP without port
            ip = addr.split(":")[0]
            try:
                node.setban(ip, "add")
            except:
                pass
    time.sleep(0.3)  # Wait for connections to drop


def unisolate_node(node: "DineroNode") -> None:
    """Re-enable P2P on a node by clearing bans"""
    try:
        node.clearbanned()
    except:
        pass


def sync_blocks(nodes: List["DineroNode"], timeout: float = 60) -> None:
    """
    Wait until all nodes have the same best block.

    Usage:
        sync_blocks([node0, node1, node2])
    """
    def all_same_tip():
        tips = [n.getbestblockhash() for n in nodes]
        return len(set(tips)) == 1

    wait_until(all_same_tip, timeout=timeout, message="Nodes failed to sync blocks")


def sync_mempools(nodes: List["DineroNode"], timeout: float = 60) -> None:
    """
    Wait until all nodes have the same mempool.

    Usage:
        sync_mempools([node0, node1])
    """
    def all_same_mempool():
        pools = [set(n.getrawmempool()) for n in nodes]
        return all(p == pools[0] for p in pools)

    wait_until(all_same_mempool, timeout=timeout, message="Nodes failed to sync mempools")


def sync_all(nodes: List["DineroNode"], timeout: float = 60) -> None:
    """Sync both blocks and mempools"""
    sync_blocks(nodes, timeout)
    sync_mempools(nodes, timeout)


def mine_blocks(node: "DineroNode", count: int, address: Optional[str] = None) -> List[str]:
    """
    Mine blocks on a node.

    Returns list of block hashes.
    """
    if address:
        return node.generatetoaddress(count, address)
    else:
        return node.generate(count)


def get_coinbase_address(node: "DineroNode") -> str:
    """Get or create an address for mining rewards"""
    try:
        return node.getnewaddress("coinbase")
    except:
        # Fallback for nodes without wallet
        return node.getnewaddress()


def una_round(amount: float) -> float:
    """Round to una precision (8 decimal places)"""
    return round(amount, 8)


def coins_to_una(amount: float) -> int:
    """Convert DIN to una (una)"""
    return int(round(amount * 100_000_000))


def una_to_coins(amount: int) -> float:
    """Convert una (una) to DIN"""
    return amount / 100_000_000
