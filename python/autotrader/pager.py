"""Pager: subscribes to control.* and broker.reconcile and pages on the
conditions in docs/DESIGN.md section 10. Backends: log (default) | webhook.

Run: at-pager --config config/paper.json
"""
from __future__ import annotations

import argparse
import asyncio
import json
import logging

import httpx

from .bus import Bus
from .config import Config
from .envelope import Envelope
from .schemas import SchemaRegistry

log = logging.getLogger("autotrader.pager")

PAGE_TRIGGERS = {"VALIDATOR_ERROR_STREAK", "HEARTBEAT_LOSS", "RECONCILE_MISMATCH", "DAILY_LOSS", "DRAWDOWN_HALVE", "DRAWDOWN_FLATTEN", "DATA_STALE", "REJECT_RATE", "CONSECUTIVE_LOSERS"}


class Pager:
    def __init__(self, backend: str = "log", webhook_url: str = ""):
        self.backend = backend
        self.webhook_url = webhook_url

    async def page(self, title: str, details: dict) -> None:
        body = {"title": title, "details": details}
        log.critical("PAGE: %s %s", title, json.dumps(details)[:800])
        if self.backend == "webhook" and self.webhook_url:
            try:
                async with httpx.AsyncClient(timeout=10) as c:
                    await c.post(self.webhook_url, json=body)
            except Exception as e:  # noqa: BLE001
                log.error("webhook page failed: %s", e)


async def run(cfg: Config) -> None:
    reg = SchemaRegistry(cfg.resolve(cfg.get("schemas_dir", "schemas")))
    pager = Pager(cfg.get("paging.backend", "log"), cfg.get("paging.webhook_url", ""))
    bus = Bus(cfg.get("nats_url", "nats://127.0.0.1:4222"), "pager", reg)
    await bus.connect()

    async def on_control(subject: str, env: Envelope) -> None:
        p = env.payload
        trig = p.get("trigger")
        if p.get("command") in ("halt", "flatten") or trig in PAGE_TRIGGERS:
            await pager.page(f"{subject} [{trig}] {p.get('reason', '')}", {"source": p.get("source"), "issued_at_utc": p.get("issued_at_utc"), "drill": p.get("drill", False), "details": p.get("details", {})})

    async def on_reconcile(subject: str, env: Envelope) -> None:
        if env.payload.get("status") != "CLEAN":
            await pager.page(f"broker.reconcile {env.payload.get('status')}", {"diffs": env.payload.get("diffs", []), "orphan_orders": env.payload.get("orphan_orders", [])})

    await bus.subscribe("control.>", "pager-control", on_control)
    await bus.subscribe("broker.reconcile", "pager-reconcile", on_reconcile)
    await bus.run()


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default=None)
    ap.add_argument("--log-level", default="INFO")
    a = ap.parse_args()
    logging.basicConfig(level=a.log_level, format="%(asctime)s %(name)s %(levelname)s %(message)s")
    asyncio.run(run(Config.load(a.config)))


if __name__ == "__main__":
    main()
