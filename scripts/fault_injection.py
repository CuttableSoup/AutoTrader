#!/usr/bin/env python
"""Gate G2 fault-injection driver (docs/TESTING.md).

Assumes: nats-server running, streams bootstrapped, at-fake-alpaca on --fake,
and the C++ services (execution, portfolio, reconciler) started with a config
whose alpaca.* URLs point at the fake (see config/paper.fake.example.json).

Each scenario injects a fault, publishes what a real upstream would publish
(orders.approved with a deterministic client_order_id), then asserts what the
bus and the fake broker show afterwards.

usage: python scripts/fault_injection.py --scenario all|lost_response|http500|reject|ws_drop|mismatch
"""
from __future__ import annotations

import argparse
import asyncio
import hashlib
import json
import sys
import time
import uuid
from pathlib import Path

import httpx

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from autotrader.bus import Bus  # noqa: E402
from autotrader.envelope import Envelope, now_utc_iso  # noqa: E402
from autotrader.schemas import SchemaRegistry  # noqa: E402


def entry_coid(candidate_msg_id: str) -> str:
    return "ate-" + hashlib.sha256(("entry|" + candidate_msg_id).encode()).hexdigest()[:32]


def approved_entry(symbol: str, qty: int = 10, limit_cents: int = 12_500, stop_cents: int = 11_900) -> Envelope:
    cid = str(uuid.uuid4())
    p = {"client_order_id": entry_coid(cid), "candidate_msg_id": cid, "validated_msg_id": None, "intent": "ENTRY", "symbol": symbol, "side": "buy", "qty": qty, "order_type": "oto",
         "limit_px_cents": limit_cents, "stop_px_cents": stop_cents, "take_profit_px_cents": None, "linked_broker_order_id": None, "tif": "gtc", "extended_hours": False,
         "reason": "fault injection", "risk_checks": [{"name": "injected", "ok": True, "value": None, "limit": None}], "equity_at_approval_cents": 100_000_000, "atr20_cents": 200}
    return Envelope("risk", p, correlation_id=cid)


class Harness:
    def __init__(self, nats_url: str, fake_url: str):
        self.reg = SchemaRegistry()
        self.bus = Bus(nats_url, "fault-injector", self.reg)
        self.fake = fake_url.rstrip("/")
        self.seen: dict[str, list[dict]] = {"orders.submitted": [], "orders.status": [], "orders.filled": [], "broker.reconcile": [], "control.pause_new": [], "control.halt": []}
        self.http = httpx.Client(timeout=40)

    async def start(self) -> None:
        await self.bus.connect()
        tag = uuid.uuid4().hex[:6]
        for subj in self.seen:
            async def h(subject, env, key=subj):
                self.seen[key].append(env.to_dict())
            await self.bus.subscribe(subj, f"fi-{subj.replace('.', '-')}-{tag}", h)

    async def pump(self, seconds: float) -> None:
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            await self.bus.poll_once(timeout=0.2)
            await asyncio.sleep(0.05)

    def fault(self, kind: str, value=None) -> None:
        self.http.post(self.fake + "/__fault", json={"kind": kind, **({"value": value} if value is not None else {})}).raise_for_status()

    def clear_faults(self) -> None:
        self.http.delete(self.fake + "/__fault")

    def state(self) -> dict:
        return self.http.get(self.fake + "/__state").json()

    def reset(self) -> None:
        """Between scenarios: clear faults and observations only. Never reset the fake broker's state behind the
        system's back - the reconciler would (correctly) flag the ledger/broker mismatch and trip the kill switch."""
        self.clear_faults()
        for k in self.seen:
            self.seen[k].clear()

    async def flatten(self) -> None:
        """Close everything through the normal broker API so fills propagate to the ledger."""
        self.http.delete(self.fake + "/v2/positions?cancel_orders=true")
        await self.pump(4)

    async def wait_clean(self, rounds: int = 8) -> bool:
        """Force reconciles until one reports CLEAN (the portfolio service re-syncs from the broker every 15 s)."""
        for _ in range(rounds):
            n = len(self.seen["broker.reconcile"])
            await self.bus.publish("control.resume", Envelope("operator", {"command": "resume", "reason": "harness: force reconcile", "source": "operator", "issued_at_utc": now_utc_iso(), "details": {"reconcile": True, "clear_halt": True}}))
            await self.pump(12)
            new = self.seen["broker.reconcile"][n:]
            if new and new[-1]["payload"]["status"] == "CLEAN":
                return True
        print("wait_clean: never saw CLEAN; last:", self.seen["broker.reconcile"][-1]["payload"]["diffs"] if self.seen["broker.reconcile"] else None)
        return False

    def set_price(self, symbol: str, price: float) -> None:
        self.http.post(self.fake + "/__price", json={"symbol": symbol, "price": price}).raise_for_status()

    async def unhalt(self, watchdog_url: str, token: str) -> None:
        """Operator reset between runs: the mismatch scenario legitimately trips the kill switch and latches halts."""
        try:
            self.http.post(watchdog_url + "/resume", json={}, headers={"Authorization": f"Bearer {token}"})
        except Exception as e:  # noqa: BLE001
            print("watchdog resume skipped:", e)
        await self.bus.publish("control.resume", Envelope("operator", {"command": "resume", "reason": "fault-injection harness reset", "source": "operator", "issued_at_utc": now_utc_iso(), "details": {"clear_halt": True}}))
        await self.pump(3)
        self.clear_faults()

    def since(self, subject: str, coid: str | None = None) -> list[dict]:
        rows = self.seen[subject]
        return [r for r in rows if coid is None or r["payload"].get("client_order_id", "").startswith(coid)]


async def scenario_lost_response(h: Harness) -> bool:
    h.reset()
    h.set_price("FAKEA", 120.0)
    h.fault("drop_response_after_accept")
    env = approved_entry("FAKEA")
    coid = env.payload["client_order_id"]
    await h.bus.publish("orders.approved", env)
    await h.pump(45)
    sub = h.since("orders.submitted", coid)
    orders = [o for o in h.state()["orders"] if o["client_order_id"] == coid]
    ok = len(sub) == 1 and len(orders) == 1 and sub[0]["payload"]["deduped_at_broker"] and len(h.since("orders.filled", coid)) == 1
    print(f"lost_response: submitted={len(sub)} broker_orders={len(orders)} attempt={sub[0]['payload']['attempt'] if sub else None} deduped={sub[0]['payload']['deduped_at_broker'] if sub else None} -> {'PASS' if ok else 'FAIL'}")
    return ok


async def scenario_http500(h: Harness) -> bool:
    h.reset()
    h.set_price("FAKEB", 120.0)
    h.fault("http_500_next")
    env = approved_entry("FAKEB")
    coid = env.payload["client_order_id"]
    await h.bus.publish("orders.approved", env)
    await h.pump(15)
    sub = h.since("orders.submitted", coid)
    fills = h.since("orders.filled", coid)
    orders = [o for o in h.state()["orders"] if o["client_order_id"] == coid]
    ok = len(sub) == 1 and len(orders) == 1 and len(fills) == 1
    print(f"http500: submitted={len(sub)} fills={len(fills)} broker_orders={len(orders)} -> {'PASS' if ok else 'FAIL'}")
    return ok


async def scenario_reject(h: Harness) -> bool:
    h.reset()
    h.fault("reject_next_order")
    env = approved_entry("FAKEC")
    coid = env.payload["client_order_id"]
    await h.bus.publish("orders.approved", env)
    await h.pump(10)
    rej = [s for s in h.since("orders.status", coid) if s["payload"]["event"] == "rejected"]
    ok = len(rej) >= 1 and not h.since("orders.filled", coid)
    print(f"reject: rejected_status={len(rej)} fills={len(h.since('orders.filled', coid))} -> {'PASS' if ok else 'FAIL'}")
    return ok


async def scenario_ws_drop(h: Harness) -> bool:
    h.reset()
    h.fault("ws_disconnect")
    await h.pump(8)   # give the stream time to reconnect + re-listen
    h.set_price("FAKED", 120.0)
    env = approved_entry("FAKED")
    coid = env.payload["client_order_id"]
    await h.bus.publish("orders.approved", env)
    await h.pump(15)
    ok = len(h.since("orders.filled", coid)) == 1 and h.state()["ws_clients"] >= 1
    print(f"ws_drop: fills_after_reconnect={len(h.since('orders.filled', coid))} ws_clients={h.state()['ws_clients']} -> {'PASS' if ok else 'FAIL'}")
    return ok


async def scenario_mismatch(h: Harness) -> bool:
    h.reset()
    h.fault("corrupt_position")
    await h.bus.publish("control.resume", Envelope("operator", {"command": "resume", "reason": "force reconcile", "source": "operator", "issued_at_utc": now_utc_iso(), "details": {"reconcile": True}}))
    await h.pump(50)   # the ghost position cannot be closed, so the watchdog exhausts its 5 verify attempts (~30 s) before announcing
    rec = [r for r in h.seen["broker.reconcile"] if r["payload"]["status"] == "MISMATCH"]
    pauses = [p for p in h.seen["control.pause_new"] if p["payload"].get("trigger") == "RECONCILE_MISMATCH"]
    ghost = any(d["field"] == "missing_internal" and d["symbol"] == "GHOST" for r in rec for d in r["payload"]["diffs"])
    halts = [x for x in h.seen["control.halt"] if x["payload"].get("trigger") == "RECONCILE_MISMATCH"]
    ok = ghost and bool(pauses) and bool(halts)   # pause + page on the bus, and the watchdog kill path announced control.halt
    h.clear_faults()
    print(f"mismatch: reconcile_mismatch={len(rec)} ghost_diff={ghost} pause_new={len(pauses)} watchdog_halt={len(halts)} -> {'PASS' if ok else 'FAIL'}")
    return ok


SCENARIOS = {"lost_response": scenario_lost_response, "http500": scenario_http500, "reject": scenario_reject, "ws_drop": scenario_ws_drop, "mismatch": scenario_mismatch}


async def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--nats", default="nats://127.0.0.1:4222")
    ap.add_argument("--fake", default="http://127.0.0.1:8790")
    ap.add_argument("--scenario", default="all")
    ap.add_argument("--watchdog", default="http://127.0.0.1:8787")
    ap.add_argument("--token", default="fake-token")
    a = ap.parse_args()
    h = Harness(a.nats, a.fake)
    await h.start()
    await h.unhalt(a.watchdog, a.token)
    await h.flatten()
    await h.wait_clean()
    await h.unhalt(a.watchdog, a.token)
    names = list(SCENARIOS) if a.scenario == "all" else [a.scenario]
    results = {n: await SCENARIOS[n](h) for n in names}
    await h.unhalt(a.watchdog, a.token)
    await h.flatten()
    await h.bus.close()
    print(json.dumps(results, indent=1))
    return 0 if all(results.values()) else 2


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
