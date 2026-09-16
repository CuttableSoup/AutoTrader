"""Shared pre-test harness: forward excess returns, bucket sorts, summaries, the pass rule.

Conventions (docs/prereg/README.md):
  * A score formed at the close of session f is traded at the open of f+1 and closed
    at the close of f+1+h. Returns are total-return, and EXCESS over SPY across the
    identical window, in percent.
  * A name that stops trading inside the window is valued at its last close for the
    rest of it (delisting returns are not in the data).
  * Pass rule at the primary horizon, per size band: top bucket excess > 0 with
    Newey-West t > 2; Spearman(bucket, mean) >= 0.8; top bucket positive in all three
    thirds of the sample; top bucket still > 0 after one full round-trip cost for the
    band. Secondary horizons are reported only.
"""
from __future__ import annotations

import datetime as dt
import json
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

import numpy as np
import pandas as pd

from autotrader.research import stats
from autotrader.research.panel import Panel

ROUND_TRIP_BPS = {"500M-2B": 50.0, "2B-5B": 30.0, "5B+": 10.0, "all": 30.0}
MIN_PER_BUCKET = 5
PASS_T = 2.0
PASS_SPEARMAN = 0.8


# ---------------------------------------------------------------- returns ---

def close_tr_filled(panel: Panel) -> np.ndarray:
    if "close_tr_ffill" not in panel._cache:
        panel._cache["close_tr_ffill"] = pd.DataFrame(panel.close_tr()).ffill().to_numpy()
    return panel._cache["close_tr_ffill"]


def forward_excess_pct(panel: Panel, f: int, h: int) -> np.ndarray:
    """(N,) excess total return over SPY, percent, entering at open f+1 and exiting at close f+1+h."""
    entry, exit_ = f + 1, f + 1 + h
    if exit_ >= panel.T:
        return np.full(panel.N, np.nan)
    key = ("fwd", f, h)
    if key not in panel._cache:
        with np.errstate(invalid="ignore", divide="ignore"):
            stock = close_tr_filled(panel)[exit_] / panel.open_tr()[entry] - 1.0
        spy = panel.spy_close_tr[exit_] / panel.spy_open_tr[entry] - 1.0
        panel._cache[key] = (stock - spy) * 100.0
    return panel._cache[key]


# -------------------------------------------------------------- summaries ---

@dataclass
class BandSeries:
    """Per-formation bucket means for one band and horizon: (F, B) with NaN where too thin."""
    labels: list[str]            # formation labels (dates or quarters)
    means: np.ndarray            # (F, B)
    counts: np.ndarray           # (F, B)
    turnover: list[float]        # top bucket, per formation (NaN where undefined)


def summarize(s: BandSeries, h: int, spacing_sessions: int, cost_bps: float, primary: bool, lags: int | None = None) -> dict:
    B = s.means.shape[1]
    top, bot = s.means[:, B - 1], s.means[:, 0]
    lag = lags if lags is not None else stats.nw_lags(h, spacing_sessions)
    ok_top = np.isfinite(top)
    bucket_means = [float(np.nanmean(s.means[:, b])) if np.isfinite(s.means[:, b]).any() else float("nan") for b in range(B)]
    spread = top - bot
    thirds_idx = np.nonzero(ok_top)[0]
    thirds = [float(np.nanmean(top[thirds_idx[sl]])) if len(thirds_idx[sl]) else float("nan") for sl in stats.thirds(len(thirds_idx))]
    top_mean = float(np.nanmean(top)) if ok_top.any() else float("nan")
    out = {
        "horizon": h, "n_formations": int(ok_top.sum()), "avg_names_top": float(np.nanmean(s.counts[:, B - 1])) if ok_top.any() else 0.0,
        "bucket_means_pct": bucket_means, "top_mean_pct": top_mean, "top_nw_t": stats.newey_west_t(top, lag),
        "spread_mean_pct": float(np.nanmean(spread)) if np.isfinite(spread).any() else float("nan"),
        "spread_nw_t": stats.newey_west_t(spread, lag), "spearman": stats.spearman_monotonicity(bucket_means),
        "thirds_top_pct": thirds, "turnover_top": float(np.nanmean(s.turnover)) if np.isfinite(s.turnover).any() else float("nan"),
        "cost_bps": cost_bps, "top_net_pct": top_mean - cost_bps / 100.0, "nw_lags": lag, "primary": primary,
    }
    if primary:
        reasons = []
        if not (top_mean > 0 and out["top_nw_t"] > PASS_T):
            reasons.append(f"top excess {top_mean:+.3f}% t={out['top_nw_t']:+.2f} (needs > 0 and t > {PASS_T})")
        if not (out["spearman"] >= PASS_SPEARMAN):
            reasons.append(f"spearman {out['spearman']:.2f} < {PASS_SPEARMAN}")
        if not all(np.isfinite(thirds)) or not all(t > 0 for t in thirds):
            reasons.append("top bucket not positive in all three sub-periods: " + ", ".join(f"{t:+.2f}" for t in thirds))
        if not (out["top_net_pct"] > 0):
            reasons.append(f"top net of {cost_bps:.0f}bp round trip {out['top_net_pct']:+.3f}% <= 0")
        out["pass"] = not reasons
        out["fail_reasons"] = reasons
    return out


# ------------------------------------------------------------- sort runner ---

def run_monthly_sort(panel: Panel, formations: np.ndarray, score_fn: Callable[[int, np.ndarray], np.ndarray],
                     n_buckets: int, horizons: list[int], bands: list[str]) -> dict[str, dict[int, BandSeries]]:
    """Sort each band's eligible names into buckets at every formation and record forward excess means.

    score_fn(f, eligible_mask) -> (N,) scores (NaN = not scoreable). It receives the band's
    mask so that cross-sectional standardisation happens within the sorted population.
    """
    masks = panel.band_masks()
    out: dict[str, dict[int, BandSeries]] = {}
    for band in bands:
        per_h = {h: BandSeries([], np.full((len(formations), n_buckets), np.nan), np.zeros((len(formations), n_buckets)), [])
                 for h in horizons}
        prev_top: set[int] = set()
        for k, f in enumerate(formations):
            elig = masks[band][f]
            score = score_fn(int(f), elig)
            sc = np.where(elig, score, np.nan)
            b = stats.assign_buckets(sc, n_buckets)
            top = set(np.nonzero(b == n_buckets - 1)[0].tolist())
            turn = stats.one_sided_turnover(prev_top, top) if prev_top else float("nan")
            prev_top = top
            for h in horizons:
                fwd = forward_excess_pct(panel, int(f), h)
                s = per_h[h]
                if k == len(s.labels):
                    s.labels.append(str(panel.dates[f]))
                    s.turnover.append(turn)
                for q in range(n_buckets):
                    vals = fwd[(b == q) & np.isfinite(fwd)]
                    s.counts[k, q] = len(vals)
                    if len(vals) >= MIN_PER_BUCKET:
                        s.means[k, q] = float(vals.mean())
        out[band] = per_h
    return out


def summarize_sort(series: dict[str, dict[int, BandSeries]], primary_h: int, spacing: int, lags: int | None = None) -> dict:
    return {band: {str(h): summarize(s, h, spacing, ROUND_TRIP_BPS[band], h == primary_h, lags) for h, s in per_h.items()}
            for band, per_h in series.items()}


# ------------------------------------------------------------ prereg gate ---

class PreregError(RuntimeError):
    pass


def prereg_commit(path: Path) -> str:
    """Commit hash of a pre-registration that is committed and unmodified; raises otherwise."""
    path = Path(path).resolve()
    cwd = path.parent
    def git(*args: str) -> subprocess.CompletedProcess:
        return subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True)
    if git("ls-files", "--error-unmatch", str(path)).returncode != 0:
        raise PreregError(f"{path.name} is not committed; commit the pre-registration before running its test")
    if git("diff", "--quiet", "HEAD", "--", str(path)).returncode != 0:
        raise PreregError(f"{path.name} has uncommitted changes; the test must run against the committed spec")
    h = git("log", "-1", "--format=%H", "--", str(path)).stdout.strip()
    if not h:
        raise PreregError(f"cannot resolve the commit of {path.name}")
    return h


# ---------------------------------------------------------------- report ---

def render_sort_markdown(title: str, summary: dict, n_buckets: int) -> str:
    lines = [f"### {title}", ""]
    for band, per_h in summary.items():
        lines.append(f"**{band}**")
        lines.append("")
        lines.append("| h | n | names/top | " + " | ".join(f"B{b + 1}" for b in range(n_buckets))
                     + " | top t | spread | spread t | spearman | thirds (top) | turnover | net top | verdict |")
        lines.append("|" + "---|" * (n_buckets + 12))
        for h, r in per_h.items():
            verdict = ("**PASS**" if r["pass"] else "FAIL") if r["primary"] else "(secondary)"
            lines.append(
                f"| {h}{'*' if r['primary'] else ''} | {r['n_formations']} | {r['avg_names_top']:.0f} | "
                + " | ".join(f"{m:+.2f}" for m in r["bucket_means_pct"])
                + f" | {r['top_nw_t']:+.2f} | {r['spread_mean_pct']:+.2f} | {r['spread_nw_t']:+.2f} | {r['spearman']:+.2f} | "
                + " / ".join(f"{t:+.2f}" for t in r["thirds_top_pct"])
                + f" | {r['turnover_top']:.2f} | {r['top_net_pct']:+.2f} | {verdict} |")
        fails = [r for r in per_h.values() if r["primary"] and not r["pass"]]
        for r in fails:
            lines.append("")
            lines.append("Fail reasons: " + "; ".join(r["fail_reasons"]))
        lines.append("")
    return "\n".join(lines)


def write_outputs(hypothesis: str, result: dict, markdown: str, out_root: Path) -> Path:
    stamp = dt.datetime.now().strftime("%Y%m%dT%H%M%S")
    d = out_root / hypothesis
    d.mkdir(parents=True, exist_ok=True)
    (d / f"{stamp}.json").write_text(json.dumps(result, indent=2, default=float), encoding="utf-8")
    (d / f"{stamp}.md").write_text(markdown, encoding="utf-8")
    return d / f"{stamp}.md"
