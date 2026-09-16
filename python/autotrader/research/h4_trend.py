"""H4: market trend overlay, long-or-flat. Spec: docs/prereg/H4-trend-overlay.md.

Books (all in the "all" band, rebalanced monthly):
  universe_ew   every eligible name, equal weight
  h1_top_decile residual momentum top decile (H1's score)
  h2_top_quint  composite top quintile (H2's score)
Membership chosen at the close of month-end session f is held from the close of f+1
to the close of the next formation's f'+1: a full session of lag, no same-close trading.
Daily book return = equal-weight mean of member close-to-close total returns.

Rules, applied with one more session of lag (a signal from close d-2 sets exposure for
the close d-1 -> close d return); flat days earn the FF daily risk-free rate:
  sma200   SPY total-return close > its 200-session mean        (primary)
  sma10m   SPY month-end close > mean of the last 10 month-end closes, set once a month
Pass per book: max drawdown at most 75% of the unfiltered book's AND annual Sharpe
(excess of rf) no more than 0.10 below it.
"""
from __future__ import annotations

import numpy as np
import pandas as pd

from autotrader.research import stats
from autotrader.research.factors import load_ff3_daily
from autotrader.research.h1_resid_mom import MonthlyData, residual_momentum_scores
from autotrader.research.h2_composite import composite, raw_signals
from autotrader.research.panel import Panel

FIRST_FORMATION = "2018-09-01"
MDD_RATIO_MAX = 0.75
SHARPE_GIVEBACK_MAX = 0.10


def book_returns(panel: Panel, formations: np.ndarray, members_fn) -> np.ndarray:
    r = panel.daily_returns()
    out = np.full(panel.T, np.nan)
    for k, f in enumerate(formations):
        start = f + 2                                            # first close-to-close return after holding from close f+1
        end = formations[k + 1] + 1 if k + 1 < len(formations) else panel.T - 1
        members = members_fn(int(f))
        if not members.any():
            continue
        block = r[start:end + 1][:, members]
        with np.errstate(invalid="ignore"):
            out[start:end + 1] = np.where(np.isfinite(block).any(axis=1), np.nanmean(block, axis=1), 0.0)
    return out


def rule_sma200(panel: Panel) -> np.ndarray:
    c = panel.spy_close_tr
    sma = pd.Series(c).rolling(200).mean().to_numpy()
    on = c > sma
    expo = np.zeros(panel.T, dtype=bool)
    expo[2:] = on[:-2] & np.isfinite(sma[:-2])
    return expo


def rule_sma10m(panel: Panel) -> np.ndarray:
    ends = panel.month_end_indices()
    c = panel.spy_close_tr
    expo = np.zeros(panel.T, dtype=bool)
    for k in range(9, len(ends)):
        on = c[ends[k]] > np.mean(c[ends[k - 9:k + 1]])
        lo = ends[k] + 2
        hi = ends[k + 1] + 2 if k + 1 < len(ends) else panel.T
        expo[lo:hi] = on
    return expo


def evaluate(book: np.ndarray, expo: np.ndarray, rf: np.ndarray) -> dict:
    ok = np.isfinite(book)
    base = book[ok]
    filt = np.where(expo[ok], book[ok], rf[ok])
    ex_base, ex_filt = base - rf[ok], filt - rf[ok]
    b = {"sharpe": stats.sharpe_annual(ex_base), "max_dd": stats.max_drawdown(base), "cagr": float(np.prod(1 + base) ** (252 / len(base)) - 1)}
    o = {"sharpe": stats.sharpe_annual(ex_filt), "max_dd": stats.max_drawdown(filt), "cagr": float(np.prod(1 + filt) ** (252 / len(filt)) - 1),
         "share_invested": float(expo[ok].mean())}
    reasons = []
    if not (o["max_dd"] <= MDD_RATIO_MAX * b["max_dd"]):
        reasons.append(f"max drawdown {o['max_dd']:.1%} vs {b['max_dd']:.1%} unfiltered (needs <= {MDD_RATIO_MAX:.0%} of it)")
    if not (o["sharpe"] >= b["sharpe"] - SHARPE_GIVEBACK_MAX):
        reasons.append(f"Sharpe {o['sharpe']:.2f} vs {b['sharpe']:.2f} unfiltered (gives back more than {SHARPE_GIVEBACK_MAX})")
    return {"base": b, "overlay": o, "pass": not reasons, "fail_reasons": reasons, "days": int(ok.sum())}


def run(panel: Panel, ff_daily: pd.DataFrame | None = None, first_formation: str = FIRST_FORMATION) -> tuple[dict, str, int]:
    ff = ff_daily if ff_daily is not None else load_ff3_daily()
    rf = ff["rf"].reindex(pd.DatetimeIndex(panel.dates.astype("datetime64[ns]"))).fillna(0.0).to_numpy()
    formations = panel.month_end_indices(start=first_formation)
    mask = panel.universe_mask()
    md = MonthlyData(panel, ff)

    def top(scores: np.ndarray, elig: np.ndarray, frac: float) -> np.ndarray:
        s = np.where(elig, scores, np.nan)
        b = stats.assign_buckets(s, int(round(1 / frac)))
        return b == int(round(1 / frac)) - 1

    books = {
        "universe_ew": book_returns(panel, formations, lambda f: mask[f].copy()),
        "h1_top_decile": book_returns(panel, formations, lambda f: top(residual_momentum_scores(md, int(md.month_of_session[f]), mask[f]), mask[f], 0.1)),
        "h2_top_quint": book_returns(panel, formations, lambda f: top(composite(raw_signals(panel, f), mask[f]), mask[f], 0.2)),
    }
    rules = {"sma200": rule_sma200(panel), "sma10m": rule_sma10m(panel)}
    result = {"hypothesis": "H4", "first_formation": str(panel.dates[formations[0]]),
              "books": {bk: {rk: evaluate(ret, ex, rf) for rk, ex in rules.items()} for bk, ret in books.items()}}
    for bk in result["books"]:
        result["books"][bk]["sma10m"]["primary"] = False
        result["books"][bk]["sma200"]["primary"] = True
    lines = [f"## H4 trend overlay (books from {result['first_formation']}, band all)", "",
             "| book | rule | days | Sharpe base | Sharpe overlay | MaxDD base | MaxDD overlay | CAGR base | CAGR overlay | invested | verdict |",
             "|---|---|---|---|---|---|---|---|---|---|---|"]
    for bk, per in result["books"].items():
        for rk, r in per.items():
            verdict = ("**PASS**" if r["pass"] else "FAIL") if r["primary"] else "(secondary)"
            lines.append(f"| {bk} | {rk}{'*' if r['primary'] else ''} | {r['days']} | {r['base']['sharpe']:.2f} | {r['overlay']['sharpe']:.2f} | "
                         f"{r['base']['max_dd']:.1%} | {r['overlay']['max_dd']:.1%} | {r['base']['cagr']:.1%} | {r['overlay']['cagr']:.1%} | "
                         f"{r['overlay']['share_invested']:.0%} | {verdict} |")
    for bk, per in result["books"].items():
        if not per["sma200"]["pass"]:
            lines.append(f"\n{bk} sma200 fail reasons: " + "; ".join(per["sma200"]["fail_reasons"]))
    return result, "\n".join(lines), len(books) * len(rules)
