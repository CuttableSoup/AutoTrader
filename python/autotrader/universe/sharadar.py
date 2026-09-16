"""Sharadar (Nasdaq Data Link) loaders producing the CSV layout the C++ side reads.

Tables used (docs/DECISIONS.md #4):
  SHARADAR/TICKERS  category, sector, exchange, firstpricedate, isdelisted
  SHARADAR/SF1      marketcap (dimension ARQ, point-in-time by datekey)
  SHARADAR/SEP      daily prices, split-adjusted (open/high/low/close/volume)
  SHARADAR/EVENTS   8-K event codes (material filings) for the mock validator

Bulk downloads are used (the tables are large); an API key comes from secrets
(nasdaq_data_link_api_key). Analyst coverage and transcript availability are
NOT in Sharadar; they are joined from an FMP snapshot when present, otherwise
coverage defaults to the universe minimum so the rule is neutral (logged).
"""
from __future__ import annotations

import csv
import datetime as dt
import io
import logging
import zipfile
from pathlib import Path

import httpx

log = logging.getLogger("autotrader.universe.sharadar")

BULK_URL = "https://data.nasdaq.com/api/v3/datatables/SHARADAR/{table}.json"


def bulk_download(table: str, api_key: str, out_dir: Path, **filters: str) -> Path:
    """Request a bulk export and download the zip; returns the CSV path. Cached per day."""
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = dt.date.today().isoformat()
    target = out_dir / f"{table}-{stamp}.csv"
    if target.exists():
        return target
    params = {"qopts.export": "true", "api_key": api_key, **filters}
    with httpx.Client(timeout=120) as c:
        for _ in range(60):
            r = c.get(BULK_URL.format(table=table), params=params)
            r.raise_for_status()
            info = r.json()["datatable_bulk_download"]
            status = info["file"]["status"]
            if status == "fresh":
                link = info["file"]["link"]
                break
            log.info("%s bulk export status=%s; waiting", table, status)
            import time
            time.sleep(10)
        else:
            raise RuntimeError(f"{table}: bulk export never became fresh")
        data = c.get(link).content
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        name = z.namelist()[0]
        target.write_bytes(z.read(name))
    return target


def write_bars_csv(sep_csv: Path, out: Path, symbols: set[str] | None = None) -> int:
    """SEP -> bars.csv (symbol,date,open,high,low,close,volume). SEP is already split-adjusted."""
    n = 0
    with open(sep_csv, newline="", encoding="utf-8") as f, open(out, "w", newline="", encoding="utf-8") as g:
        rd = csv.DictReader(f)
        w = csv.writer(g, lineterminator="\n")
        w.writerow(["symbol", "date", "open", "high", "low", "close", "volume"])
        for row in rd:
            if symbols is not None and row["ticker"] not in symbols:
                continue
            w.writerow([row["ticker"], row["date"], row["open"], row["high"], row["low"], row["close"], int(float(row["volume"] or 0))])
            n += 1
    return n


def write_securities_csv(tickers_csv: Path, sf1_csv: Path, out: Path, coverage: dict[str, tuple[int, bool]] | None, spread_bps: dict[str, float] | None, default_coverage: int) -> int:
    """TICKERS + SF1(ARQ marketcap by datekey) -> securities.csv with one row per (symbol, datekey)."""
    meta: dict[str, dict] = {}
    with open(tickers_csv, newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            if row.get("table") not in ("SF1", "SEP", None, ""):
                continue
            meta[row["ticker"]] = row
    n = 0
    with open(sf1_csv, newline="", encoding="utf-8") as f, open(out, "w", newline="", encoding="utf-8") as g:
        w = csv.writer(g, lineterminator="\n")
        w.writerow(["symbol", "name", "sector", "category", "market_cap", "first_listed", "analyst_coverage", "transcript_available", "median_spread_bps", "as_of"])
        for row in csv.DictReader(f):
            if row.get("dimension") != "ARQ":
                continue
            t = row["ticker"]
            m = meta.get(t)
            if not m or not row.get("marketcap"):
                continue
            cov, transcript = (coverage or {}).get(t, (default_coverage, True))
            spr = (spread_bps or {}).get(t, 2.0)
            w.writerow([t, (m.get("name") or "").replace(",", " "), m.get("sector", ""), m.get("category", ""), f"{float(row['marketcap']):.2f}",
                        m.get("firstpricedate", ""), cov, 1 if transcript else 0, f"{spr:.2f}", row["datekey"]])
            n += 1
    return n


def write_events_from_sharadar(events_csv: Path, sf1_csv: Path, out: Path) -> int:
    """EVENTS (8-K codes) + SF1 report dates -> earnings.csv skeleton. EPS actual/consensus
    come from the FMP/Finnhub feed (Sharadar has no consensus); this writes report dates,
    fiscal periods, next_report_date, and material 8-K dates so the mock validator has them."""
    k8: dict[str, list[str]] = {}
    with open(events_csv, newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            codes = row.get("eventcodes", "")
            # Material codes: 1.01 entry into agreement, 2.01 completion of acquisition, 2.04 triggering events,
            # 2.06 material impairments, 4.01 auditor change, 4.02 non-reliance, 5.02 officer departure, 8.01 other.
            if any(c in codes.split("|") for c in ("101", "201", "204", "206", "401", "402", "502")):
                k8.setdefault(row["ticker"], []).append(row["date"])
    reports: dict[str, list[tuple[str, str]]] = {}
    with open(sf1_csv, newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            if row.get("dimension") != "ARQ" or not row.get("datekey"):
                continue
            reports.setdefault(row["ticker"], []).append((row["datekey"], row.get("calendardate", "")))
    n = 0
    with open(out, "w", newline="", encoding="utf-8") as g:
        w = csv.writer(g, lineterminator="\n")
        w.writerow(["event_id", "symbol", "report_date", "timing", "fiscal_period", "eps_actual", "eps_consensus", "eps_consensus_asof", "revenue_actual", "revenue_consensus", "next_report_date", "material_8k_dates"])
        for t, rows in reports.items():
            rows.sort()
            for i, (datekey, caldate) in enumerate(rows):
                nxt = rows[i + 1][0] if i + 1 < len(rows) else ""
                cd = dt.date.fromisoformat(caldate) if caldate else None
                fp = f"{cd.year}Q{(cd.month - 1) // 3 + 1}" if cd else ""
                ks = [d for d in k8.get(t, []) if datekey < d <= (nxt or "9999")]
                w.writerow(["", t, datekey, "UNKNOWN", fp, "", "", "", "", "", nxt, ";".join(ks)])
                n += 1
    return n
