"""Vendor clients for the earnings calendar. Every function returns a list of
VendorEvent with a uniform shape so the feed can cross-check them.

Endpoints and fields drift (docs/DESIGN.md section 13): re-verify at signup.
"""
from __future__ import annotations

import datetime as dt
import logging
from dataclasses import dataclass

import httpx

log = logging.getLogger("autotrader.events.vendors")
# httpx logs full request URLs at INFO, which would put vendor API keys into the log archive.
logging.getLogger("httpx").setLevel(logging.WARNING)


@dataclass
class VendorEvent:
    vendor: str
    symbol: str
    report_date: dt.date
    timing: str                      # BMO | AMC | UNKNOWN
    fiscal_period: str               # "2026Q3" when derivable, else ""
    eps_actual: float | None
    eps_estimate: float | None
    revenue_actual: float | None     # dollars
    revenue_estimate: float | None
    raw: dict


def _f(v) -> float | None:
    try:
        return None if v in (None, "", "null") else float(v)
    except (TypeError, ValueError):
        return None


def _timing(s: str | None) -> str:
    s = (s or "").lower()
    if s in ("bmo", "before market open", "pre-market", "premarket"):
        return "BMO"
    if s in ("amc", "after market close", "post-market", "aftermarket"):
        return "AMC"
    if s in ("dmh", "during market hours"):
        return "BMO"   # reaction happens on the report date itself, which is what BMO means for day 0
    return "UNKNOWN"


def _fiscal_from_date(fiscal_end: str | None) -> str:
    try:
        d = dt.date.fromisoformat(fiscal_end[:10])
        return f"{d.year}Q{(d.month - 1) // 3 + 1}"
    except Exception:  # noqa: BLE001
        return ""


class FmpClient:
    """Financial Modeling Prep. Premium: earnings calendar with BMO/AMC, actual vs consensus."""

    def __init__(self, api_key: str, base_url: str = "https://financialmodelingprep.com", timeout: float = 20.0):
        self.api_key = api_key
        self.base_url = base_url.rstrip("/")
        self.http = httpx.Client(timeout=timeout)

    def earnings_calendar(self, start: dt.date, end: dt.date) -> list[VendorEvent]:
        r = self.http.get(f"{self.base_url}/stable/earnings-calendar", params={"from": start.isoformat(), "to": end.isoformat(), "apikey": self.api_key})
        r.raise_for_status()
        out = []
        for row in r.json():
            try:
                out.append(VendorEvent(
                    vendor="fmp", symbol=row["symbol"], report_date=dt.date.fromisoformat(row["date"][:10]),
                    timing=_timing(row.get("time")), fiscal_period=_fiscal_from_date(row.get("fiscalDateEnding")),
                    eps_actual=_f(row.get("eps") if row.get("eps") is not None else row.get("epsActual")),
                    eps_estimate=_f(row.get("epsEstimated")), revenue_actual=_f(row.get("revenue") if row.get("revenue") is not None else row.get("revenueActual")),
                    revenue_estimate=_f(row.get("revenueEstimated")), raw=row))
            except (KeyError, ValueError) as e:
                log.debug("fmp row skipped: %s (%s)", row, e)
        return out

    def transcript(self, symbol: str, year: int, quarter: int) -> str:
        r = self.http.get(f"{self.base_url}/stable/earning-call-transcript", params={"symbol": symbol, "year": year, "quarter": quarter, "apikey": self.api_key})
        r.raise_for_status()
        data = r.json()
        return data[0].get("content", "") if data else ""


class FinnhubClient:
    """Finnhub free tier: cross-check for dates, timing (hour), and FDA/insider events."""

    def __init__(self, api_key: str, base_url: str = "https://finnhub.io/api/v1", timeout: float = 20.0):
        self.api_key = api_key
        self.base_url = base_url.rstrip("/")
        self.http = httpx.Client(timeout=timeout, headers={"X-Finnhub-Token": api_key})

    def earnings_calendar(self, start: dt.date, end: dt.date, symbol: str | None = None) -> list[VendorEvent]:
        params = {"from": start.isoformat(), "to": end.isoformat()}
        if symbol:
            params["symbol"] = symbol
        r = self.http.get(f"{self.base_url}/calendar/earnings", params=params)
        r.raise_for_status()
        out = []
        for row in r.json().get("earningsCalendar", []):
            try:
                fp = f"{row['year']}Q{row['quarter']}" if row.get("year") and row.get("quarter") else ""
                out.append(VendorEvent(
                    vendor="finnhub", symbol=row["symbol"], report_date=dt.date.fromisoformat(row["date"][:10]), timing=_timing(row.get("hour")),
                    fiscal_period=fp, eps_actual=_f(row.get("epsActual")), eps_estimate=_f(row.get("epsEstimate")),
                    revenue_actual=_f(row.get("revenueActual")), revenue_estimate=_f(row.get("revenueEstimate")), raw=row))
            except (KeyError, ValueError) as e:
                log.debug("finnhub row skipped: %s (%s)", row, e)
        return out

    def fda_calendar(self) -> list[dict]:
        r = self.http.get(f"{self.base_url}/fda-advisory-committee-calendar")
        r.raise_for_status()
        return r.json()
