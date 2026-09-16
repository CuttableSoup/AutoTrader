"""Minimal Alpaca trading REST client (stdlib urllib). Only what the kill path needs."""
from __future__ import annotations

import json
import logging
import time
import urllib.error
import urllib.request

log = logging.getLogger("watchdog.alpaca")


class AlpacaError(RuntimeError):
    def __init__(self, status: int, body: str):
        super().__init__(f"alpaca {status}: {body[:300]}")
        self.status = status
        self.body = body


class Alpaca:
    def __init__(self, base_url: str, key_id: str, secret_key: str, timeout: float = 10.0):
        self.base_url = base_url.rstrip("/")
        self.headers = {"APCA-API-KEY-ID": key_id, "APCA-API-SECRET-KEY": secret_key, "Content-Type": "application/json", "Accept": "application/json"}
        self.timeout = timeout

    def _req(self, method: str, path: str, body: dict | None = None):
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(self.base_url + path, data=data, method=method, headers=self.headers)
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                raw = r.read()
                return json.loads(raw) if raw else None
        except urllib.error.HTTPError as e:
            raise AlpacaError(e.code, e.read().decode(errors="replace")) from None

    def account(self) -> dict:
        return self._req("GET", "/v2/account")

    def positions(self) -> list[dict]:
        return self._req("GET", "/v2/positions") or []

    def open_orders(self) -> list[dict]:
        return self._req("GET", "/v2/orders?status=open&limit=500") or []

    def cancel_all_orders(self):
        return self._req("DELETE", "/v2/orders")

    def close_all_positions(self):
        return self._req("DELETE", "/v2/positions?cancel_orders=true")

    def kill(self, attempts: int = 5) -> dict:
        """Cancel every order, close every position, verify flat. Retries with backoff. Returns a report."""
        report = {"attempts": 0, "flat": False, "errors": []}
        for i in range(attempts):
            report["attempts"] = i + 1
            try:
                self.cancel_all_orders()
            except Exception as e:  # noqa: BLE001
                report["errors"].append(f"cancel_all: {e}")
            try:
                self.close_all_positions()
            except Exception as e:  # noqa: BLE001
                report["errors"].append(f"close_all: {e}")
            try:
                pos = self.positions()
                orders = self.open_orders()
                report["positions_left"] = len(pos)
                report["orders_left"] = len(orders)
                if not pos and not orders:
                    report["flat"] = True
                    return report
            except Exception as e:  # noqa: BLE001
                report["errors"].append(f"verify: {e}")
            time.sleep(min(2.0 * (i + 1), 8.0))
        return report
