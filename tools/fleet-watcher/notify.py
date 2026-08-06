# tools/fleet-watcher/notify.py
"""Notification transports.

The interface is deliberately tiny so another provider can be added without
touching rules or storage. send() returns success rather than raising, because
delivery failure is an expected condition the outbox already handles.
"""
from __future__ import annotations

import http.client
import json
import urllib.parse
import urllib.request
from typing import Protocol

PUSHOVER_URL = "https://api.pushover.net/1/messages.json"


class Notifier(Protocol):
    def send(self, title: str, message: str, priority: str) -> bool: ...


class PushoverNotifier:
    """Pushover transport.

    Emergency priority repeats until acknowledged OR until `expire` elapses —
    three hours, Pushover's maximum. It is not indefinite re-paging, and this
    docstring deliberately does not claim otherwise: `send()` returns True when
    Pushover ACCEPTS the message, which is submission, not operator
    acknowledgement. After the window lapses unacknowledged nothing re-pages,
    though the incident stays open and queryable, and the normal recovery
    notification still fires when the condition clears.

    True indefinite escalation needs receipt polling and persisted
    acknowledgement state. That is a separate feature, not something a
    docstring should imply.
    """

    def __init__(self, token: str, user_key: str, timeout: float = 10.0,
                 retry: int = 60, expire: int = 10800) -> None:
        self._token = token
        self._user = user_key
        self._timeout = timeout
        self._retry = retry
        self._expire = expire

    def send(self, title: str, message: str, priority: str) -> bool:
        fields = {"token": self._token, "user": self._user,
                  "title": title, "message": message}
        if priority == "emergency":
            fields.update({"priority": "2", "retry": str(self._retry),
                           "expire": str(self._expire)})
        data = urllib.parse.urlencode(fields).encode()
        request = urllib.request.Request(PUSHOVER_URL, data=data, method="POST")
        try:
            with urllib.request.urlopen(request, timeout=self._timeout) as response:
                return json.loads(response.read()).get("status") == 1
        except (OSError, http.client.HTTPException, ValueError):
            # urllib only wraps errors from h.request(); getresponse() and
            # read() propagate raw, so ConnectionResetError, IncompleteRead and
            # ssl.SSLError all reach here. urllib.error.URLError and
            # socket.timeout are both subclasses of OSError, so this set covers
            # them without naming them.
            return False
