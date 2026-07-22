"""game_client.py -- UDP request/reply client for the TD5RE live-control socket.

Talks the tiny JSON-over-UDP protocol served by td5_control.c (dev builds,
[Control] Enabled=1 / --Control=1, default 127.0.0.1:37060). One JSON object
per datagram; every request carries an auto-incrementing "id" that the reply
echoes, so a late/duplicate datagram from a previous timed-out request is
discarded instead of being mistaken for the current reply.

Standalone (stdlib only) so it can be driven directly from a Python REPL when
MCP registration is awkward:

    from game_client import GameClient
    c = GameClient()
    print(c.ping())
    print(c.command("start_race", {"track": 5, "opponents": 3, "cops": 1}))
"""
from __future__ import annotations

import json
import os
import socket
from typing import Any, Dict, Optional


DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = int(os.environ.get("TD5RE_CONTROL_PORT", "37060"))


class ControlError(RuntimeError):
    """The game replied ok:false, or the transport failed/timed out."""


class GameClient:
    def __init__(self, host: str = DEFAULT_HOST, port: int = DEFAULT_PORT,
                 timeout: float = 2.0, retries: int = 2):
        self.addr = (host, port)
        self.timeout = timeout
        self.retries = retries
        self._id = 0
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.settimeout(timeout)

    def close(self) -> None:
        try:
            self._sock.close()
        except OSError:
            pass

    def command(self, cmd: str, args: Optional[Dict[str, Any]] = None,
                *, raise_on_error: bool = False) -> Dict[str, Any]:
        """Send one command, wait for the matching reply.

        Returns the parsed reply dict (with "ok"). Raises ControlError on
        transport failure/timeout, or on ok:false when raise_on_error=True.
        """
        self._id += 1
        req_id = self._id
        payload = {"id": req_id, "cmd": cmd}
        if args:
            payload["args"] = args
        blob = json.dumps(payload).encode("utf-8")

        last_exc: Optional[Exception] = None
        for _ in range(self.retries + 1):
            try:
                self._sock.sendto(blob, self.addr)
                # Loop until we see OUR id (drop stale replies from prior tries).
                while True:
                    data, _from = self._sock.recvfrom(65535)
                    try:
                        reply = json.loads(data.decode("utf-8"))
                    except (ValueError, UnicodeDecodeError):
                        continue
                    if reply.get("id") == req_id:
                        if raise_on_error and not reply.get("ok", False):
                            raise ControlError(reply.get("error", "command failed"))
                        return reply
                    # else: stale/mismatched id -> keep reading until timeout
            except socket.timeout as exc:
                last_exc = exc
                continue
        raise ControlError(
            f"no reply to '{cmd}' after {self.retries + 1} tries "
            f"(is td5re.exe running with --Control=1 on {self.addr[0]}:{self.addr[1]}?)"
        ) from last_exc

    # -- convenience wrappers ------------------------------------------------
    def ping(self) -> Dict[str, Any]:
        return self.command("ping")

    def get_state(self) -> Dict[str, Any]:
        return self.command("get_state")

    def is_alive(self) -> bool:
        try:
            return bool(self.ping().get("ok"))
        except ControlError:
            return False
