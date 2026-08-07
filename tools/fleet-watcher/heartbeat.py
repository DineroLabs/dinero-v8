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


CANARY_MAX_AGE_SECONDS = 2 * 24 * 60 * 60   # two canary intervals of grace


def should_ping(store: Store, cycle_committed: bool, worker_alive: bool,
                 now: float, deadline: float,
                 canary_max_age: float = CANARY_MAX_AGE_SECONDS) -> bool:
    """`now` MUST come from the same clock the Store was constructed with.

    Mixing domains silently disables the overdue gate — a monotonic `now`
    against wall-clock `created_at` compares nonsense, and a None `now` makes
    the SQL comparison NULL, which reads as "nothing overdue" and pings. Both
    fail OPEN, which is the wrong direction for a dead-man, so guard first.
    """
    if not isinstance(now, (int, float)) or isinstance(now, bool):
        return False
    if not cycle_committed:
        return False
    if not worker_alive:
        return False
    if store.has_overdue_critical(now=now, deadline=deadline):
        return False
    if store.has_stale_canary(now=now, max_age=canary_max_age):
        # The canary is the only proactive test of the alert path. If it cannot
        # be delivered, alerting is broken even though nothing has failed yet,
        # and the operator must find out from the dead-man rather than from the
        # next real incident going unreported.
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
