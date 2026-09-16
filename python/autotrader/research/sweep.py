"""The brute-force signal grid: docs/prereg/BRUTEFORCE-v1.md.

Where H1-H4 each pre-registered one hypothesis and ran it once, this runs a wide,
frozen grid of signal x bucket-count x horizon x band combinations in one shot, and
leans on two independent overfitting corrections instead of a narrow pre-registered
pass rule:

  * Deflated Sharpe Ratio (cpp/src/backtester/metrics.cpp, via the at_dsr CLI) --
    the same machinery every gate in this repo already uses, penalised for the FULL
    grid size, not just the winner.
  * Probability of Backtest Overfitting / CSCV (autotrader.research.pbo) -- does the
    in-sample-best config actually degrade out of sample across many resampled splits?

Everything here runs on the pre-seal panel (autotrader.research.seal.SEAL_DATE). One
run appends exactly one `sweep` ledger entry sized to the grid, whatever the outcome.
"""
from __future__ import annotations

import datetime as dt
import json
import subprocess
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from autotrader.research import pbo as pbo_mod
from autotrader.research import pretest, stats
from autotrader.research.factors import load_ff3_daily
from autotrader.research.h1_resid_mom import MonthlyData, momentum_12_1, residual_momentum_scores
from autotrader.research.h2_composite import raw_signals as h2_raw_signals
from autotrader.research import signals as sig
from autotrader.research.panel import Panel, load_event_dates, load_insider_transactions, root
from autotrader.universe.sharadar import MATERIAL_8K_CODES

N_BUCKETS_GRID = (3, 5, 10)
HORIZONS = (5, 10, 20, 40, 60, 120)
BANDS = ("500M-2B", "2B-5B", "5B+", "all")
PRIMARY_HORIZON = 60
FIRST_FORMATION = "2018-09-01"
NW_SPACING = 21
MIN_FORMATIONS = 10          # a cell with fewer valid top-bucket months is dropped from DSR/PBO, not from the trial count
PBO_SPLITS = 16
DSR_CLI_CANDIDATES = ("cpp/build/cyg-full/bin/at_dsr", "cpp/build/cyg-full/bin/at_dsr.exe",
                      "cpp/build/fetchcontent-release/bin/at_dsr", "cpp/build/fetchcontent-release/bin/at_dsr.exe")

RAW_SIGNAL_NAMES = ("mom_12_1", "resid_mom", "reversal_1m", "high52w", "idio_vol", "amihud",
                    "gpa", "iss", "asset_growth", "accruals", "book_to_market", "leverage_change",
                    "insider_buy", "event_flag")
COMPOSITE_FAMILIES = {
    "momentum_family": ("mom_12_1", "resid_mom", "reversal_1m", "high52w"),
    "quality_family": ("gpa", "accruals", "leverage_change"),
    "value_issuance_family": ("book_to_market", "iss", "asset_growth"),
    "risk_family": ("idio_vol", "amihud"),
    "ownership_family": ("insider_buy", "event_flag"),
}
N_CONFIGS = (len(RAW_SIGNAL_NAMES) + len(COMPOSITE_FAMILIES)) * len(N_BUCKETS_GRID) * len(HORIZONS) * len(BANDS)


@dataclass
class Cell:
    arm: str
    n_buckets: int
    horizon: int
    band: str
    summary: dict                # pretest.summarize() output
    returns: np.ndarray = field(repr=False)   # (F,) top-bucket excess return FRACTION, NaN where too thin; aligned to `formations`


def build_arms(panel: Panel, ff_daily=None) -> dict:
    """name -> score_fn(f, eligible_mask) -> (N,) scores, for every raw signal + composite family."""
    md = MonthlyData(panel, ff_daily if ff_daily is not None else load_ff3_daily())
    insider_tx = load_insider_transactions(panel.symbols)
    event_dates = load_event_dates(panel.symbols, MATERIAL_8K_CODES)
    h2_cache: dict[int, dict] = {}

    def h2sig(f: int) -> dict:
        if f not in h2_cache:
            h2_cache[f] = h2_raw_signals(panel, f)
        return h2_cache[f]

    raw = {
        "mom_12_1": lambda f, e: momentum_12_1(panel, f),
        "resid_mom": lambda f, e: residual_momentum_scores(md, int(md.month_of_session[f]), panel.universe_mask()[f]),
        "reversal_1m": lambda f, e: sig.reversal_1m(panel, f),
        "high52w": lambda f, e: sig.high52w_proximity(panel, f),
        "idio_vol": lambda f, e: sig.idio_vol_ff3(panel, f, md),
        "amihud": lambda f, e: sig.amihud_illiquidity(panel, f),
        "gpa": lambda f, e: h2sig(f)["gpa"],
        "iss": lambda f, e: h2sig(f)["iss"],
        "asset_growth": lambda f, e: sig.asset_growth(panel, f),
        "accruals": lambda f, e: sig.accruals(panel, f),
        "book_to_market": lambda f, e: sig.book_to_market(panel, f),
        "leverage_change": lambda f, e: sig.leverage_change(panel, f),
        "insider_buy": lambda f, e: sig.insider_net_buying(panel, f, insider_tx),
        "event_flag": lambda f, e: sig.recent_material_event_flag(panel, f, event_dates),
    }
    assert set(raw) == set(RAW_SIGNAL_NAMES)

    def zmean(names: tuple[str, ...], f: int, elig: np.ndarray) -> np.ndarray:
        zs = []
        for name in names:
            v = np.asarray(raw[name](f, elig), dtype=float)
            z = np.full(v.shape, np.nan)
            z[elig] = stats.winsorized_z(v[elig])
            zs.append(z)
        Z = np.vstack(zs)
        n = np.isfinite(Z).sum(axis=0)
        c = np.where(n > 0, np.nansum(Z, axis=0) / np.maximum(n, 1), np.nan)
        c[n < min(2, len(names))] = np.nan
        return c

    composites = {name: (lambda f, e, members=members: zmean(members, f, e)) for name, members in COMPOSITE_FAMILIES.items()}
    return {**raw, **composites}


def run_grid(panel: Panel, first_formation: str = FIRST_FORMATION) -> list[Cell]:
    arms = build_arms(panel)
    formations = panel.month_end_indices(start=first_formation)
    cells: list[Cell] = []
    for arm_name, score_fn in arms.items():
        for n_buckets in N_BUCKETS_GRID:
            series = pretest.run_monthly_sort(panel, formations, score_fn, n_buckets, list(HORIZONS), list(BANDS))
            for band in BANDS:
                for h in HORIZONS:
                    s = series[band][h]
                    summary = pretest.summarize(s, h, NW_SPACING, pretest.ROUND_TRIP_BPS[band], h == PRIMARY_HORIZON)
                    top_frac = s.means[:, n_buckets - 1] / 100.0   # summarize() works in percent; DSR/PBO want fractions
                    cells.append(Cell(arm_name, n_buckets, h, band, summary, top_frac))
    return cells


# ------------------------------------------------------------- overfitting control ---

def find_dsr_cli(explicit: str | Path | None = None) -> Path:
    if explicit:
        p = Path(explicit)
        if p.exists():
            return p
        raise FileNotFoundError(f"at_dsr not found at {p}")
    for cand in DSR_CLI_CANDIDATES:
        p = root() / cand
        if p.exists():
            return p
    tried = ", ".join(DSR_CLI_CANDIDATES)
    raise FileNotFoundError(f"at_dsr not built; tried {tried} (build the cygwin target, see CLAUDE.md)")


def run_dsr(cells: list[Cell], periods_per_year: float = 12.0, dsr_cli: str | Path | None = None) -> dict:
    """Deflated Sharpe of the grid's best cell, penalised for every cell's per-period Sharpe."""
    trial_sharpes = [pbo_mod.sharpe_like(c.returns) for c in cells]
    finite = [(i, s) for i, s in enumerate(trial_sharpes) if np.isfinite(s)]
    if not finite:
        raise RuntimeError("no cell produced a finite Sharpe; nothing to deflate")
    best_i = max(finite, key=lambda t: t[1])[0]
    best_returns = cells[best_i].returns
    best_returns = best_returns[np.isfinite(best_returns)].tolist()
    payload = {"returns": best_returns, "trial_sharpes_per_period": [s for s in trial_sharpes if np.isfinite(s)],
              "periods_per_year": periods_per_year}
    exe = find_dsr_cli(dsr_cli)
    proc = subprocess.run([str(exe)], input=json.dumps(payload), capture_output=True, text=True, timeout=60)
    if proc.returncode != 0:
        raise RuntimeError(f"at_dsr failed (exit {proc.returncode}): {proc.stderr.strip()}")
    result = json.loads(proc.stdout)
    result["best_cell"] = {"arm": cells[best_i].arm, "n_buckets": cells[best_i].n_buckets,
                           "horizon": cells[best_i].horizon, "band": cells[best_i].band}
    result["var_sr_trials"] = float(np.var([s for s in trial_sharpes if np.isfinite(s)], ddof=1))
    return result


def run_pbo(cells: list[Cell], n_splits: int = PBO_SPLITS) -> dict:
    """CSCV over every cell with at least MIN_FORMATIONS valid formations, sharing one time index."""
    usable = [c for c in cells if np.isfinite(c.returns).sum() >= MIN_FORMATIONS]
    if len(usable) < 2:
        raise RuntimeError(f"fewer than 2 usable cells (>= {MIN_FORMATIONS} valid formations each); cannot run CSCV")
    M = np.column_stack([c.returns for c in usable])
    return pbo_mod.cscv_pbo(M, n_splits=n_splits)


# --------------------------------------------------------------------------- report ---

def render_report(cells: list[Cell], dsr: dict, pbo_result: dict, dsr_pass: float = 0.0, pbo_ceiling: float = 0.5) -> str:
    # A cell with too few valid formations (e.g. insider_buy with no insiders data fetched) reports
    # t=0.0 (stats.newey_west_t's early-return, not a real "no effect" reading) and must not rank
    # above a cell that was genuinely evaluated and came up negative.
    tested = [c for c in cells if np.isfinite(c.returns).sum() >= MIN_FORMATIONS]
    ranked = sorted(tested, key=lambda c: (c.summary["top_nw_t"] if c.summary["horizon"] == PRIMARY_HORIZON else float("-inf")), reverse=True)
    lines = [f"# Brute-force sweep ({len(cells)} configurations evaluated, grid size {N_CONFIGS})", "",
             f"* DSR (best cell, {dsr['best_cell']}): {dsr['deflated_sharpe_annual']:+.3f} annualised, "
             f"n_trials={dsr['n_trials']}, P(SR>SR0)={dsr['dsr_probability']:.3f}",
             f"* PBO ({pbo_result['n_splits_evaluated']} CSCV splits, {pbo_result['n_configs']} configs): {pbo_result['pbo']:.3f}",
             "", f"**Verdict: {'PASS' if dsr['deflated_sharpe_annual'] > dsr_pass and pbo_result['pbo'] < pbo_ceiling else 'FAIL'}** "
             f"(needs DSR > {dsr_pass} and PBO < {pbo_ceiling})", "",
             f"## Top 15 by primary-horizon Newey-West t ({len(cells) - len(tested)} of {len(cells)} cells excluded: "
             f"fewer than {MIN_FORMATIONS} valid formation months)", "",
             "| arm | buckets | horizon | band | top excess % | t | spearman |", "|---|---|---|---|---|---|---|"]
    for c in ranked[:15]:
        s = c.summary
        lines.append(f"| {c.arm} | {c.n_buckets} | {c.horizon} | {c.band} | {s['top_mean_pct']:+.2f} | "
                     f"{s['top_nw_t']:+.2f} | {s['spearman']:+.2f} |")
    return "\n".join(lines)


def write_outputs(cells: list[Cell], dsr: dict, pbo_result: dict, out_root: Path) -> Path:
    stamp = dt.datetime.now().strftime("%Y%m%dT%H%M%S")
    d = out_root / "bruteforce"
    d.mkdir(parents=True, exist_ok=True)
    md = render_report(cells, dsr, pbo_result)
    payload = {"dsr": dsr, "pbo": pbo_result, "n_configs": N_CONFIGS,
              "cells": [{"arm": c.arm, "n_buckets": c.n_buckets, "horizon": c.horizon, "band": c.band, **c.summary} for c in cells]}
    (d / f"{stamp}.json").write_text(json.dumps(payload, indent=2, default=float), encoding="utf-8")
    (d / f"{stamp}.md").write_text(md, encoding="utf-8")
    return d / f"{stamp}.md"
