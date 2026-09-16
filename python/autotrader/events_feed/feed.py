"""Earnings/event feed: polls FMP and Finnhub, cross-checks BMO/AMC, snapshots
consensus so it is always as-of the day before the report, and publishes
events.earnings (once per change).

Rules enforced here (docs/DESIGN.md section 8):
  * consensus is the estimate snapshot taken on or before report_date - 1
  * EPS is as-reported at first publication; later restatements never overwrite
  * timing is BMO/AMC only when two vendors agree; otherwise UNKNOWN

Run: at-events-feed --config config/paper.json [--once] [--from 2026-09-01 --to 2026-12-31]
"""
from __future__ import annotations

import argparse
import asyncio
import datetime as dt
import hashlib
import json
import logging
from pathlib import Path
from typing import Any

from ..bus import Bus
from ..config import Config, Secrets
from ..envelope import Envelope, now_utc_iso
from ..schemas import SchemaRegistry
from .vendors import FinnhubClient, FmpClient, VendorEvent

log = logging.getLogger("autotrader.events.feed")


def event_id(symbol: str, fiscal_period: str, report_date: dt.date) -> str:
    return hashlib.sha256(f"{symbol}|{fiscal_period}|{report_date.isoformat()}".encode()).hexdigest()[:32]


def to_cents(dollars: float | None) -> int | None:
    return None if dollars is None else int(round(dollars * 100))


class FeedState:
    """Persisted: consensus snapshots per event, published content hashes, first-reported actuals."""

    def __init__(self, path: Path):
        self.path = path
        self.data: dict[str, Any] = {"consensus": {}, "published": {}, "actuals": {}}
        try:
            self.data = json.loads(path.read_text(encoding="utf-8"))
        except FileNotFoundError:
            pass

    def save(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.path.write_text(json.dumps(self.data), encoding="utf-8")

    def snapshot_consensus(self, eid: str, eps: float | None, revenue: float | None, today: dt.date) -> None:
        if eps is None and revenue is None:
            return
        snaps = self.data["consensus"].setdefault(eid, [])
        if snaps and snaps[-1]["asof"] == today.isoformat():
            snaps[-1].update({"eps": eps, "revenue": revenue})
        else:
            snaps.append({"asof": today.isoformat(), "eps": eps, "revenue": revenue})

    def consensus_asof(self, eid: str, cutoff: dt.date) -> tuple[float | None, float | None, str | None]:
        best = None
        for s in self.data["consensus"].get(eid, []):
            if dt.date.fromisoformat(s["asof"]) <= cutoff:
                best = s
        return (best["eps"], best["revenue"], best["asof"]) if best else (None, None, None)

    def first_actuals(self, eid: str, eps: float | None, revenue: float | None) -> tuple[float | None, float | None]:
        rec = self.data["actuals"].get(eid)
        if rec is None and (eps is not None or revenue is not None):
            rec = {"eps": eps, "revenue": revenue}
            self.data["actuals"][eid] = rec
        return (rec["eps"], rec["revenue"]) if rec else (None, None)


def merge_vendor_events(fmp: list[VendorEvent], finnhub: list[VendorEvent]) -> dict[tuple[str, dt.date], list[VendorEvent]]:
    """Group by (symbol, report_date) allowing a +/-1 day disagreement (FMP wins the date)."""
    groups: dict[tuple[str, dt.date], list[VendorEvent]] = {}
    for e in fmp:
        groups.setdefault((e.symbol, e.report_date), []).append(e)
    for e in finnhub:
        placed = False
        for delta in (0, 1, -1):
            key = (e.symbol, e.report_date + dt.timedelta(days=delta))
            if key in groups:
                groups[key].append(e)
                placed = True
                break
        if not placed:
            groups.setdefault((e.symbol, e.report_date), []).append(e)
    return groups


def build_payload(symbol: str, report_date: dt.date, sources: list[VendorEvent], state: FeedState, today: dt.date, next_report: dt.date | None) -> dict[str, Any]:
    timings = {s.vendor: s.timing for s in sources}
    known = [t for t in timings.values() if t != "UNKNOWN"]
    if len(set(known)) > 1:
        timing = "UNKNOWN"   # explicit disagreement: day 0 is not trusted
        log.warning("%s %s: vendors disagree on timing %s -> UNKNOWN", symbol, report_date, timings)
    elif len(known) >= 1 and len(sources) >= 2:
        timing = known[0]    # at least one vendor states it and no vendor contradicts it (FMP's stable calendar carries no time field)
    else:
        timing = "UNKNOWN"   # single vendor only, or nobody knows
    fiscal = next((s.fiscal_period for s in sources if s.fiscal_period), "")
    eid = event_id(symbol, fiscal, report_date)
    # Consensus snapshot (today) then read the as-of value.
    est_eps = next((s.eps_estimate for s in sources if s.eps_estimate is not None), None)
    est_rev = next((s.revenue_estimate for s in sources if s.revenue_estimate is not None), None)
    if today < report_date:
        state.snapshot_consensus(eid, est_eps, est_rev, today)
    eps_c, rev_c, asof = state.consensus_asof(eid, report_date - dt.timedelta(days=1))
    act_eps = next((s.eps_actual for s in sources if s.eps_actual is not None), None)
    act_rev = next((s.revenue_actual for s in sources if s.revenue_actual is not None), None)
    act_eps, act_rev = state.first_actuals(eid, act_eps, act_rev)
    return {
        "event_id": eid,
        "symbol": symbol,
        "report_date": report_date.isoformat(),
        "timing": timing,
        "timing_sources": [{"vendor": s.vendor, "timing": s.timing, "report_date": s.report_date.isoformat()} for s in sources],
        "fiscal_period": fiscal,
        "eps_actual": act_eps,
        "eps_consensus": eps_c,
        "eps_consensus_asof": asof,
        "revenue_actual_cents": to_cents(act_rev),
        "revenue_consensus_cents": to_cents(rev_c),
        "next_report_date": next_report.isoformat() if next_report else None,
        "material_8k_dates": [],
        "source": "fmp" if any(s.vendor == "fmp" for s in sources) else "finnhub",
        "as_of_utc": now_utc_iso(),
    }


def content_hash(p: dict[str, Any]) -> str:
    stable = {k: v for k, v in p.items() if k not in ("as_of_utc",)}
    return hashlib.sha256(json.dumps(stable, sort_keys=True).encode()).hexdigest()[:16]


class EventsFeed:
    def __init__(self, cfg: Config, secrets: Secrets):
        self.cfg = cfg
        self.reg = SchemaRegistry(cfg.resolve(cfg.get("schemas_dir", "schemas")))
        self.bus = Bus(cfg.get("nats_url", "nats://127.0.0.1:4222"), "events-feed", self.reg)
        self.state = FeedState(cfg.resolve(cfg.get("state_dir", "var/state")) / "events_feed_state.json")
        vendors = cfg.get("events_feed.vendors", ["fmp", "finnhub"])
        self.fmp = FmpClient(secrets.require("fmp_api_key")) if "fmp" in vendors else None
        self.finnhub = FinnhubClient(secrets.require("finnhub_api_key")) if "finnhub" in vendors else None
        self.universe_path = cfg.resolve(cfg.get("state_dir", "var/state")) / "universe.json"

    def universe_symbols(self) -> set[str] | None:
        try:
            return set(json.loads(self.universe_path.read_text(encoding="utf-8"))["symbols"])
        except (FileNotFoundError, KeyError, ValueError):
            return None

    async def poll(self, start: dt.date, end: dt.date) -> int:
        today = dt.date.today()
        fmp_ev = await asyncio.to_thread(self.fmp.earnings_calendar, start, end) if self.fmp else []
        fh_ev = await asyncio.to_thread(self.finnhub.earnings_calendar, start, end) if self.finnhub else []
        uni = self.universe_symbols()
        groups = merge_vendor_events(fmp_ev, fh_ev)
        by_symbol: dict[str, list[dt.date]] = {}
        for (sym, d) in groups:
            by_symbol.setdefault(sym, []).append(d)
        published = 0
        for (sym, d), sources in sorted(groups.items()):
            if uni is not None and sym not in uni:
                continue
            later = sorted(x for x in by_symbol[sym] if x > d)
            payload = build_payload(sym, d, sources, self.state, today, later[0] if later else None)
            h = content_hash(payload)
            if self.state.data["published"].get(payload["event_id"]) == h:
                continue
            await self.bus.publish("events.earnings", Envelope("events-feed", payload))
            self.state.data["published"][payload["event_id"]] = h
            published += 1
        self.state.save()
        log.info("poll %s..%s: %d groups, %d published", start, end, len(groups), published)
        return published

    async def run(self, once: bool, start: dt.date | None, end: dt.date | None) -> None:
        await self.bus.connect()
        interval = int(self.cfg.get("events_feed.poll_interval_s", 900))
        while True:
            today = dt.date.today()
            try:
                await self.poll(start or today - dt.timedelta(days=7), end or today + dt.timedelta(days=120))
            except Exception as e:  # noqa: BLE001
                log.exception("poll failed: %s", e)
            if once:
                break
            await asyncio.sleep(interval)
        await self.bus.close()


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default=None)
    ap.add_argument("--once", action="store_true")
    ap.add_argument("--from", dest="start", default=None)
    ap.add_argument("--to", dest="end", default=None)
    ap.add_argument("--log-level", default="INFO")
    a = ap.parse_args()
    logging.basicConfig(level=a.log_level, format="%(asctime)s %(name)s %(levelname)s %(message)s")
    cfg = Config.load(a.config)
    feed = EventsFeed(cfg, Secrets.load(cfg))
    asyncio.run(feed.run(a.once, dt.date.fromisoformat(a.start) if a.start else None, dt.date.fromisoformat(a.end) if a.end else None))


if __name__ == "__main__":
    main()
