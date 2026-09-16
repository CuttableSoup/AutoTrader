"""Fake Alpaca (trading + market data + trade_updates WebSocket) with fault injection.

Enough of the API surface for the execution engine, portfolio service,
reconciler, ingestor and watchdog to run against it, plus knobs to break
things on purpose (docs/TESTING.md, Gate G2):

  POST /__fault   {"kind": "drop_next_response" | "http_500_next" | "delay_ms" | "reject_next_order"
                   | "ws_disconnect" | "corrupt_position" | "ws_drop_next_update", ...}
  POST /__price   {"symbol": "ABC", "price": 98.5}   moves the last price and triggers resting stops
  POST /__reset   {"equity": 1000000}
  GET  /__state

Run: at-fake-alpaca --port 8790
"""
from __future__ import annotations

import argparse
import asyncio
import datetime as dt
import json
import logging
import random
import uuid
from typing import Any

from aiohttp import WSMsgType, web

log = logging.getLogger("autotrader.fake_alpaca")


def now_iso() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")


class FakeAlpaca:
    def __init__(self, equity: float = 1_000_000.0):
        self.reset(equity)
        self.ws_clients: list[web.WebSocketResponse] = []
        self.faults: dict[str, Any] = {}
        self.request_log: list[dict] = []

    def reset(self, equity: float) -> None:
        self.cash = equity
        self.positions: dict[str, dict] = {}
        self.orders: dict[str, dict] = {}
        self.by_coid: dict[str, str] = {}
        self.prices: dict[str, float] = {}
        self.calls = 0

    # ---- helpers ----
    def price(self, sym: str) -> float:
        return self.prices.setdefault(sym, 100.0 + (sum(map(ord, sym)) % 50))

    def equity(self) -> float:
        return self.cash + sum(p["qty"] * self.price(s) for s, p in self.positions.items())

    def account(self) -> dict:
        eq = self.equity()
        return {"id": "fake", "status": "ACTIVE", "currency": "USD", "cash": f"{self.cash:.2f}", "equity": f"{eq:.2f}", "last_equity": f"{eq:.2f}",
                "buying_power": f"{max(self.cash, 0):.2f}", "non_marginable_buying_power": f"{max(self.cash, 0):.2f}", "portfolio_value": f"{eq:.2f}",
                "multiplier": "1", "trading_blocked": False, "account_blocked": False}

    def position_json(self, sym: str) -> dict:
        p = self.positions[sym]
        px = self.price(sym)
        return {"symbol": sym, "qty": str(p["qty"]), "avg_entry_price": f"{p['avg']:.4f}", "side": "long", "market_value": f"{p['qty'] * px:.2f}",
                "current_price": f"{px:.2f}", "unrealized_pl": f"{p['qty'] * (px - p['avg']):.2f}", "asset_class": "us_equity", "exchange": "NASDAQ"}

    def order_json(self, o: dict) -> dict:
        legs = [self.order_json(self.orders[l]) for l in o.get("leg_ids", [])] or None
        return {"id": o["id"], "client_order_id": o["client_order_id"], "symbol": o["symbol"], "qty": str(o["qty"]), "filled_qty": str(o["filled_qty"]),
                "side": o["side"], "type": o["type"], "time_in_force": o["tif"], "limit_price": o.get("limit_price"), "stop_price": o.get("stop_price"),
                "status": o["status"], "order_class": o.get("order_class", "simple"), "filled_avg_price": o.get("filled_avg_price"), "legs": legs,
                "created_at": o["created_at"], "updated_at": now_iso(), "submitted_at": o["created_at"], "filled_at": o.get("filled_at"), "parent_id": o.get("parent_id"),
                "extended_hours": False, "asset_class": "us_equity"}

    async def push(self, event: str, o: dict, price: float | None = None, qty: int | None = None) -> None:
        if self.faults.pop("ws_drop_next_update", None):
            log.warning("FAULT: dropping trade_update %s %s", event, o["client_order_id"])
            return
        pos_qty = self.positions.get(o["symbol"], {}).get("qty", 0)
        msg = {"stream": "trade_updates", "data": {"event": event, "order": self.order_json(o), "timestamp": now_iso(), "position_qty": str(pos_qty)}}
        if price is not None:
            msg["data"]["price"] = f"{price:.2f}"
            msg["data"]["qty"] = str(qty)
        for ws in list(self.ws_clients):
            try:
                await ws.send_str(json.dumps(msg))
            except Exception:  # noqa: BLE001
                self.ws_clients.remove(ws)

    async def fill(self, o: dict, px: float) -> None:
        qty = o["qty"]
        if o["side"] == "buy":
            p = self.positions.setdefault(o["symbol"], {"qty": 0, "avg": 0.0})
            p["avg"] = (p["avg"] * p["qty"] + px * qty) / (p["qty"] + qty)
            p["qty"] += qty
            self.cash -= px * qty
        else:
            p = self.positions.get(o["symbol"])
            if not p or p["qty"] < qty:
                o["status"] = "rejected"
                await self.push("rejected", o)
                return
            p["qty"] -= qty
            self.cash += px * qty
            if p["qty"] == 0:
                del self.positions[o["symbol"]]
        o["filled_qty"] = qty
        o["filled_avg_price"] = f"{px:.2f}"
        o["status"] = "filled"
        o["filled_at"] = now_iso()
        await self.push("fill", o, px, qty)
        # Activate legs (oto/bracket) once the parent fills; cancel sibling legs when one fills.
        for lid in o.get("leg_ids", []):
            leg = self.orders[lid]
            if leg["status"] == "held":
                leg["status"] = "new"
                await self.push("new", leg)
        if o.get("parent_id"):
            parent = self.orders[o["parent_id"]]
            for sid in parent.get("leg_ids", []):
                sib = self.orders[sid]
                if sid != o["id"] and sib["status"] in ("new", "held", "accepted"):
                    sib["status"] = "canceled"
                    await self.push("canceled", sib)

    async def check_stops(self, sym: str) -> None:
        px = self.price(sym)
        for o in list(self.orders.values()):
            if o["symbol"] == sym and o["status"] == "new" and o["type"] in ("stop", "stop_limit") and o["side"] == "sell" and px <= float(o["stop_price"]):
                await self.fill(o, px)

    # ---- fault gate ----
    async def gate(self, request: web.Request) -> web.Response | None:
        self.calls += 1
        self.request_log.append({"m": request.method, "p": request.path, "t": now_iso()})
        if "delay_ms" in self.faults:
            await asyncio.sleep(self.faults["delay_ms"] / 1000.0)
        if self.faults.pop("drop_next_response", None):
            log.warning("FAULT: dropping response to %s %s", request.method, request.path)
            await asyncio.sleep(30)
            return web.Response(status=504, text="dropped")
        if self.faults.pop("http_500_next", None):
            return web.json_response({"message": "internal server error (injected)"}, status=500)
        return None


def make_app(fa: FakeAlpaca) -> web.Application:
    app = web.Application()
    r = app.router

    async def account(req):
        return (await fa.gate(req)) or web.json_response(fa.account())

    async def positions(req):
        if (g := await fa.gate(req)):
            return g
        out = [fa.position_json(s) for s in fa.positions]
        if fa.faults.get("corrupt_position"):
            out.append({"symbol": "GHOST", "qty": "17", "avg_entry_price": "10.0000", "side": "long", "market_value": "170.00", "current_price": "10.00", "unrealized_pl": "0", "asset_class": "us_equity", "exchange": "NYSE"})
        return web.json_response(out)

    async def close_all(req):
        if (g := await fa.gate(req)):
            return g
        # cancel_orders=true semantics: cancel every open order first
        for o in fa.orders.values():
            if o["status"] in ("new", "accepted", "held", "partially_filled"):
                o["status"] = "canceled"
                await fa.push("canceled", o)
        out = []
        for sym in list(fa.positions):
            o = new_order(fa, {"symbol": sym, "qty": fa.positions[sym]["qty"], "side": "sell", "type": "market", "time_in_force": "day", "client_order_id": f"closeall-{uuid.uuid4().hex[:12]}"})
            await fa.fill(o, fa.price(sym))
            out.append({"symbol": sym, "status": 200, "body": fa.order_json(o)})
        return web.json_response(out, status=207)

    async def close_one(req):
        if (g := await fa.gate(req)):
            return g
        sym = req.match_info["sym"]
        if sym not in fa.positions:
            return web.json_response({"message": "position does not exist"}, status=404)
        o = new_order(fa, {"symbol": sym, "qty": fa.positions[sym]["qty"], "side": "sell", "type": "market", "time_in_force": "day", "client_order_id": f"close-{uuid.uuid4().hex[:12]}"})
        await fa.fill(o, fa.price(sym))
        return web.json_response(fa.order_json(o))

    async def list_orders(req):
        if (g := await fa.gate(req)):
            return g
        status = req.query.get("status", "open")
        want = {"open": ("new", "accepted", "held", "partially_filled", "pending_new"), "closed": ("filled", "canceled", "rejected", "expired", "replaced"), "all": None}[status]
        nested = req.query.get("nested", "true") != "false"
        out = []
        for o in fa.orders.values():
            matches = want is None or o["status"] in want
            if o.get("parent_id") and nested:
                # Like Alpaca: a leg rolls up under its parent when the parent is in the result set, otherwise it is listed itself.
                parent = fa.orders.get(o["parent_id"])
                parent_listed = parent is not None and (want is None or parent["status"] in want)
                if matches and not parent_listed:
                    out.append(fa.order_json(o))
            elif matches:
                out.append(fa.order_json(o))
        return web.json_response(out)

    async def post_order(req):
        if (g := await fa.gate(req)):
            return g
        body = await req.json()
        coid = body.get("client_order_id") or uuid.uuid4().hex
        if coid in fa.by_coid:
            return web.json_response({"code": 40010001, "message": "client_order_id must be unique"}, status=422)
        if fa.faults.pop("reject_next_order", None):
            o = new_order(fa, body)
            o["status"] = "rejected"
            await fa.push("rejected", o)
            return web.json_response({"code": 40310000, "message": "insufficient buying power (injected)"}, status=403)
        o = new_order(fa, body)
        await fa.push("new", o)
        px = fa.price(o["symbol"])
        marketable = o["type"] == "market" or (o["type"] == "limit" and ((o["side"] == "buy" and px <= float(o["limit_price"])) or (o["side"] == "sell" and px >= float(o["limit_price"]))))
        if marketable:
            await fa.fill(o, px)
        if fa.faults.pop("drop_response_after_accept", None):
            log.warning("FAULT: order %s accepted, dropping the response", o["client_order_id"])
            await asyncio.sleep(30)
            return web.Response(status=504, text="dropped after accept")
        return web.json_response(fa.order_json(o))

    async def get_order(req):
        if (g := await fa.gate(req)):
            return g
        o = fa.orders.get(req.match_info["id"])
        return web.json_response(fa.order_json(o)) if o else web.json_response({"message": "order not found"}, status=404)

    async def get_by_coid(req):
        if (g := await fa.gate(req)):
            return g
        oid = fa.by_coid.get(req.query.get("client_order_id", ""))
        return web.json_response(fa.order_json(fa.orders[oid])) if oid else web.json_response({"message": "order not found"}, status=404)

    async def patch_order(req):
        if (g := await fa.gate(req)):
            return g
        o = fa.orders.get(req.match_info["id"])
        if not o or o["status"] not in ("new", "accepted", "held"):
            return web.json_response({"message": "order not replaceable"}, status=422)
        body = await req.json()
        no = dict(o, id=uuid.uuid4().hex, client_order_id=body.get("client_order_id") or o["client_order_id"] + "-r", created_at=now_iso())
        for k in ("qty", "limit_price", "stop_price", "time_in_force"):
            if k in body:
                no["qty" if k == "qty" else k] = int(body[k]) if k == "qty" else body[k]
        o["status"] = "replaced"
        o["replaced_by"] = no["id"]
        fa.orders[no["id"]] = no
        fa.by_coid[no["client_order_id"]] = no["id"]
        if o.get("parent_id"):
            parent = fa.orders[o["parent_id"]]
            parent["leg_ids"] = [no["id"] if l == o["id"] else l for l in parent["leg_ids"]]
        await fa.push("replaced", o)
        await fa.push("new", no)
        return web.json_response(fa.order_json(no))

    async def cancel_order(req):
        if (g := await fa.gate(req)):
            return g
        o = fa.orders.get(req.match_info["id"])
        if not o:
            return web.json_response({"message": "order not found"}, status=404)
        if o["status"] in ("new", "accepted", "held", "partially_filled"):
            o["status"] = "canceled"
            await fa.push("canceled", o)
        return web.Response(status=204)

    async def cancel_all(req):
        if (g := await fa.gate(req)):
            return g
        out = []
        for o in fa.orders.values():
            if o["status"] in ("new", "accepted", "held", "partially_filled"):
                o["status"] = "canceled"
                await fa.push("canceled", o)
                out.append({"id": o["id"], "status": 200})
        return web.json_response(out, status=207)

    async def clock(req):
        if (g := await fa.gate(req)):
            return g
        now = dt.datetime.now(dt.timezone.utc)
        return web.json_response({"timestamp": now_iso(), "is_open": True, "next_open": now_iso(), "next_close": now_iso()})

    async def calendar(req):
        if (g := await fa.gate(req)):
            return g
        start = dt.date.fromisoformat(req.query.get("start", dt.date.today().isoformat()))
        end = dt.date.fromisoformat(req.query.get("end", start.isoformat()))
        days = []
        d = start
        while d <= end:
            if d.weekday() < 5:
                days.append({"date": d.isoformat(), "open": "09:30", "close": "16:00"})
            d += dt.timedelta(days=1)
        return web.json_response(days)

    async def bars(req):
        if (g := await fa.gate(req)):
            return g
        syms = req.query.get("symbols", "").split(",")
        start = dt.date.fromisoformat(req.query.get("start", "2025-01-01")[:10])
        end = dt.date.fromisoformat(req.query.get("end", dt.date.today().isoformat())[:10])
        out: dict[str, list] = {}
        for s in syms:
            rng = random.Random(s)
            px = fa.price(s)
            d = start
            rows = []
            while d <= end:
                if d.weekday() < 5:
                    o = px * (1 + rng.uniform(-0.01, 0.01))
                    c = px * (1 + rng.uniform(-0.01, 0.01))
                    rows.append({"t": f"{d.isoformat()}T04:00:00Z", "o": round(o, 2), "h": round(max(o, c) * 1.005, 2), "l": round(min(o, c) * 0.995, 2), "c": round(c, 2), "v": 1_500_000, "n": 1000, "vw": round(c, 2)})
                d += dt.timedelta(days=1)
            out[s] = rows
        return web.json_response({"bars": out, "next_page_token": None})

    async def quotes(req):
        if (g := await fa.gate(req)):
            return g
        out = {}
        for s in req.query.get("symbols", "").split(","):
            px = fa.price(s)
            out[s] = {"t": now_iso(), "bp": round(px * 0.9998, 2), "bs": 5, "ap": round(px * 1.0002, 2), "as": 5, "ax": "V", "bx": "V"}
        return web.json_response({"quotes": out})

    async def stream(req):
        ws = web.WebSocketResponse(heartbeat=20)
        await ws.prepare(req)
        fa.ws_clients.append(ws)
        try:
            async for msg in ws:
                if msg.type != WSMsgType.TEXT:
                    continue
                m = json.loads(msg.data)
                if m.get("action") in ("auth", "authenticate"):
                    await ws.send_str(json.dumps({"stream": "authorization", "data": {"action": "authenticate", "status": "authorized"}}))
                elif m.get("action") == "listen":
                    await ws.send_str(json.dumps({"stream": "listening", "data": {"streams": m.get("data", {}).get("streams", [])}}))
                if fa.faults.pop("ws_disconnect", None):
                    log.warning("FAULT: closing websocket")
                    await ws.close()
        finally:
            if ws in fa.ws_clients:
                fa.ws_clients.remove(ws)
        return ws

    async def fault(req):
        body = await req.json()
        kind = body.pop("kind")
        fa.faults[kind] = body.get("value", True) if kind != "delay_ms" else int(body.get("value", 1000))
        if kind == "ws_disconnect":
            for ws in list(fa.ws_clients):
                await ws.close()
        return web.json_response({"faults": fa.faults})

    async def clear_fault(req):
        fa.faults.clear()
        return web.json_response({"faults": {}})

    async def set_price(req):
        body = await req.json()
        fa.prices[body["symbol"]] = float(body["price"])
        await fa.check_stops(body["symbol"])
        return web.json_response({"symbol": body["symbol"], "price": fa.prices[body["symbol"]], "equity": fa.equity()})

    async def state(req):
        return web.json_response({"account": fa.account(), "positions": [fa.position_json(s) for s in fa.positions], "orders": [fa.order_json(o) for o in fa.orders.values()],
                                  "faults": fa.faults, "calls": fa.calls, "ws_clients": len(fa.ws_clients), "recent_requests": fa.request_log[-20:]})

    async def reset(req):
        body = await req.json() if req.can_read_body else {}
        fa.reset(float(body.get("equity", 1_000_000)))
        fa.faults.clear()
        return web.json_response(fa.account())

    r.add_get("/v2/account", account)
    r.add_get("/v2/positions", positions)
    r.add_delete("/v2/positions", close_all)
    r.add_delete("/v2/positions/{sym}", close_one)
    r.add_get("/v2/orders", list_orders)
    r.add_post("/v2/orders", post_order)
    r.add_get("/v2/orders:by_client_order_id", get_by_coid)
    r.add_get("/v2/orders/{id}", get_order)
    r.add_patch("/v2/orders/{id}", patch_order)
    r.add_delete("/v2/orders/{id}", cancel_order)
    r.add_delete("/v2/orders", cancel_all)
    r.add_get("/v2/clock", clock)
    r.add_get("/v2/calendar", calendar)
    r.add_get("/v2/stocks/bars", bars)
    r.add_get("/v2/stocks/quotes/latest", quotes)
    r.add_get("/stream", stream)
    r.add_post("/__fault", fault)
    r.add_delete("/__fault", clear_fault)
    r.add_post("/__price", set_price)
    r.add_get("/__state", state)
    r.add_post("/__reset", reset)
    return app


def new_order(fa: FakeAlpaca, body: dict) -> dict:
    oid = uuid.uuid4().hex
    o = {"id": oid, "client_order_id": body.get("client_order_id") or oid, "symbol": body["symbol"], "qty": int(float(body["qty"])), "filled_qty": 0, "side": body["side"],
         "type": body.get("type", "market"), "tif": body.get("time_in_force", "day"), "limit_price": body.get("limit_price"), "stop_price": body.get("stop_price"),
         "status": "new", "order_class": body.get("order_class", "simple"), "created_at": now_iso(), "leg_ids": []}
    fa.orders[oid] = o
    fa.by_coid[o["client_order_id"]] = oid
    if o["order_class"] in ("oto", "bracket"):
        if "stop_loss" in body:
            leg = {"id": uuid.uuid4().hex, "client_order_id": o["client_order_id"] + "-sl", "symbol": o["symbol"], "qty": o["qty"], "filled_qty": 0, "side": "sell", "type": "stop", "tif": o["tif"],
                   "limit_price": None, "stop_price": body["stop_loss"]["stop_price"], "status": "held", "order_class": o["order_class"], "created_at": now_iso(), "leg_ids": [], "parent_id": oid}
            fa.orders[leg["id"]] = leg
            fa.by_coid[leg["client_order_id"]] = leg["id"]
            o["leg_ids"].append(leg["id"])
        if "take_profit" in body:
            leg = {"id": uuid.uuid4().hex, "client_order_id": o["client_order_id"] + "-tp", "symbol": o["symbol"], "qty": o["qty"], "filled_qty": 0, "side": "sell", "type": "limit", "tif": o["tif"],
                   "limit_price": body["take_profit"]["limit_price"], "stop_price": None, "status": "held", "order_class": o["order_class"], "created_at": now_iso(), "leg_ids": [], "parent_id": oid}
            fa.orders[leg["id"]] = leg
            fa.by_coid[leg["client_order_id"]] = leg["id"]
            o["leg_ids"].append(leg["id"])
    return o


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8790)
    ap.add_argument("--equity", type=float, default=1_000_000)
    a = ap.parse_args()
    logging.basicConfig(level="INFO", format="%(asctime)s %(name)s %(levelname)s %(message)s")
    web.run_app(make_app(FakeAlpaca(a.equity)), host=a.host, port=a.port)


if __name__ == "__main__":
    main()
