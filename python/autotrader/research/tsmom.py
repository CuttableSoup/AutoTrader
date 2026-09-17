"""TSMOM-v1: time-series trend-following on a fixed 18-ETF universe.
Spec: docs/prereg/TSMOM-v1.md.

Primary: 12-month sign-of-return, inverse-vol scaled, monthly formation, gross
exposure capped at 1.0x (no leverage -- a deliberate, stated departure from
Moskowitz-Ooi-Pedersen 2012's ~40%-vol-per-instrument target, which assumes
futures-level leverage this is a cash ETF account per the owner's "real, modest
income" goal). Held from the open of session f+1 to the open of the next
formation's f'+1: the outgoing weights earn the overnight gap into f'+1, the
incoming weights earn that day's intraday move (_weight_series below) -- no
single day is misattributed whole to the wrong side of a rebalance, unlike
h4_trend.py's coarser close-to-close book_returns this module's cost model
originally (and wrongly) copied. Idle capital earns the FF risk-free rate.

Secondary (reported only, never gating): 1-month and 3-month lookback variants,
and a long-only variant (mom floored at 0).

Cost: charged on turnover (not a per-trade round trip -- this book holds
continuously and only adjusts weights monthly), using a per-ETF spread estimated
from Corwin & Schultz (2012) on the high/low the `funds` table already returns,
plus the project's existing 10bp conservative floor (commission_bps_per_side:
5.0, already the default in every config/backtest.*.json, round-tripped).

CORRECTED 2026-09-16, before this ran against live data (code review, not a
live-data finding): (1) the day-attribution bug above; (2) a NaN Corwin-Schultz
spread (missing high/low) silently became a 0bp -- free -- cost instead of a
conservative fallback (_fill_missing_spread); (3) FIRST_FORMATION was a
hand-picked date that could fall short of the true 252+60-session warm-up,
replaced with eligible_formations() computed from the panel; (4) etf_panel.py's
on-disk cache stored prices downcast to float32 without ever recovering the lost
precision on a cache hit, risking a "frozen" spec giving different numbers
depending on cache state -- fixed to cache at full float64.

PBO/CSCV is deliberately NOT run here: it measures whether the in-sample winner
of a SEARCH degrades out of sample, and this is one frozen, literature-cited
spec with no selection step -- there is no "winner" for CSCV to interrogate.
"""
from __future__ import annotations

import numpy as np
import pandas as pd

from autotrader.research import pbo, walkforward
from autotrader.research.etf_panel import EtfPanel
from autotrader.research.factors import load_ff3_daily

UNIVERSE = [
    "SPY", "QQQ", "IWM", "EFA", "EEM",       # equity index
    "TLT", "IEF", "SHY", "LQD", "HYG",       # rates/credit
    "GLD", "SLV", "USO", "DBC",              # commodities
    "UUP", "FXE", "FXY", "FXB",              # currencies
]
VOL_LOOKBACK = 60
COMMISSION_BPS = 10.0               # round-tripped commission_bps_per_side floor, matching config/backtest.*.json
SPREAD_WINDOW = 60
TRAIN_SESSIONS = 756                # 3 years
TEST_SESSIONS = 252                 # 1 year
PURGE_SESSIONS = 21                 # 1 month
EMBARGO_SESSIONS = 21               # 1 month

VARIANTS = {
    "primary_12m": {"lookback": 252, "long_only": False},
    "secondary_1m": {"lookback": 21, "long_only": False},
    "secondary_3m": {"lookback": 63, "long_only": False},
    "secondary_long_only_12m": {"lookback": 252, "long_only": True},
}
PRIMARY = "primary_12m"
MAX_LOOKBACK = max(spec["lookback"] for spec in VARIANTS.values())


def eligible_formations(panel: EtfPanel) -> np.ndarray:
    """Month-end formation indices with enough warm-up for every variant's lookback and
    the vol window, computed from the panel rather than a hand-picked start date -- a
    hardcoded guess (e.g. "2017-09-01") can silently fall short of the true 252+60-session
    requirement, letting the first formation or two slip through compute_weights' internal
    `if f < lookback: continue` guard as an all-zero (idle-cash) row instead of a real one.
    """
    all_f = panel.month_end_indices()
    return all_f[all_f >= MAX_LOOKBACK + VOL_LOOKBACK]


# --------------------------------------------------------------------- signal ---

def corwin_schultz_spread(high: np.ndarray, low: np.ndarray) -> np.ndarray:
    """Corwin & Schultz (2012) daily effective spread, as a FRACTION of price.

    Works on (T,) or (T, N) high/low arrays. Row 0 is NaN (needs a 2-day window);
    negative raw estimates (a known property of the estimator in quiet periods) are
    floored at 0.
    """
    k = 3 - 2 * np.sqrt(2.0)
    h1, l1 = high[1:], low[1:]
    h0, l0 = high[:-1], low[:-1]
    with np.errstate(invalid="ignore", divide="ignore"):
        beta = np.log(h1 / l1) ** 2 + np.log(h0 / l0) ** 2
        gamma = np.log(np.maximum(h1, h0) / np.minimum(l1, l0)) ** 2
        alpha = (np.sqrt(2 * beta) - np.sqrt(beta)) / k - np.sqrt(gamma / k)
        spread = 2 * (np.exp(alpha) - 1) / (1 + np.exp(alpha))
    spread = np.maximum(np.nan_to_num(spread, nan=0.0), 0.0)
    out = np.full(high.shape, np.nan)
    out[1:] = spread
    return out


def compute_weights(panel: EtfPanel, formations: np.ndarray, lookback: int, long_only: bool = False,
                    vol_lookback: int = VOL_LOOKBACK) -> np.ndarray:
    """(F, N) inverse-vol-scaled, gross-capped-at-1.0x weights set at each formation."""
    c = panel.close_tr()
    r = panel.daily_returns()
    F, N = len(formations), panel.N
    W = np.zeros((F, N))
    for k, f in enumerate(formations):
        if f < lookback or f < vol_lookback:
            continue
        with np.errstate(invalid="ignore", divide="ignore"):
            mom = np.sign(c[f] / c[f - lookback] - 1.0)
        if long_only:
            mom = np.maximum(mom, 0.0)
        vol = np.nanstd(r[f - vol_lookback + 1:f + 1], axis=0, ddof=1) * np.sqrt(252)
        with np.errstate(invalid="ignore", divide="ignore"):
            raw_w = np.where(vol > 0, mom / vol, 0.0)
        raw_w = np.nan_to_num(raw_w, nan=0.0)
        gross = np.sum(np.abs(raw_w))
        scale = min(1.0, 1.0 / gross) if gross > 0 else 0.0     # DOWN-scale only, never lever above 1.0x
        W[k] = raw_w * scale
    return W


# --------------------------------------------------------------- portfolio/cost ---

def spread_bps_series(panel: EtfPanel, window: int = SPREAD_WINDOW) -> np.ndarray:
    """(T, N) trailing-mean Corwin-Schultz spread, in basis points."""
    cs = corwin_schultz_spread(panel.px["high"], panel.px["low"])
    df = pd.DataFrame(cs)
    return (df.rolling(window, min_periods=window // 2).mean().to_numpy()) * 10000.0


def _weight_series(panel: EtfPanel, formations: np.ndarray, W: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """(T, N) weight vectors split by which half of each session they apply to.

    w_intraday[t] is the weight applied to session t's open-to-close move; formation k's
    weights take effect starting at the OPEN of formations[k]+1, so w_intraday jumps to
    W[k] exactly there. w_overnight[t] is the weight applied to the close(t-1)-to-open(t)
    gap -- whatever was intraday-active the PREVIOUS session, since positions don't change
    overnight without a formation event. This is what makes the transition day correct:
    the outgoing weights earn the overnight gap into the new formation date, the incoming
    weights earn that same day's intraday move -- no single day is misattributed whole.
    """
    T, N = panel.T, panel.N
    w_intraday = np.zeros((T, N))
    for k, f in enumerate(formations):
        start = int(f) + 1
        end = int(formations[k + 1]) + 1 if k + 1 < len(formations) else T
        w_intraday[start:end] = W[k]
    w_overnight = np.zeros((T, N))
    w_overnight[1:] = w_intraday[:-1]
    return w_intraday, w_overnight


def _fill_missing_spread(spr_row: np.ndarray, default_bps: float = 50.0) -> np.ndarray:
    """A NaN spread (missing high/low data) must not silently become a 0bp (free) cost --
    fall back to the widest spread quoted elsewhere in the universe that day, or a fixed
    conservative default if nothing is available at all.
    """
    if np.isfinite(spr_row).any():
        fallback = float(np.nanmax(spr_row))
    else:
        fallback = default_bps
    return np.where(np.isfinite(spr_row), spr_row, fallback)


def portfolio_returns(panel: EtfPanel, formations: np.ndarray, W: np.ndarray, rf: np.ndarray,
                      spread_bps: np.ndarray, commission_bps: float = COMMISSION_BPS) -> tuple[np.ndarray, np.ndarray]:
    """Daily net book return (T,), and per-formation turnover cost as a return fraction (F,)."""
    open_tr, close_tr = panel.open_tr(), panel.close_tr()
    T = panel.T
    with np.errstate(invalid="ignore", divide="ignore"):
        oc_ret = close_tr / open_tr - 1.0                        # (T, N) intraday, open -> close
        co_ret = np.zeros_like(open_tr)
        co_ret[1:] = open_tr[1:] / close_tr[:-1] - 1.0            # (T, N) overnight gap, prior close -> open

    w_intraday, w_overnight = _weight_series(panel, formations, W)
    idle_intraday = 1.0 - np.abs(w_intraday).sum(axis=1)
    with np.errstate(invalid="ignore"):
        book = (np.nansum(w_intraday * oc_ret, axis=1) + np.nansum(w_overnight * co_ret, axis=1)
               + idle_intraday * rf)

    turnover_cost = np.zeros(len(formations))
    prev_w = np.zeros(panel.N)
    for k, f in enumerate(formations):
        w = W[k]
        delta = np.abs(w - prev_w)
        spr = _fill_missing_spread(spread_bps[int(f)])
        turnover_cost[k] = float(np.sum(delta * (spr + commission_bps) / 2.0 / 10000.0))
        prev_w = w
        entry = int(f) + 1                                        # first session the new weights are intraday-active
        if entry < T and np.isfinite(book[entry]):
            book[entry] -= turnover_cost[k]
    return book, turnover_cost


# ------------------------------------------------------------------------- run ---

def run_variant(panel: EtfPanel, formations: np.ndarray, spec: dict, rf: np.ndarray, spr: np.ndarray) -> np.ndarray:
    W = compute_weights(panel, formations, spec["lookback"], spec["long_only"])
    book, _ = portfolio_returns(panel, formations, W, rf, spr)
    return book


def run(panel: EtfPanel, ff_daily: pd.DataFrame | None = None, dsr_cli: str | None = None) -> tuple[dict, str, int]:
    ff = ff_daily if ff_daily is not None else load_ff3_daily()
    rf = ff["rf"].reindex(pd.DatetimeIndex(panel.dates.astype("datetime64[ns]"))).fillna(0.0).to_numpy()
    spr = spread_bps_series(panel)
    formations = eligible_formations(panel)

    books = {name: run_variant(panel, formations, spec, rf, spr) for name, spec in VARIANTS.items()}
    primary_book = books[PRIMARY]

    folds = walkforward.purge_embargo_folds(panel.T, TRAIN_SESSIONS, TEST_SESSIONS, PURGE_SESSIONS, EMBARGO_SESSIONS)
    if not folds:
        raise RuntimeError(f"panel has {panel.T} sessions, too few for a single "
                           f"{TRAIN_SESSIONS}+{PURGE_SESSIONS}+{TEST_SESSIONS}-session fold")

    oos_by_variant = {name: np.concatenate([b[f.test_start:f.test_end] for f in folds]) for name, b in books.items()}
    oos_primary = oos_by_variant[PRIMARY]
    oos_primary_finite = oos_primary[np.isfinite(oos_primary)]

    fold_returns = [float(np.prod(1.0 + np.nan_to_num(primary_book[f.test_start:f.test_end], nan=0.0)) - 1.0)
                    for f in folds]
    folds_net_positive = sum(1 for x in fold_returns if x > 0)

    thirds = walkforward.sub_period_stability(oos_primary_finite, 3)

    trial_sharpes = [pbo.sharpe_like(oos_by_variant[name]) for name in VARIANTS]
    dsr = walkforward.call_dsr_cli(oos_primary_finite.tolist(), trial_sharpes, periods_per_year=252.0, dsr_cli=dsr_cli)

    checks = {
        "dsr_positive": dsr["deflated_sharpe_annual"] > 0,
        "folds_net_positive": folds_net_positive >= max(2, int(np.ceil(2 * len(folds) / 3))),
        "sub_period_stability": all(t > 0 for t in thirds),
        "min_sample_folds": len(folds) >= 4,
    }
    passed = all(checks.values())

    result = {
        "hypothesis": "TSMOM-v1", "n_folds": len(folds), "fold_returns_pct": [x * 100 for x in fold_returns],
        "folds_net_positive": folds_net_positive, "sub_period_thirds_pct": [t * 100 for t in thirds],
        "dsr": dsr, "checks": checks, "pass": passed,
        "variant_sharpe_per_period": {name: pbo.sharpe_like(oos_by_variant[name]) for name in VARIANTS},
    }
    lines = [
        f"## TSMOM-v1 ({len(formations)} monthly formations from {panel.dates[formations[0]]}, {len(folds)} OOS folds)",
        "",
        "| fold | test start | test end | return % |", "|---|---|---|---|",
    ]
    for i, (f, ret) in enumerate(zip(folds, fold_returns)):
        lines.append(f"| {i} | {panel.dates[f.test_start]} | {panel.dates[f.test_end - 1]} | {ret * 100:+.2f} |")
    lines += ["", f"Sub-period thirds (% total return): {', '.join(f'{t:+.2f}' for t in result['sub_period_thirds_pct'])}", "",
             f"Deflated Sharpe (annual): {dsr['deflated_sharpe_annual']:+.3f}, n_trials={dsr['n_trials']}, "
             f"P(SR>SR0)={dsr['dsr_probability']:.3f}", "",
             "| variant | per-period Sharpe (unannualised) |", "|---|---|"]
    for name, sh in result["variant_sharpe_per_period"].items():
        tag = " (primary)" if name == PRIMARY else ""
        lines.append(f"| {name}{tag} | {sh:+.4f} |")
    lines += ["", f"**Verdict: {'PASS' if passed else 'FAIL'}**", "",
             "| check | result |", "|---|---|"] + [f"| {k} | {'OK' if v else 'FAIL'} |" for k, v in checks.items()]
    return result, "\n".join(lines), len(VARIANTS)
