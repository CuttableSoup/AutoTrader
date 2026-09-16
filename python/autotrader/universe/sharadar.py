"""Sharadar loaders producing the CSV layout the C++ backtester reads.

API (verified 2026-09-15): https://api.sharadar.com/v1.0/data/{table}
  auth      x-api-key header (never the query string: request URLs reach logs)
  response  {"count": N, "data": [ {...}, ... ]}   rows are objects
  paging    limit (max 10000) + offset
  tables    tickers | stocks (SEP) | fundamentals (SF1) | events | daily | actions

Tables used (docs/DECISIONS.md #4):
  tickers       name, exchange, category, sector, firstpricedate, isdelisted, table
  stocks        daily OHLCV, split-adjusted (closeadj additionally dividend-adjusted)
  fundamentals  dimension=ARQ; `date` is the point-in-time filing date (the old
                SF1 `datekey`), `fiscalperiod` e.g. "2026-Q3", marketcap in RAW
                DOLLARS, eps/revenue as reported
  events        8-K item codes as 2 digits: 11 = item 1.01, 42 = item 4.02, ...

Not in Sharadar: analyst coverage, transcript availability, quoted spreads.
Those universe rules are filled from FMP when a key is supplied, otherwise they
are set to neutral values and loudly logged, so the rule does not silently pass.
"""
from __future__ import annotations

import csv
import datetime as dt
import logging
import shutil
import time
import zipfile
from pathlib import Path
from typing import Any, Iterator

import httpx

log = logging.getLogger("autotrader.universe.sharadar")

BASE_URL = "https://api.sharadar.com/v1.0"
PAGE_LIMIT = 10000

# 8-K items that a validator would consider material (docs/DESIGN.md 7.1 "second material 8-K").
# Deliberately excluded as boilerplate or as the earnings release itself:
#   22 (2.02 results of operations), 71 (7.01 Reg FD), 81 (8.01 other), 91 (9.01 exhibits), 57 (5.07 annual meeting).
MATERIAL_8K_CODES = {
    "11",  # 1.01 entry into a material definitive agreement
    "12",  # 1.02 termination of a material definitive agreement
    "13",  # 1.03 bankruptcy or receivership
    "21",  # 2.01 completion of acquisition or disposition
    "23",  # 2.03 creation of a direct financial obligation
    "24",  # 2.04 triggering events accelerating an obligation
    "25",  # 2.05 costs associated with exit or disposal
    "26",  # 2.06 material impairments
    "31",  # 3.01 notice of delisting / failure to satisfy a listing rule
    "41",  # 4.01 changes in registrant's certifying accountant
    "42",  # 4.02 non-reliance on previously issued financials (restatement)
    "52",  # 5.02 departure or election of directors or principal officers
}


class SharadarFreeTier(RuntimeError):
    """The API key is limited to the free sample universe (the ~30 Dow names)."""


class SharadarClient:
    def __init__(self, api_key: str, base_url: str = BASE_URL, timeout: float = 90.0, pause_s: float = 0.25,
                 max_retries: int = 6, retry_base_s: float = 20.0):
        self.base_url = base_url.rstrip("/")
        self.pause_s = pause_s            # between pages, to stay under the request-count quota
        self.max_retries = max_retries
        self.retry_base_s = retry_base_s
        self.http = httpx.Client(timeout=timeout, headers={"x-api-key": api_key, "Accept": "application/json"})

    def close(self) -> None:
        self.http.close()

    def page(self, table: str, /, **params: Any) -> dict:
        # `table` is positional-only: several endpoints take a `table` FILTER (tickers?table=stocks).
        p = {k: v for k, v in params.items() if v is not None}
        p["format"] = "json"
        delay = self.retry_base_s
        for attempt in range(1, self.max_retries + 1):
            r = self.http.get(f"{self.base_url}/data/{table}", params=p)
            if r.status_code == 403 and "free tier" in r.text.lower():
                raise SharadarFreeTier(f"{table} {p.get('ticker', '')}: {r.text[:160]}")
            if r.status_code == 429:
                # Sharadar throttles on request count; back off rather than hammering it.
                if attempt == self.max_retries:
                    raise RuntimeError(f"sharadar rate limited on {table} after {attempt} attempts: {r.text[:160]}")
                wait = float(r.headers.get("retry-after") or delay)
                log.warning("sharadar 429 on %s; sleeping %.0fs (attempt %d/%d)", table, wait, attempt, self.max_retries)
                time.sleep(wait)
                delay = min(delay * 2, 120.0)
                continue
            r.raise_for_status()
            return r.json()
        raise RuntimeError(f"sharadar: unreachable retry loop for {table}")

    def rows(self, table: str, /, **params: Any) -> Iterator[dict]:
        """Paginate a table. Stops when a short page comes back."""
        offset = 0
        while True:
            j = self.page(table, **params, limit=PAGE_LIMIT, offset=offset)
            data = j.get("data", [])
            for row in data:
                yield row
            if len(data) < PAGE_LIMIT:
                return
            offset += len(data)
            if self.pause_s:
                time.sleep(self.pause_s)

    def rows_safe(self, table: str, /, **params: Any) -> Iterator[dict]:
        """Like rows(), but a free-tier 403 for one ticker skips that ticker instead of aborting."""
        try:
            yield from self.rows(table, **params)
        except SharadarFreeTier as e:
            log.warning("free tier: skipping %s", e)

    def bulk(self, table: str, years: str, dest_dir: Path) -> Path | None:
        """Download a whole table as CSV via the bulk export (302 -> presigned zip).

        Sharadar's own throttle message says to use this for large extracts, and it
        is the difference between minutes and hours: ten years of `stocks` is ~12M
        rows, which is ~1200 paginated requests. Returns None when the subscription
        does not include bulk for that table, so the caller can fall back to paging.
        Cached per (table, years, day).
        """
        dest_dir.mkdir(parents=True, exist_ok=True)
        target = dest_dir / f"{table}-{years}-{dt.date.today().isoformat()}.csv"
        if target.exists() and target.stat().st_size > 0:
            log.info("bulk %s: cached %s (%.0f MB)", table, target.name, target.stat().st_size / 1e6)
            return target
        r = self.http.get(f"{self.base_url}/data/{table}", params={"years": years, "format": "csv"},
                          follow_redirects=False, timeout=120)
        if r.status_code != 302:
            log.warning("bulk %s unavailable (HTTP %s): %s", table, r.status_code, r.text[:160])
            return None
        url = r.headers["location"]
        log.info("bulk %s: downloading %s years...", table, years)
        tmp = target.with_suffix(".zip")
        with httpx.stream("GET", url, timeout=1800, follow_redirects=True) as resp:
            resp.raise_for_status()
            with open(tmp, "wb") as f:
                for chunk in resp.iter_bytes(1 << 20):
                    f.write(chunk)
        with zipfile.ZipFile(tmp) as z:
            name = z.namelist()[0]
            with z.open(name) as src, open(target, "wb") as dst:
                shutil.copyfileobj(src, dst)
        tmp.unlink()
        log.info("bulk %s: %s (%.0f MB)", table, target.name, target.stat().st_size / 1e6)
        return target

    def stock_tickers(self) -> list[dict]:
        """Ticker metadata for securities that have a price history (table == 'stocks')."""
        return list(self.rows("tickers", table="stocks"))

    def entitled_symbols(self, on_or_before: dt.date, lookback_days: int = 10) -> set[str]:
        """Every symbol this key may read, in one query.

        An unfiltered `stocks` query returns rows only for entitled tickers, so the
        symbols present over a recent window ARE the entitlement. A free-tier key
        yields ~30 Dow names; a full subscription yields the whole tape. This costs
        one paginated request instead of one probe per ticker.
        """
        start = (on_or_before - dt.timedelta(days=lookback_days)).isoformat()
        syms = {r["ticker"] for r in self.rows("stocks", **{"from": start, "to": on_or_before.isoformat()})}
        log.info("sharadar entitlement: %d symbols readable in %s..%s", len(syms), start, on_or_before)
        return syms


def _f(v: Any) -> float | None:
    try:
        return None if v in (None, "", "None") else float(v)
    except (TypeError, ValueError):
        return None


def _d(v: Any) -> str:
    return str(v)[:10] if v else ""


# --------------------------------------------------------------------- bars --

def write_bars_csv(client: SharadarClient, out: Path, symbols: list[str], start: dt.date, end: dt.date) -> int:
    """stocks -> bars.csv (symbol,date,open,high,low,close,volume). OHLC are split-adjusted."""
    out.parent.mkdir(parents=True, exist_ok=True)
    n = 0
    with open(out, "w", newline="", encoding="utf-8") as g:
        w = csv.writer(g, lineterminator="\n")
        w.writerow(["symbol", "date", "open", "high", "low", "close", "volume"])
        for i, sym in enumerate(symbols, 1):
            rows = sorted(client.rows_safe("stocks", ticker=sym, **{"from": start.isoformat(), "to": end.isoformat()}),
                          key=lambda r: r["date"])
            for r in rows:
                o, h, l, c = _f(r.get("open")), _f(r.get("high")), _f(r.get("low")), _f(r.get("close"))
                v = _f(r.get("volume"))
                if None in (o, h, l, c) or v is None or c <= 0:
                    continue
                w.writerow([sym, _d(r["date"]), f"{o:.4f}", f"{h:.4f}", f"{l:.4f}", f"{c:.4f}", int(v)])
                n += 1
            if i % 25 == 0:
                log.info("bars: %d/%d symbols, %d rows", i, len(symbols), n)
            if client.pause_s:
                time.sleep(client.pause_s)
    return n


# --------------------------------------------------------------- securities --

def write_securities_csv(client: SharadarClient, out: Path, tickers: list[dict], symbols: list[str], start: dt.date,
                         coverage: dict[str, tuple[int, bool]] | None, spread_bps: dict[str, float] | None,
                         default_coverage: int, default_spread_bps: float) -> int:
    """tickers + fundamentals(ARQ) -> securities.csv, one row per point-in-time filing date.

    market_cap comes from fundamentals.marketcap (raw dollars) keyed by the filing
    date, so the C++ universe builder sees it only from the day it was public.
    """
    meta = {t["ticker"]: t for t in tickers}
    out.parent.mkdir(parents=True, exist_ok=True)
    n = 0
    missing_cov = set()
    with open(out, "w", newline="", encoding="utf-8") as g:
        w = csv.writer(g, lineterminator="\n")
        w.writerow(["symbol", "name", "sector", "category", "market_cap", "first_listed", "analyst_coverage",
                    "transcript_available", "median_spread_bps", "as_of"])
        for sym in symbols:
            m = meta.get(sym)
            if not m:
                continue
            cov, transcript = (coverage or {}).get(sym, (default_coverage, True))
            if coverage is not None and sym not in coverage:
                missing_cov.add(sym)
            spr = (spread_bps or {}).get(sym, default_spread_bps)
            wrote_any = False
            for r in client.rows_safe("fundamentals", ticker=sym, dimension="ARQ", **{"from": start.isoformat()}):
                cap = _f(r.get("marketcap"))
                asof = _d(r.get("date"))
                if cap is None or cap <= 0 or not asof:
                    continue
                w.writerow([sym, (m.get("name") or "").replace(",", " "), m.get("sector") or "", m.get("category") or "",
                            f"{cap:.2f}", _d(m.get("firstpricedate")), cov, 1 if transcript else 0, f"{spr:.2f}", asof])
                n += 1
                wrote_any = True
            if not wrote_any:
                log.debug("securities: no ARQ fundamentals for %s", sym)
    if missing_cov:
        log.warning("analyst coverage unknown for %d symbols; defaulted to %d", len(missing_cov), default_coverage)
    return n


# ----------------------------------------------------------------- earnings --

def write_earnings_csv(client: SharadarClient, out: Path, symbols: list[str], start: dt.date,
                       consensus: dict[tuple[str, str], dict] | None = None) -> int:
    """fundamentals(ARQ) + events -> earnings.csv.

    report_date = fundamentals.date (the filing date, the first day the market can react).
    timing stays UNKNOWN: Sharadar has no BMO/AMC field, so the C++ side treats day 0
    as the session after the report date, which is the conservative reading.
    eps/revenue are as reported. Consensus is optional and comes from a vendor that
    snapshots estimates before the announcement (FMP); without it the v1.0 signal is
    unaffected (it is price/volume/momentum based) and only the mock validator's
    revenue-surprise proxy goes quiet.
    """
    out.parent.mkdir(parents=True, exist_ok=True)
    n = 0
    with open(out, "w", newline="", encoding="utf-8") as g:
        w = csv.writer(g, lineterminator="\n")
        w.writerow(["event_id", "symbol", "report_date", "timing", "fiscal_period", "eps_actual", "eps_consensus",
                    "eps_consensus_asof", "revenue_actual", "revenue_consensus", "next_report_date", "material_8k_dates"])
        for sym in symbols:
            k8 = sorted(_d(r["date"]) for r in client.rows_safe("events", ticker=sym, **{"from": start.isoformat()})
                        if MATERIAL_8K_CODES & set(str(r.get("eventcodes") or "").split("|")))
            reports = sorted(client.rows_safe("fundamentals", ticker=sym, dimension="ARQ", **{"from": start.isoformat()}),
                             key=lambda r: _d(r.get("date")))
            for i, r in enumerate(reports):
                rd = _d(r.get("date"))
                if not rd:
                    continue
                nxt = _d(reports[i + 1].get("date")) if i + 1 < len(reports) else ""
                fp = (r.get("fiscalperiod") or "").replace("-", "")     # "2026-Q3" -> "2026Q3"
                eps = _f(r.get("eps"))
                rev = _f(r.get("revenue"))
                cons = (consensus or {}).get((sym, rd), {})
                window = [d for d in k8 if rd < d <= (nxt or "9999-12-31")]
                w.writerow(["", sym, rd, "UNKNOWN", fp,
                            "" if eps is None else f"{eps:.4f}",
                            cons.get("eps_consensus", ""), cons.get("eps_consensus_asof", ""),
                            "" if rev is None else f"{rev:.2f}", cons.get("revenue_consensus", ""),
                            nxt, ";".join(window)])
                n += 1
    return n


# ------------------------------------------------------- bulk transform -----

def _iter_csv(path: Path) -> Iterator[dict]:
    with open(path, newline="", encoding="utf-8", errors="ignore") as f:
        yield from csv.DictReader(f)


def build_from_bulk(bulk: dict[str, Path], out: Path, start: dt.date, end: dt.date, min_market_cap: float,
                    benchmark: str, coverage: dict[str, tuple[int, bool]] | None, default_coverage: int,
                    default_spread_bps: float = 0.0) -> dict[str, int]:
    """Turn the four bulk CSVs into bars.csv / securities.csv / earnings.csv.

    Everything streams: `stocks` alone is ~16M rows and must never be held in memory.

    The symbol set is chosen from `fundamentals` first: a name that never reached
    `min_market_cap` inside the window can never satisfy the universe rule, so its
    bars are not worth carrying. That is a superset of the real universe (the ADV,
    spread and coverage rules narrow it further at evaluation time), so it cannot
    remove a name the strategy would otherwise have traded. It cuts the bar file
    by roughly an order of magnitude.

    Delisted names are kept. Dropping them is exactly the survivorship bias that
    makes a backtest lie (docs/DESIGN.md 7.1).
    """
    out.mkdir(parents=True, exist_ok=True)
    lo, hi = start.isoformat(), end.isoformat()
    tickers = {r["ticker"]: r for r in _iter_csv(bulk["tickers"]) if r.get("table") in ("SEP", "stocks")}
    log.info("bulk: %d tickers with a price history", len(tickers))

    # ---- pass 1: fundamentals -> eligible symbols + securities rows + earnings rows
    eligible: set[str] = set()
    fund_rows: dict[str, list[dict]] = {}
    scanned = 0
    for r in _iter_csv(bulk["fundamentals"]):
        scanned += 1
        if r.get("dimension") != "ARQ":
            continue
        sym, d = r.get("ticker", ""), _d(r.get("date"))
        if not d or not (lo <= d <= hi) or sym not in tickers:
            continue
        cap = _f(r.get("marketcap"))
        if cap is not None and cap > min_market_cap:
            eligible.add(sym)
        fund_rows.setdefault(sym, []).append({"date": d, "marketcap": cap, "eps": _f(r.get("eps")),
                                              "revenue": _f(r.get("revenue")), "fiscalperiod": (r.get("fiscalperiod") or "").replace("-", "")})
    log.info("bulk: scanned %d fundamentals rows; %d symbols ever above the market-cap floor", scanned, len(eligible))

    # ---- pass 2: events -> material 8-K dates for eligible symbols
    k8: dict[str, list[str]] = {}
    for r in _iter_csv(bulk["events"]):
        sym = r.get("ticker", "")
        if sym not in eligible:
            continue
        if MATERIAL_8K_CODES & set(str(r.get("eventcodes") or "").split("|")):
            d = _d(r.get("date"))
            if lo <= d <= hi:
                k8.setdefault(sym, []).append(d)
    for v in k8.values():
        v.sort()
    log.info("bulk: material 8-K dates for %d symbols", len(k8))

    # ---- securities.csv + earnings.csv
    n_sec = n_ev = 0
    missing_cov: set[str] = set()
    with open(out / "securities.csv", "w", newline="", encoding="utf-8") as gs, \
         open(out / "earnings.csv", "w", newline="", encoding="utf-8") as ge:
        ws, we = csv.writer(gs, lineterminator="\n"), csv.writer(ge, lineterminator="\n")
        ws.writerow(["symbol", "name", "sector", "category", "market_cap", "first_listed", "analyst_coverage",
                     "transcript_available", "median_spread_bps", "as_of"])
        we.writerow(["event_id", "symbol", "report_date", "timing", "fiscal_period", "eps_actual", "eps_consensus",
                     "eps_consensus_asof", "revenue_actual", "revenue_consensus", "next_report_date", "material_8k_dates"])
        for sym in sorted(eligible):
            m = tickers[sym]
            cov, transcript = (coverage or {}).get(sym, (default_coverage, True))
            if coverage is not None and sym not in coverage:
                missing_cov.add(sym)
            reports = sorted(fund_rows.get(sym, []), key=lambda x: x["date"])
            for i, r in enumerate(reports):
                if r["marketcap"] is not None and r["marketcap"] > 0:
                    ws.writerow([sym, (m.get("name") or "").replace(",", " "), m.get("sector") or "", m.get("category") or "",
                                 f"{r['marketcap']:.2f}", _d(m.get("firstpricedate")), cov, 1 if transcript else 0,
                                 f"{default_spread_bps:.2f}", r["date"]])
                    n_sec += 1
                nxt = reports[i + 1]["date"] if i + 1 < len(reports) else ""
                window = [d for d in k8.get(sym, []) if r["date"] < d <= (nxt or "9999-12-31")]
                we.writerow(["", sym, r["date"], "UNKNOWN", r["fiscalperiod"],
                             "" if r["eps"] is None else f"{r['eps']:.4f}", "", "",
                             "" if r["revenue"] is None else f"{r['revenue']:.2f}", "", nxt, ";".join(window)])
                n_ev += 1
    if missing_cov:
        log.warning("analyst coverage unknown for %d symbols; defaulted to %d", len(missing_cov), default_coverage)

    # ---- bars.csv: stream, keeping only eligible symbols (+ benchmark if Sharadar carries it)
    keep = eligible | {benchmark}
    n_bars = 0
    with open(out / "bars.csv", "w", newline="", encoding="utf-8") as gb:
        wb = csv.writer(gb, lineterminator="\n")
        wb.writerow(["symbol", "date", "open", "high", "low", "close", "volume"])
        for i, r in enumerate(_iter_csv(bulk["stocks"]), 1):
            sym = r.get("ticker", "")
            if sym not in keep:
                continue
            d = _d(r.get("date"))
            if not (lo <= d <= hi):
                continue
            o, h, l, c, v = (_f(r.get("open")), _f(r.get("high")), _f(r.get("low")), _f(r.get("close")), _f(r.get("volume")))
            if None in (o, h, l, c) or v is None or c <= 0:
                continue
            wb.writerow([sym, d, f"{o:.4f}", f"{h:.4f}", f"{l:.4f}", f"{c:.4f}", int(v)])
            n_bars += 1
            if i % 4_000_000 == 0:
                log.info("bulk bars: %dM rows scanned, %d kept", i // 1_000_000, n_bars)
    return {"bars": n_bars, "securities": n_sec, "earnings": n_ev, "symbols": len(eligible)}


def append_benchmark_from_funds(client: "SharadarClient", out: Path, symbol: str, start: dt.date, end: dt.date) -> int:
    """Append benchmark bars from Sharadar `funds`.

    ETFs are not in `stocks`; Sharadar keeps them in `funds` (legacy SFP). Taking the
    benchmark from there rather than from Alpaca matters for history depth: Alpaca's
    free IEX feed starts in 2020, which would strand four of the ten purchased years,
    because the trend filter cannot run until 200 sessions after the first benchmark bar.
    """
    rows = sorted(client.rows_safe("funds", ticker=symbol, **{"from": start.isoformat(), "to": end.isoformat()}),
                  key=lambda r: r["date"])
    n = 0
    with open(out, "a", newline="", encoding="utf-8") as g:
        w = csv.writer(g, lineterminator="\n")
        for r in rows:
            o, h, l, c, v = (_f(r.get("open")), _f(r.get("high")), _f(r.get("low")), _f(r.get("close")), _f(r.get("volume")))
            if None in (o, h, l, c) or v is None or c <= 0:
                continue
            w.writerow([symbol, _d(r["date"]), f"{o:.4f}", f"{h:.4f}", f"{l:.4f}", f"{c:.4f}", int(v)])
            n += 1
    log.info("benchmark %s from sharadar funds: %d bars", symbol, n)
    return n


# ------------------------------------------------- benchmark from Alpaca ----

def append_alpaca_bars(out: Path, symbol: str, key_id: str, secret_key: str, start: dt.date, end: dt.date,
                       data_base_url: str = "https://data.alpaca.markets", feed: str = "iex", timeout: float = 60.0) -> int:
    """Append split-adjusted daily bars for one symbol to an existing bars.csv.

    The benchmark (SPY) drives both the abnormal-return calculation and the trend
    filter, so a dataset without it yields no candidates at all. Sharadar's cheaper
    tiers exclude ETFs, so the benchmark is taken from Alpaca instead. Alpaca's free
    IEX feed starts around 2020-07; a backtest cannot begin until 200 sessions after
    the first benchmark bar, or the trend filter has nothing to read.
    """
    headers = {"APCA-API-KEY-ID": key_id, "APCA-API-SECRET-KEY": secret_key}
    rows: list[dict] = []
    token: str | None = None
    with httpx.Client(timeout=timeout, headers=headers) as c:
        while True:
            params = {"symbols": symbol, "timeframe": "1Day", "adjustment": "split", "feed": feed, "limit": 10000,
                      "start": start.isoformat(), "end": end.isoformat()}
            if token:
                params["page_token"] = token
            r = c.get(f"{data_base_url.rstrip('/')}/v2/stocks/bars", params=params)
            r.raise_for_status()
            j = r.json()
            rows.extend(j.get("bars", {}).get(symbol, []))
            token = j.get("next_page_token")
            if not token:
                break
    with open(out, "a", newline="", encoding="utf-8") as g:
        w = csv.writer(g, lineterminator="\n")
        for b in sorted(rows, key=lambda x: x["t"]):
            w.writerow([symbol, b["t"][:10], f"{b['o']:.4f}", f"{b['h']:.4f}", f"{b['l']:.4f}", f"{b['c']:.4f}", int(b["v"])])
    if rows:
        log.info("benchmark %s from alpaca: %d bars, %s..%s", symbol, len(rows), rows[0]["t"][:10], rows[-1]["t"][:10])
    return len(rows)


# ---------------------------------------------------------- FMP enrichment ---

def fmp_coverage(api_key: str, symbols: list[str], timeout: float = 30.0, max_retries: int = 4,
                 retry_base_s: float = 15.0) -> dict[str, tuple[int, bool]]:
    """symbol -> (analyst_coverage, transcript_available) from FMP.

    Coverage is max(numAnalystsEps, numAnalystsRevenue) on the latest annual
    estimate. It is a CURRENT value applied to every historical row: a small
    look-ahead on a slow-moving, non-timing rule (docs/DECISIONS.md).

    A symbol whose coverage could not be established is OMITTED from the result
    rather than reported as zero. This matters: FMP answers 429 once its daily
    quota is gone, and treating that as "no analysts cover this name" would
    silently empty the universe instead of falling back to the neutral default.

    Transcript availability needs an FMP tier that exposes the transcript
    endpoints; on plans without it the flag is True (rule disabled) and the
    caller reports that the universe rule is not being enforced.
    """
    out: dict[str, tuple[int, bool]] = {}
    transcript_endpoint_ok = True
    quota_exhausted = False
    with httpx.Client(timeout=timeout) as c:
        for sym in symbols:
            if quota_exhausted:
                break
            cov: int | None = None
            delay = retry_base_s
            for attempt in range(1, max_retries + 1):
                try:
                    r = c.get("https://financialmodelingprep.com/stable/analyst-estimates",
                              params={"symbol": sym, "period": "annual", "limit": 1, "apikey": api_key})
                except Exception as e:  # noqa: BLE001
                    log.debug("fmp coverage %s transport error: %s", sym, e)
                    break
                if r.status_code == 429:
                    if attempt == max_retries:
                        quota_exhausted = True
                        log.warning("fmp rate limit persists after %d attempts; analyst coverage left UNKNOWN for the "
                                    "remaining symbols (the rule falls back to the configured default, not to zero)", attempt)
                        break
                    log.warning("fmp 429 on %s; sleeping %.0fs (attempt %d/%d)", sym, delay, attempt, max_retries)
                    time.sleep(delay)
                    delay = min(delay * 2, 120.0)
                    continue
                if r.status_code == 402:
                    quota_exhausted = True
                    log.warning("fmp analyst-estimates not available on this plan; coverage rule not enforced")
                    break
                if r.status_code == 200:
                    rows = r.json()
                    if rows:
                        row = rows[0]
                        cov = max(int(row.get("numAnalystsEps") or 0), int(row.get("numAnalystsRevenue") or 0))
                break
            if cov is None:
                continue  # unknown, not zero
            transcript = True
            if transcript_endpoint_ok:
                try:
                    r = c.get("https://financialmodelingprep.com/stable/earning-call-transcript-latest",
                              params={"symbol": sym, "limit": 1, "apikey": api_key})
                    if r.status_code in (402, 403):
                        transcript_endpoint_ok = False
                        log.warning("fmp transcripts not available on this plan; transcript rule not enforced")
                    elif r.status_code == 200:
                        transcript = bool(r.json())
                except Exception as e:  # noqa: BLE001
                    log.debug("fmp transcript %s failed: %s", sym, e)
            out[sym] = (cov, transcript)
    return out
