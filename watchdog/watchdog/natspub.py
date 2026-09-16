"""Best-effort NATS core publish over a raw socket (stdlib only).

Used ONLY to announce control.halt after the kill path has already acted
through Alpaca REST. JetStream captures core publishes on stream subjects; the
Nats-Msg-Id header lets the stream dedupe a retry.
"""
from __future__ import annotations

import json
import logging
import socket
import uuid
from urllib.parse import urlparse

log = logging.getLogger("watchdog.nats")


def publish(nats_url: str, subject: str, payload: dict, producer: str = "watchdog", timeout: float = 3.0) -> bool:
    try:
        u = urlparse(nats_url)
        host, port = u.hostname or "127.0.0.1", u.port or 4222
        env = {"schema_version": "1.0", "msg_id": str(uuid.uuid4()), "ts_utc": payload.get("issued_at_utc"), "producer": producer, "payload": payload}
        body = json.dumps(env, separators=(",", ":")).encode()
        headers = f"NATS/1.0\r\nNats-Msg-Id: {env['msg_id']}\r\n\r\n".encode()
        with socket.create_connection((host, port), timeout=timeout) as s:
            s.settimeout(timeout)
            s.recv(4096)  # INFO
            connect = json.dumps({"verbose": False, "pedantic": False, "headers": True, "name": "watchdog", "lang": "python", "version": "0.1"})
            s.sendall(f"CONNECT {connect}\r\n".encode())
            s.sendall(f"HPUB {subject} {len(headers)} {len(headers) + len(body)}\r\n".encode() + headers + body + b"\r\n")
            s.sendall(b"PING\r\n")
            got = b""
            while b"PONG" not in got and b"-ERR" not in got:
                chunk = s.recv(4096)
                if not chunk:
                    break
                got += chunk
            if b"-ERR" in got:
                log.error("nats publish error: %s", got)
                return False
        return True
    except Exception as e:  # noqa: BLE001
        log.error("nats publish failed (non-fatal): %s", e)
        return False
