"""H1: residual (idiosyncratic) momentum. Spec: docs/prereg/H1-residual-momentum.md.

At the last session of month m:
  * monthly total returns in excess of the risk-free rate are regressed on FF3 over the
    36 months m-35..m (at least 24 valid months);
  * score = sum of residuals over m-11..m-1 divided by their standard deviation
    (at least 8 of the 11 valid). Month m itself is skipped, as in 12-1 momentum.
Reference arm: vanilla 12-1 momentum, close_tr[f-21] / close_tr[f-252] - 1.
"""
from __future__ import annotations

import numpy as np
import pandas as pd

from autotrader.research import pretest
from autotrader.research.factors import compound_monthly, load_ff3_daily, monthly_from_daily
from autotrader.research.panel import Panel

REG_MONTHS = 36
MIN_REG_MONTHS = 24
MOM_MONTHS = 11          # m-11 .. m-1
MIN_MOM_MONTHS = 8
N_BUCKETS = 10
HORIZONS = [20, 60, 120]
PRIMARY = 60
BANDS = ["5B+", "2B-5B", "500M-2B", "all"]
FIRST_FORMATION = "2018-09-01"


class MonthlyData:
    def __init__(self, panel: Panel, ff_daily: pd.DataFrame | None = None):
        month_ids = panel.dates.astype("datetime64[M]")
        self.months, self.ret = compound_monthly(panel.daily_returns(), month_ids)      # (M,), (M, N)
        ff = monthly_from_daily(ff_daily if ff_daily is not None else load_ff3_daily())
        ff.index = ff.index.to_period("M")
        per = pd.PeriodIndex(self.months.astype("datetime64[M]").astype(str), freq="M")
        ffm = ff.reindex(per)
        self.factors = ffm[["mkt_rf", "smb", "hml"]].to_numpy()                          # (M, 3), NaN if not published
        self.rf = ffm["rf"].to_numpy()
        self.month_of_session = np.searchsorted(self.months, month_ids)


def residual_momentum_scores(md: MonthlyData, m: int, candidates: np.ndarray) -> np.ndarray:
    """(N,) residual momentum at the end of month index m for the candidate columns."""
    N = md.ret.shape[1]
    out = np.full(N, np.nan)
    lo = m - REG_MONTHS + 1
    if lo < 0:
        return out
    X = md.factors[lo:m + 1]
    if not np.isfinite(X).all():
        return out
    Xc = np.column_stack([np.ones(len(X)), X])
    Y = md.ret[lo:m + 1] - md.rf[lo:m + 1, None]
    for j in np.nonzero(candidates)[0]:
        y = Y[:, j]
        ok = np.isfinite(y)
        if ok.sum() < MIN_REG_MONTHS:
            continue
        beta, *_ = np.linalg.lstsq(Xc[ok], y[ok], rcond=None)
        resid = y - Xc @ beta                                  # NaN where y is NaN
        window = resid[REG_MONTHS - 1 - MOM_MONTHS:REG_MONTHS - 1]   # months m-11 .. m-1
        w = window[np.isfinite(window)]
        if len(w) < MIN_MOM_MONTHS:
            continue
        sd = w.std(ddof=1)
        if sd > 0:
            out[j] = w.sum() / sd
    return out


def momentum_12_1(panel: Panel, f: int) -> np.ndarray:
    if f < 252:
        return np.full(panel.N, np.nan)
    c = panel.close_tr()
    with np.errstate(invalid="ignore", divide="ignore"):
        return c[f - 21] / c[f - 252] - 1.0


def run(panel: Panel, ff_daily: pd.DataFrame | None = None, first_formation: str = FIRST_FORMATION) -> tuple[dict, str, int]:
    md = MonthlyData(panel, ff_daily)
    formations = panel.month_end_indices(start=first_formation)
    cache: dict[int, np.ndarray] = {}

    def resid_score(f: int, elig: np.ndarray) -> np.ndarray:
        if f not in cache:
            all_elig = panel.universe_mask()[f]
            cache[f] = residual_momentum_scores(md, int(md.month_of_session[f]), all_elig)
        return cache[f]

    resid = pretest.run_monthly_sort(panel, formations, resid_score, N_BUCKETS, HORIZONS, BANDS)
    vanilla = pretest.run_monthly_sort(panel, formations, lambda f, e: momentum_12_1(panel, f), N_BUCKETS, HORIZONS, BANDS)
    result = {
        "hypothesis": "H1", "formations": [str(panel.dates[f]) for f in formations],
        "residual_momentum": pretest.summarize_sort(resid, PRIMARY, 21),
        "reference_vanilla_12_1": pretest.summarize_sort(vanilla, PRIMARY, 21),
    }
    md_text = "\n\n".join([
        f"## H1 residual momentum ({len(formations)} monthly formations, {result['formations'][0]} .. {result['formations'][-1]})",
        pretest.render_sort_markdown("Residual momentum (gating arm)", result["residual_momentum"], N_BUCKETS),
        pretest.render_sort_markdown("Reference: vanilla 12-1 momentum (not gating)", result["reference_vanilla_12_1"], N_BUCKETS),
    ])
    n_configs = 2 * len(BANDS) * len(HORIZONS)
    return result, md_text, n_configs
