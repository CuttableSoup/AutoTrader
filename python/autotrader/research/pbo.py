"""Probability of Backtest Overfitting via Combinatorially Symmetric Cross-Validation
(Bailey, Borwein, Lopez de Prado & Zhu, 2014).

Deflated Sharpe (docs/trials.jsonl, cpp/src/backtester/metrics.cpp) corrects the
significance bar for how many configurations a search tried. It says nothing about
whether the specific config a search selects then degrades out of sample -- that is
what CSCV measures, and it is the natural companion to a brute-force sweep rather
than a one-at-a-time pre-registered hypothesis.

Method: split the T formation periods into `n_splits` contiguous, near-equal blocks.
For every way to assign half the blocks to train and half to test (C(n_splits,
n_splits/2) combinations, each block-set used once as train with its complement as
test), pick the in-sample winner by `metric` on train, then find that winner's
RELATIVE RANK among all configs' `metric` on test. Convert the rank to a logit.

PBO is the share of splits where the logit is <= 0: the in-sample winner finished at
or below the test-period median. Pure noise across many configs gives PBO near 0.5
(the in-sample winner carries no out-of-sample information). A column with a real,
consistent edge gives a low PBO (it keeps winning out of sample too).
"""
from __future__ import annotations

import itertools
import math
from typing import Callable

import numpy as np


def sharpe_like(x: np.ndarray) -> float:
    """Per-period Sharpe proxy (mean/std) for ranking only -- not annualised.

    A monotonic transform of a proper Sharpe, which is all CSCV ranking needs.
    """
    x = x[np.isfinite(x)]
    if len(x) < 2:
        return float("-inf")
    sd = x.std(ddof=1)
    if sd > 0:
        return float(x.mean() / sd)
    return float("inf") if x.mean() > 0 else (float("-inf") if x.mean() < 0 else 0.0)


def cscv_pbo(returns: np.ndarray, n_splits: int = 16, metric: Callable[[np.ndarray], float] = sharpe_like) -> dict:
    """returns: (T, N) per-period returns, T periods (rows) x N configs (columns).

    n_splits must be even and <= T (16 -> C(16,8) = 12,870 splits). Raises ValueError
    otherwise, rather than silently truncating -- a truncated split count is not what
    the caller asked to run.
    """
    T, N = returns.shape
    if n_splits % 2 != 0:
        raise ValueError(f"n_splits must be even, got {n_splits}")
    if n_splits > T:
        raise ValueError(f"n_splits ({n_splits}) exceeds the number of periods ({T})")
    if N < 2:
        raise ValueError("cscv_pbo needs at least 2 configs to rank")
    blocks = np.array_split(np.arange(T), n_splits)
    logits: list[float] = []
    below_median = 0
    for train_blocks in itertools.combinations(range(n_splits), n_splits // 2):
        test_blocks = [b for b in range(n_splits) if b not in train_blocks]
        train_rows = np.concatenate([blocks[b] for b in train_blocks])
        test_rows = np.concatenate([blocks[b] for b in test_blocks])
        train_perf = np.array([metric(returns[train_rows, c]) for c in range(N)])
        best = int(np.argmax(train_perf))
        test_perf = np.array([metric(returns[test_rows, c]) for c in range(N)])
        # relative rank of the in-sample winner within the test-period performance distribution, in (0, 1]
        rank = float(np.sum(test_perf <= test_perf[best])) / N
        w = min(max(rank, 1.0 / (N + 1)), N / (N + 1))    # keep the logit finite
        logit = math.log(w / (1.0 - w))
        logits.append(logit)
        if logit <= 0:
            below_median += 1
    n = len(logits)
    return {
        "n_splits_evaluated": n,
        "n_configs": N,
        "n_periods": T,
        "pbo": below_median / n,
        "mean_logit": float(np.mean(logits)),
    }
