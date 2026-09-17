"""Purge/embargo fold geometry and the shared at_dsr CLI caller, factored out so any
research module (not just sweep.py's grid) can run a walk-forward split and get a
Deflated Sharpe without re-deriving either.

Fold geometry ports cpp/src/backtester/walk_forward.cpp's rolling train/purge/test
split to Python. For TSMOM-v1 (docs/prereg/TSMOM-v1.md), purge/embargo's classical
leakage-prevention justification doesn't strictly apply -- the 252-session/60-session
windows are cited constants, not fitted parameters -- but it still buys several
genuinely independent OOS blocks to check individually, and keeps this module
consistent with the rest of the project's gate discipline.
"""
from __future__ import annotations

import json
import subprocess
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from autotrader.research.sweep import find_dsr_cli


@dataclass
class Fold:
    train_start: int    # inclusive
    train_end: int       # exclusive
    test_start: int       # exclusive of the purge gap
    test_end: int          # exclusive


def purge_embargo_folds(T: int, train_sessions: int, test_sessions: int, purge_sessions: int,
                        embargo_sessions: int) -> list[Fold]:
    """Rolling folds over a (T,)-length daily index.

    Fold k: train = [k*step, k*step + train_sessions), then a `purge_sessions` gap,
    then test = [test_start, test_start + test_sessions). `step = test_sessions +
    embargo_sessions`, so consecutive test windows sit exactly `embargo_sessions`
    apart -- each fold's own lookback-window computations get a clean gap at both
    ends of its test block, and no two folds' test windows touch.
    """
    step = test_sessions + embargo_sessions
    folds: list[Fold] = []
    k = 0
    while True:
        train_start = k * step
        train_end = train_start + train_sessions
        test_start = train_end + purge_sessions
        test_end = test_start + test_sessions
        if test_end > T:
            break
        folds.append(Fold(train_start, train_end, test_start, test_end))
        k += 1
    return folds


def call_dsr_cli(returns: list[float], trial_sharpes_per_period: list[float], periods_per_year: float,
                 dsr_cli: str | Path | None = None) -> dict:
    """Subprocess call to at_dsr, the same request shape sweep.py's run_dsr() already
    makes. Reuses sweep.find_dsr_cli() rather than re-deriving the build-output search
    path.
    """
    payload = {"returns": list(returns), "trial_sharpes_per_period": list(trial_sharpes_per_period),
              "periods_per_year": periods_per_year}
    exe = find_dsr_cli(dsr_cli)
    proc = subprocess.run([str(exe)], input=json.dumps(payload), capture_output=True, text=True, timeout=60)
    if proc.returncode != 0:
        raise RuntimeError(f"at_dsr failed (exit {proc.returncode}): {proc.stderr.strip()}")
    return json.loads(proc.stdout)


def sub_period_stability(returns: np.ndarray, n_parts: int = 3) -> list[float]:
    """Total return of each of `n_parts` roughly-equal contiguous slices of `returns`."""
    r = np.asarray(returns, dtype=float)
    r = r[np.isfinite(r)]
    parts = np.array_split(r, n_parts)
    return [float(np.prod(1.0 + p) - 1.0) if len(p) else float("nan") for p in parts]
