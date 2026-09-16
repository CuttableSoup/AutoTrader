"""Fama-French 3 factors from the Ken French data library (data/factors/, see SOURCE.md)."""
from __future__ import annotations

import csv
from pathlib import Path

import numpy as np
import pandas as pd

from autotrader.schemas import find_project_root


def default_path() -> Path:
    return find_project_root(Path(__file__).parent) / "data" / "factors" / "ff3_daily.csv"


def load_ff3_daily(path: Path | None = None) -> pd.DataFrame:
    """Daily factors as DECIMALS, indexed by date: mkt_rf, smb, hml, rf.

    ff3_daily.csv is written by scripts/fetch_ff_factors.py in that normalised form.
    """
    df = pd.read_csv(path or default_path(), parse_dates=["date"]).set_index("date").sort_index()
    return df[["mkt_rf", "smb", "hml", "rf"]].astype(float)


def parse_french_csv(text: str) -> pd.DataFrame:
    """Parse the library's CSV (percent units, YYYYMMDD dates, prose header and footer)."""
    rows = []
    for rec in csv.reader(text.splitlines()):
        if len(rec) < 5:
            continue
        d = rec[0].strip()
        if len(d) != 8 or not d.isdigit():
            continue
        try:
            vals = [float(v) / 100.0 for v in rec[1:5]]
        except ValueError:
            continue
        rows.append((pd.Timestamp(d), *vals))
    if not rows:
        raise ValueError("no daily factor rows found; has the library format changed?")
    return pd.DataFrame(rows, columns=["date", "mkt_rf", "smb", "hml", "rf"]).set_index("date")


def monthly_from_daily(daily: pd.DataFrame) -> pd.DataFrame:
    """Compound daily factor returns to calendar months (index = month-end timestamp)."""
    return (1.0 + daily).groupby(daily.index.to_period("M")).prod().sub(1.0).to_timestamp("M")


def compound_monthly(daily_returns: np.ndarray, month_ids: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """(T, N) daily simple returns -> (M, N) monthly compounded returns.

    A month with fewer than 15 valid sessions for a stock is NaN for that stock, so a
    listing or delisting mid-month does not create a fake partial-month return.
    """
    months = np.unique(month_ids)
    out = np.full((len(months), daily_returns.shape[1]), np.nan)
    for i, m in enumerate(months):
        block = daily_returns[month_ids == m]
        valid = np.isfinite(block).sum(axis=0)
        comp = np.nanprod(1.0 + block, axis=0) - 1.0
        comp[valid < 15] = np.nan
        out[i] = comp
    return months, out
