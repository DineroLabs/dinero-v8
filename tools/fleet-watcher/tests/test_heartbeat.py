import os
import tempfile
import unittest

from heartbeat import should_ping
from store import Store


class TestShouldPing(unittest.TestCase):
    def setUp(self):
        fd, self.path = tempfile.mkstemp(suffix=".db")
        os.close(fd)
        self.store = Store(self.path, clock=lambda: 0.0)

    def tearDown(self):
        os.unlink(self.path)

    def test_pings_when_everything_is_healthy(self):
        self.assertTrue(should_ping(self.store, cycle_committed=True,
                                    worker_alive=True, now=1.0, deadline=300.0))

    def test_failed_cycle_suppresses_ping(self):
        self.assertFalse(should_ping(self.store, cycle_committed=False,
                                     worker_alive=True, now=1.0, deadline=300.0))

    def test_dead_delivery_worker_suppresses_ping(self):
        """The failure this gating exists for: collection fine, alerting dead."""
        self.assertFalse(should_ping(self.store, cycle_committed=True,
                                     worker_alive=False, now=1.0, deadline=300.0))

    def test_overdue_emergency_item_suppresses_ping(self):
        self.store.open_incident("safe_mode", ("a",), "emergency", "halted")
        self.assertFalse(should_ping(self.store, cycle_committed=True,
                                     worker_alive=True, now=1e6, deadline=300.0))

    def test_unreachable_nodes_do_not_suppress_ping(self):
        """The heartbeat means the watcher works, not that the fleet is healthy."""
        self.store.open_incident("majority_unreachable", ("a", "b"), "normal", "down")
        self.assertTrue(should_ping(self.store, cycle_committed=True,
                                    worker_alive=True, now=1.0, deadline=300.0))


if __name__ == "__main__":
    unittest.main()
