"""External dead-man ping.

Software on this host cannot report that this host has disappeared, so absence
detection lives at an external service.

The ping is gated on the WHOLE alarm path, not just collection. A watcher that
polls and persists happily while its delivery worker is dead is worse than one
that crashed: it looks healthy and will never tell you anything again.
Collection working is not the property worth monitoring — the ability to raise
an alarm is.
"""
from __future__ import annotations

import http.client
import urllib.request

from store import Store


def should_ping(store: Store, cycle_committed: bool, worker_alive: bool,
                 now: float, deadline: float) -> bool:
    """`now` must come from the SAME clock the Store was built with, or the
    deadline comparison tests nothing.

    A dead-man must never fail open. A bad clock read (e.g. `now=None`) binds
    as SQL NULL, and every comparison against NULL reads as "nothing is
    overdue" — silently turning a broken clock into a healthy-looking ping.
    The isinstance guard below fails closed instead.
    """
    if not isinstance(now, (int, float)):
        return False
    if not cycle_committed:
        return False
    if not worker_alive:
        return False
    if store.has_overdue_critical(now=now, deadline=deadline):
        return False
    return True


class Heartbeat:
    """Carries no node details and no secrets — liveness only."""

    def __init__(self, url: str, timeout: float = 10.0) -> None:
        self._url = url
        self._timeout = timeout

    def ping(self) -> bool:
        try:
            request = urllib.request.Request(self._url, method="GET")
            with urllib.request.urlopen(request, timeout=self._timeout) as response:
                return 200 <= response.status < 300
        except (OSError, http.client.HTTPException, ValueError):
            # urllib only wraps errors from h.request(); getresponse() and
            # read() propagate raw, so ConnectionResetError,
            # http.client.RemoteDisconnected (the routine keep-alive-proxy
            # blip), IncompleteRead and ssl.SSLError all reach here.
            # urllib.error.URLError and socket.timeout are both subclasses of
            # OSError, so this set covers them without naming them. ValueError
            # covers urlopen() rejecting an unrecognized/malformed URL scheme.
            return False
