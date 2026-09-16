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
from .sharadar import (SharadarClient, SharadarFreeTier, append_alpaca_bars, fmp_coverage, write_bars_csv,
                       write_earnings_csv, write_securities_csv)

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
    """Pull point-in-time Sharadar data into the CSV layout the C++ backtester reads."""
    cfg = Config.load(a.config)
    secrets = Secrets.load(cfg)
    strat = load_strategy(cfg)
    client = SharadarClient(secrets.require("nasdaq_data_link_api_key"))
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    start = dt.date.fromisoformat(a.start)
    end = dt.date.fromisoformat(a.end) if a.end else dt.date.today()
    # Bars need a warm-up before `start` for the 252-session momentum lookback and the 200-day trend MA.
    bars_start = start - dt.timedelta(days=int(a.warmup_days))

    tickers = client.stock_tickers()
    log.info("sharadar: %d tickers with a price history", len(tickers))
    entitled = [t for t in tickers if (t.get("isdelisted") in ("N", None) or a.include_delisted)]
    symbols = sorted({t["ticker"] for t in entitled})
    if a.symbols:
        want = {s.strip().upper() for s in a.symbols.split(",")}
        symbols = [s for s in symbols if s in want]
    if a.max_symbols:
        symbols = symbols[: int(a.max_symbols)]
    benchmark = strat["universe"].get("benchmark", "SPY")

    # What can this key actually read? A free-tier key covers ~30 names; probing first
    # avoids marching through thousands of 403s and reports the real coverage up front.
    if not a.no_probe:
        readable = client.entitled_symbols(end)
        kept = [s for s in symbols if s in readable]
        if len(kept) < len(symbols):
            log.warning("Sharadar subscription covers %d of %d tickers; building only those", len(kept), len(symbols))
        symbols = kept
    if not symbols:
        log.error("Sharadar subscription covers none of the requested tickers")
        raise SystemExit(3)
    log.info("building %d symbols (+ benchmark %s) from %s to %s", len(symbols), benchmark, bars_start, end)

    coverage = None
    if secrets.has("fmp_api_key") and a.enrich:
        # Two FMP calls per symbol: only worth it once the symbol list is final.
        coverage = fmp_coverage(secrets.get("fmp_api_key"), symbols)
        counts = sorted(c for c, _ in coverage.values())
        log.info("fmp coverage: %d symbols, median analysts=%s", len(coverage), counts[len(counts) // 2] if counts else "n/a")

    benchmark_from_sharadar = benchmark in readable if not a.no_probe else True
    try:
        n_bars = write_bars_csv(client, out / "bars.csv", symbols + ([benchmark] if benchmark_from_sharadar else []), bars_start, end)
        if not benchmark_from_sharadar:
            # Sharadar's cheaper tiers exclude ETFs; without the benchmark there is no EAR and no trend filter.
            log.warning("benchmark %s not in the Sharadar subscription; pulling it from Alpaca instead", benchmark)
            n_bars += append_alpaca_bars(out / "bars.csv", benchmark, secrets.require("alpaca_key_id"),
                                         secrets.require("alpaca_secret_key"), bars_start, end,
                                         cfg.get("alpaca.data_base_url", "https://data.alpaca.markets"),
                                         cfg.get("alpaca.data_feed", "iex"))
        n_sec = write_securities_csv(client, out / "securities.csv", tickers, symbols, bars_start, coverage, None,
                                     strat["universe"]["min_analyst_coverage"], 0.0)
        n_ev = write_earnings_csv(client, out / "earnings.csv", symbols, bars_start)
    except SharadarFreeTier as e:
        log.error("Sharadar subscription does not cover the requested universe: %s", e)
        raise SystemExit(3) from None
    finally:
        client.close()

    if coverage is None:
        log.warning("analyst coverage defaulted to the universe minimum; the coverage rule is NOT being enforced")
    log.warning("quoted spreads are not in Sharadar; the median-spread rule is NOT being enforced (median_spread_bps=0)")
    log.info("wrote %d bars, %d security rows, %d earnings rows to %s", n_bars, n_sec, n_ev, out)
    if len(symbols) < 200:
        log.warning("only %d symbols: too small for a meaningful cross-sectional momentum percentile or Gate G1", len(symbols))


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
    b = sub.add_parser("backtest", help="pull point-in-time Sharadar data into bars/securities/earnings CSVs")
    b.add_argument("--config", default=None)
    b.add_argument("--out", default="data/sharadar")
    b.add_argument("--start", default="2019-01-01", help="first session the backtest will evaluate")
    b.add_argument("--end", default=None)
    b.add_argument("--warmup-days", default=500, help="extra calendar days of bars before --start for the 12-1 and 200-day lookbacks")
    b.add_argument("--symbols", default=None, help="comma-separated subset")
    b.add_argument("--max-symbols", default=None)
    b.add_argument("--include-delisted", action="store_true", help="keep delisted tickers (survivorship-free; recommended)")
    b.add_argument("--enrich", action="store_true", help="join FMP analyst coverage (2 API calls per symbol)")
    b.add_argument("--no-probe", action="store_true", help="skip the entitlement probe and attempt every ticker")
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
