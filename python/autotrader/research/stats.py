"""Statistics for the pre-tests: Newey-West t, bucket sorts, monotonicity, turnover."""
from __future__ import annotations

import math

import numpy as np
from scipy import stats as sps


def newey_west_t(x: np.ndarray, lags: int) -> float:
    """t-statistic of the mean of x with a Bartlett-kernel HAC standard error.

    Monthly formations with 60-session holds overlap, so plain t overstates
    significance; lags should cover the overlap (ceil(h / 21)).
    """
    x = np.asarray(x, dtype=float)
    x = x[np.isfinite(x)]
    n = len(x)
    if n < 3:
        return 0.0
    e = x - x.mean()
    lr_var = float(e @ e) / n
    for k in range(1, min(lags, n - 1) + 1):
        w = 1.0 - k / (lags + 1.0)
        lr_var += 2.0 * w * float(e[k:] @ e[:-k]) / n
    if lr_var <= 0:
        return 0.0
    return float(x.mean() / math.sqrt(lr_var / n))


def nw_lags(horizon_sessions: int, spacing_sessions: int = 21) -> int:
    """ceil(h / spacing), at least 1: covers the overlap of consecutive holding windows."""
    return max(1, math.ceil(horizon_sessions / spacing_sessions))


def assign_buckets(scores: np.ndarray, n_buckets: int) -> np.ndarray:
    """Bucket index 0..n-1 by rank (n-1 = highest score); -1 where the score is missing.

    Rank-based so ties and skew do not empty a bucket.
    """
    s = np.asarray(scores, dtype=float)
    out = np.full(s.shape, -1, dtype=np.int16)
    ok = np.isfinite(s)
    m = int(ok.sum())
    if m < n_buckets:
        return out
    ranks = sps.rankdata(s[ok], method="ordinal") - 1          # 0..m-1
    out[ok] = np.minimum((ranks * n_buckets) // m, n_buckets - 1).astype(np.int16)
    return out


def spearman_monotonicity(bucket_means: list[float]) -> float:
    vals = np.asarray(bucket_means, dtype=float)
    ok = np.isfinite(vals)
    if ok.sum() < 3:
        return float("nan")
    rho = sps.spearmanr(np.arange(len(vals))[ok], vals[ok]).statistic
    return float(rho)


def one_sided_turnover(prev: set[str] | set[int], cur: set[str] | set[int]) -> float:
    """Fraction of the current bucket that was not in the previous one."""
    if not cur:
        return float("nan")
    return len(cur - prev) / len(cur)


def thirds(n: int) -> list[slice]:
    a, b = n // 3, 2 * n // 3
    return [slice(0, a), slice(a, b), slice(b, n)]


def winsorized_z(x: np.ndarray, lo_pct: float = 1.0, hi_pct: float = 99.0) -> np.ndarray:
    x = np.asarray(x, dtype=float)
    out = np.full(x.shape, np.nan)
    ok = np.isfinite(x)
    if ok.sum() < 3:
        return out
    lo, hi = np.percentile(x[ok], [lo_pct, hi_pct])
    c = np.clip(x[ok], lo, hi)
    sd = c.std(ddof=1)
    out[ok] = (c - c.mean()) / sd if sd > 0 else 0.0
    return out


def sharpe_annual(daily: np.ndarray, periods: float = 252.0) -> float:
    d = np.asarray(daily, dtype=float)
    d = d[np.isfinite(d)]
    if len(d) < 3 or d.std(ddof=1) == 0:
        return 0.0
    return float(d.mean() / d.std(ddof=1) * math.sqrt(periods))


def max_drawdown(daily: np.ndarray) -> float:
    """Maximum peak-to-trough loss of the compounded series, as a positive fraction."""
    d = np.nan_to_num(np.asarray(daily, dtype=float))
    eq = np.cumprod(1.0 + d)
    peak = np.maximum.accumulate(np.concatenate([[1.0], eq]))[1:]
    return float(np.max(1.0 - eq / peak)) if len(eq) else 0.0
