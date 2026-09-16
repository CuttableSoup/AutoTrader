"""New signal primitives for the brute-force grid (docs/prereg/BRUTEFORCE-v1.md).

Existing primitives are imported, not copied, from where the H1-H4 pre-tests already
built and tested them: momentum_12_1 and residual_momentum_scores (h1_resid_mom),
raw_signals/composite -- gross profitability and share issuance (h2_composite),
surprise -- SUE/SRUE (h3_pead), rule_sma200/rule_sma10m (h4_trend).

Every function here follows the sign convention the rest of the research code uses:
a HIGHER score should mean a HIGHER expected forward return, matching how
pretest.run_monthly_sort buckets (bucket n_buckets-1 is "top"). Where the underlying
anomaly runs the other way (e.g. asset growth, accruals, idiosyncratic volatility),
the raw measure is negated in-function so callers never have to remember the sign.

Signature convention: `(panel, f, ...) -> (N,) scores`, matching momentum_12_1's
shape, so callers wrap with `lambda f, e: fn(panel, f, ...)` for
pretest.run_monthly_sort exactly as h1_resid_mom.run() already does.
"""
from __future__ import annotations

import warnings

import numpy as np
import pandas as pd

from autotrader.research.h1_resid_mom import MIN_MOM_MONTHS, MonthlyData, ff3_residual_window
from autotrader.research.panel import Panel

LOOKBACK_1M = 21
LOOKBACK_1Y = 252


# ------------------------------------------------------------- price/technical ---

def reversal_1m(panel: Panel, f: int) -> np.ndarray:
    """Short-term reversal (Jegadeesh 1990): last month's losers outperform next month."""
    if f < LOOKBACK_1M:
        return np.full(panel.N, np.nan)
    c = panel.close_tr()
    with np.errstate(invalid="ignore", divide="ignore"):
        r = c[f] / c[f - LOOKBACK_1M] - 1.0
    return -r


def high52w_proximity(panel: Panel, f: int, window: int = LOOKBACK_1Y) -> np.ndarray:
    """52-week-high momentum (George & Hwang 2004): closer to the trailing high, higher score."""
    if f < window:
        return np.full(panel.N, np.nan)
    c = panel.close_tr()
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", category=RuntimeWarning)   # a delisted/unlisted name's window is legitimately all-NaN
        hi = np.nanmax(c[f - window + 1:f + 1], axis=0)
    with np.errstate(invalid="ignore", divide="ignore"):
        return c[f] / hi - 1.0


def idio_vol_ff3(panel: Panel, f: int, md: MonthlyData) -> np.ndarray:
    """Low-volatility anomaly (Ang, Hodrick, Xing & Zhang 2006): NEGATED std of the FF3
    residuals over months m-11..m-1 (reuses h1_resid_mom's regression, not its own)."""
    m = int(md.month_of_session[f])
    window = ff3_residual_window(md, m, np.ones(panel.N, dtype=bool))
    out = np.full(window.shape[0], np.nan)
    for j in range(window.shape[0]):
        w = window[j][np.isfinite(window[j])]
        if len(w) >= MIN_MOM_MONTHS:
            sd = w.std(ddof=1)
            if sd > 0:
                out[j] = -sd
    return out


def amihud_illiquidity(panel: Panel, f: int, window: int = 20) -> np.ndarray:
    """Illiquidity premium (Amihud 2002): mean(|daily return| / dollar volume) over `window`
    sessions; more illiquid names carry a higher expected return, so the raw measure scores up."""
    if f < window:
        return np.full(panel.N, np.nan)
    r = panel.daily_returns()[f - window + 1:f + 1]
    dv = (panel.px["close"] * panel.px["volume"])[f - window + 1:f + 1]
    with np.errstate(invalid="ignore", divide="ignore"):
        ratio = np.where(dv > 0, np.abs(r) / dv, np.nan)
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", category=RuntimeWarning)   # a delisted/unlisted name's window is legitimately all-NaN
        return np.nanmean(ratio, axis=0) * 1e6   # scaled to a readable magnitude; monotonic, rank-only use


# --------------------------------------------------------------------- fundamental ---

def asset_growth(panel: Panel, f: int) -> np.ndarray:
    """Asset growth anomaly (Cooper, Gulen & Schill 2008): NEGATED, low growth scores high."""
    if f < LOOKBACK_1Y:
        return np.full(panel.N, np.nan)
    a = panel.asof_matrix("ARQ", "assets")
    with np.errstate(invalid="ignore", divide="ignore"):
        g = np.where(a[f - LOOKBACK_1Y] > 0, a[f] / a[f - LOOKBACK_1Y] - 1.0, np.nan)
    return -g


def accruals(panel: Panel, f: int) -> np.ndarray:
    """Accruals anomaly (Sloan 1996): NEGATED (netinc - ncfo) / assets, low accruals score high."""
    ni = panel.asof_matrix("ARQ", "netinc")[f]
    cfo = panel.asof_matrix("ARQ", "ncfo")[f]
    a = panel.asof_matrix("ARQ", "assets")[f]
    with np.errstate(invalid="ignore", divide="ignore"):
        acc = np.where(a > 0, (ni - cfo) / a, np.nan)
    return -acc


def book_to_market(panel: Panel, f: int) -> np.ndarray:
    """Value (Fama & French 1992): book equity / market cap; high B/M scores high."""
    eq = panel.asof_matrix("ARQ", "equity")[f]
    mcap = panel.market_cap()[f]
    with np.errstate(invalid="ignore", divide="ignore"):
        return np.where(mcap > 0, eq / mcap, np.nan)


def leverage_change(panel: Panel, f: int) -> np.ndarray:
    """Deleveraging anomaly: NEGATED change in debt/assets over the trailing year."""
    if f < LOOKBACK_1Y:
        return np.full(panel.N, np.nan)
    debt = panel.asof_matrix("ARQ", "debt")
    a = panel.asof_matrix("ARQ", "assets")
    with np.errstate(invalid="ignore", divide="ignore"):
        lev_now = np.where(a[f] > 0, debt[f] / a[f], np.nan)
        lev_then = np.where(a[f - LOOKBACK_1Y] > 0, debt[f - LOOKBACK_1Y] / a[f - LOOKBACK_1Y], np.nan)
    return -(lev_now - lev_then)


# ------------------------------------------------------------------- event-based ---

def insider_net_buying(panel: Panel, f: int, insider_tx: pd.DataFrame, window_days: int = 90) -> np.ndarray:
    """Net open-market insider buying (signed dollars / market cap) over the trailing
    `window_days`. NaN for every name when no insiders data was loaded at all (see
    panel.load_insider_transactions); 0.0 for a specific name with no filings in the
    window, which is a real observation, not missing data.
    """
    if insider_tx.empty:
        return np.full(panel.N, np.nan)
    d = panel.dates[f]
    lo = d - np.timedelta64(window_days, "D")
    win = insider_tx[(insider_tx["filingdate"] > lo) & (insider_tx["filingdate"] <= d)]
    out = np.zeros(panel.N)
    if win.empty:
        return out
    sym_idx = {s: i for i, s in enumerate(panel.symbols)}
    mcap = panel.market_cap()[f]
    for sym, val in win.groupby("ticker")["signed_dollars"].sum().items():
        j = sym_idx.get(sym)
        if j is not None and mcap[j] > 0:
            out[j] = val / mcap[j]
    return out


def recent_material_event_flag(panel: Panel, f: int, event_dates: dict[str, np.ndarray], lookback_days: int = 10) -> np.ndarray:
    """-1.0 for a name with a material 8-K (autotrader.universe.sharadar.MATERIAL_8K_CODES)
    filed in the trailing `lookback_days`, else 0.0. Directional a priori: DESIGN.md's mock
    validator already treats a second material 8-K inside the drift window as a REJECT
    (docs/DESIGN.md 7.1); this encodes the same prior as a rank score instead of a veto.
    """
    d = panel.dates[f]
    lo = d - np.timedelta64(lookback_days, "D")
    out = np.zeros(panel.N)
    for j, sym in enumerate(panel.symbols):
        dates = event_dates.get(sym)
        if dates is not None and dates.size and bool(((dates > lo) & (dates <= d)).any()):
            out[j] = -1.0
    return out
