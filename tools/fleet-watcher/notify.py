# tools/fleet-watcher/notify.py
"""Notification transports.

The interface is deliberately tiny so another provider can be added without
touching rules or storage. send() returns success rather than raising, because
delivery failure is an expected condition the outbox already handles.
"""
from __future__ import annotations

import json
import urllib.error
import urllib.parse
import urllib.request
from typing import Protocol

PUSHOVER_URL = "https://api.pushover.net/1/messages.json"


class Notifier(Protocol):
    def send(self, title: str, message: str, priority: str) -> bool: ...


class PushoverNotifier:
    """Emergency priority repeats until acknowledged. A 3am safe-mode page that
    scrolls away unread is the same as no page at all."""

    def __init__(self, token: str, user_key: str, timeout: float = 10.0,
                 retry: int = 60, expire: int = 3600) -> None:
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
        except (urllib.error.URLError, ValueError, TimeoutError):
            return False
