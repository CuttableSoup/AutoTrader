"""Universe builder CLI.

  at-universe backtest --data-dir data/sharadar --out data/sharadar   # writes bars.csv, securities.csv, earnings.csv
  at-universe live --config config/paper.json                          # writes var/state/universe.json for the live services

The membership rules themselves live in C++ (cpp/src/strategy/universe.cpp)
so backtest and live use one implementation; this module only prepares the
point-in-time inputs and, for live, applies the same rules in Python to
produce the monthly snapshot (cross-checked against the C++ output in tests).
"""
from __future__ import annotations

import argparse
import csv
import datetime as dt
import hashlib
import json
import logging
import statistics
from pathlib import Path

from ..config import Config, Secrets
from .sharadar import bulk_download, write_bars_csv, write_events_from_sharadar, write_securities_csv

log = logging.getLogger("autotrader.universe")


def load_strategy(cfg: Config) -> dict:
    return json.loads(cfg.resolve(cfg.get("strategy_config", "config/strategy.v1.json")).read_text(encoding="utf-8"))


def exclusion_reason(sec: dict, adv20_dollars: float | None, as_of: dt.date, u: dict) -> str:
    """Mirror of cpp/src/strategy/universe.cpp::universe_exclusion_reason (same order, same names)."""
    cat = (sec.get("category") or "").lower()
    if sec["symbol"] in set(u.get("exclude_over_optioned", [])):
        return "over_optioned"
    if u.get("exclude_etfs", True) and ("etf" in cat or "etn" in cat):
        return "etf"
    if u.get("exclude_adrs", True) and "adr" in cat:
        return "adr"
    if "common stock" not in cat or "adr" in cat:
        return "not_common_stock"
    if float(sec.get("market_cap", 0)) * 100 <= u["min_market_cap_cents"]:
        return "market_cap"
    fl = sec.get("first_listed") or ""
    if not fl:
        return "listing_date_unknown"
    if (as_of - dt.date.fromisoformat(fl[:10])).days < u.get("min_listing_age_days", 365):
        return "ipo_age"
    if int(sec.get("analyst_coverage", 0)) < u.get("min_analyst_coverage", 5):
        return "analyst_coverage"
    if u.get("require_transcript", True) and not int(sec.get("transcript_available", 0)):
        return "no_transcript"
    if float(sec.get("median_spread_bps", 0)) >= u.get("max_median_spread_bps", 5.0):
        return "spread"
    if adv20_dollars is None:
        return "insufficient_price_history"
    if adv20_dollars * 100 <= u["min_adv20_dollars_cents"]:
        return "adv"
    return ""


def build_snapshot(securities: list[dict], adv20: dict[str, float], as_of: dt.date, u: dict) -> dict:
    members, reasons = [], {}
    for s in securities:
        why = exclusion_reason(s, adv20.get(s["symbol"]), as_of, u)
        if why:
            reasons[s["symbol"]] = why
        else:
            members.append(s["symbol"])
    members.sort()
    sid = hashlib.sha256(("|".join([as_of.isoformat(), *members])).encode()).hexdigest()[:16]
    return {"id": sid, "as_of": as_of.isoformat(), "symbols": members, "sectors": {s["symbol"]: s.get("sector", "") for s in securities if s["symbol"] in members}, "exclusion_reasons": reasons}


def adv20_from_bars(bars_csv: Path, as_of: dt.date) -> dict[str, float]:
    """Mean(close*volume) over the 20 sessions on or before as_of, per symbol."""
    rows: dict[str, list[tuple[str, float]]] = {}
    with open(bars_csv, newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            if r["date"] <= as_of.isoformat():
                rows.setdefault(r["symbol"], []).append((r["date"], float(r["close"]) * float(r["volume"])))
    out = {}
    for sym, lst in rows.items():
        lst.sort()
        if len(lst) >= 20:
            out[sym] = statistics.fmean(v for _, v in lst[-20:])
    return out


def cmd_backtest(a: argparse.Namespace) -> None:
    cfg = Config.load(a.config)
    secrets = Secrets.load(cfg)
    key = secrets.require("nasdaq_data_link_api_key")
    raw = Path(a.data_dir) / "raw"
    tickers = bulk_download("TICKERS", key, raw)
    sf1 = bulk_download("SF1", key, raw, **{"dimension": "ARQ"})
    sep = bulk_download("SEP", key, raw)
    events = bulk_download("EVENTS", key, raw)
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    strat = load_strategy(cfg)
    n_sec = write_securities_csv(tickers, sf1, out / "securities.csv", None, None, strat["universe"]["min_analyst_coverage"])
    n_bars = write_bars_csv(sep, out / "bars.csv")
    n_ev = write_events_from_sharadar(events, sf1, out / "earnings.csv")
    log.warning("analyst coverage and transcript availability defaulted to neutral values; join an FMP snapshot before trusting the universe size")
    log.info("wrote %d security rows, %d bars, %d earnings skeleton rows to %s", n_sec, n_bars, n_ev, out)


def cmd_live(a: argparse.Namespace) -> None:
    cfg = Config.load(a.config)
    strat = load_strategy(cfg)
    sec_csv = Path(a.securities)
    bars_csv = Path(a.bars)
    as_of = dt.date.fromisoformat(a.as_of) if a.as_of else dt.date.today()
    latest: dict[str, dict] = {}
    with open(sec_csv, newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            if r["as_of"] <= as_of.isoformat() and (r["symbol"] not in latest or latest[r["symbol"]]["as_of"] < r["as_of"]):
                latest[r["symbol"]] = r
    snap = build_snapshot(list(latest.values()), adv20_from_bars(bars_csv, as_of), as_of, strat["universe"])
    out = cfg.resolve(cfg.get("state_dir", "var/state")) / "universe.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(snap, indent=1), encoding="utf-8")
    log.info("universe %s as of %s: %d members -> %s", snap["id"], snap["as_of"], len(snap["symbols"]), out)


def main() -> None:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("backtest")
    b.add_argument("--config", default=None)
    b.add_argument("--data-dir", default="data/sharadar")
    b.add_argument("--out", default="data/sharadar")
    l = sub.add_parser("live")
    l.add_argument("--config", default=None)
    l.add_argument("--securities", required=True)
    l.add_argument("--bars", required=True)
    l.add_argument("--as-of", default=None)
    a = ap.parse_args()
    logging.basicConfig(level="INFO", format="%(asctime)s %(name)s %(levelname)s %(message)s")
    {"backtest": cmd_backtest, "live": cmd_live}[a.cmd](a)


if __name__ == "__main__":
    main()
