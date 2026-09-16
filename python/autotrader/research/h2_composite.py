"""H2: long-only multi-factor composite. Spec: docs/prereg/H2-composite.md.

At the last session of each month, within the band being sorted:
  mom   = 12-1 momentum (close_tr[f-21] / close_tr[f-252] - 1)
  gpa   = trailing-twelve-month gross profit (SF1 ART gp) / total assets (SF1 ARQ assets)
  iss   = -ln(shares[f] / shares[f-252])   (SF1 ARQ sharesbas, restated for splits; buybacks score high)
Each is winsorised at the 1st/99th percentile and z-scored within the band; the
composite is the equal-weight mean of the available z-scores, needing at least two.
Weights are fixed and never estimated.
"""
from __future__ import annotations

import numpy as np

from autotrader.research import pretest, stats
from autotrader.research.h1_resid_mom import momentum_12_1
from autotrader.research.panel import Panel

N_BUCKETS = 5
HORIZONS = [20, 60, 120]
PRIMARY = 60
BANDS = ["5B+", "2B-5B", "500M-2B", "all"]
FIRST_FORMATION = "2017-09-01"
MIN_COMPONENTS = 2


def raw_signals(panel: Panel, f: int) -> dict[str, np.ndarray]:
    gp = panel.asof_matrix("ART", "gp")[f]
    assets = panel.asof_matrix("ARQ", "assets")[f]
    sh = panel.shares()
    with np.errstate(invalid="ignore", divide="ignore"):
        gpa = np.where(assets > 0, gp / assets, np.nan)
        iss = -np.log(sh[f] / sh[f - 252]) if f >= 252 else np.full(panel.N, np.nan)
    iss = np.where(np.isfinite(iss), iss, np.nan)
    return {"mom": momentum_12_1(panel, f), "gpa": gpa, "iss": iss}


def composite(signals: dict[str, np.ndarray], elig: np.ndarray) -> np.ndarray:
    zs = []
    for v in signals.values():
        z = np.full(v.shape, np.nan)
        z[elig] = stats.winsorized_z(v[elig])
        zs.append(z)
    Z = np.vstack(zs)
    n = np.isfinite(Z).sum(axis=0)
    c = np.where(n > 0, np.nansum(Z, axis=0) / np.maximum(n, 1), np.nan)
    c[n < MIN_COMPONENTS] = np.nan
    return c


def run(panel: Panel, first_formation: str = FIRST_FORMATION) -> tuple[dict, str, int]:
    formations = panel.month_end_indices(start=first_formation)
    cache: dict[int, dict[str, np.ndarray]] = {}

    def sig(f: int) -> dict[str, np.ndarray]:
        if f not in cache:
            cache[f] = raw_signals(panel, f)
        return cache[f]

    arms = {
        "composite": lambda f, e: composite(sig(f), e),
        "single_mom": lambda f, e: sig(f)["mom"],
        "single_gpa": lambda f, e: sig(f)["gpa"],
        "single_iss": lambda f, e: sig(f)["iss"],
    }
    result = {"hypothesis": "H2", "formations": [str(panel.dates[f]) for f in formations]}
    parts = [f"## H2 composite ({len(formations)} monthly formations, {result['formations'][0]} .. {result['formations'][-1]})"]
    for name, fn in arms.items():
        series = pretest.run_monthly_sort(panel, formations, fn, N_BUCKETS, HORIZONS, BANDS)
        result[name] = pretest.summarize_sort(series, PRIMARY, 21)
        title = "Composite (gating arm)" if name == "composite" else f"Single signal {name[7:]} (diversification check, not gating)"
        parts.append(pretest.render_sort_markdown(title, result[name], N_BUCKETS))

    # Diversification check: composite top quintile vs each single signal's, per band at the primary horizon.
    div = {}
    for band in BANDS:
        c = result["composite"][band][str(PRIMARY)]["top_mean_pct"]
        singles = {k: result[k][band][str(PRIMARY)]["top_mean_pct"] for k in ("single_mom", "single_gpa", "single_iss")}
        div[band] = {"composite_top_pct": c, **{f"{k}_top_pct": v for k, v in singles.items()},
                     "beats_all_singles": bool(all(c > v for v in singles.values()))}
    result["diversification_check"] = div
    parts.append("### Diversification check (primary horizon, top quintile, reported not gating)\n\n| band | composite | mom | gpa | iss | beats all |\n|---|---|---|---|---|---|\n"
                 + "\n".join(f"| {b} | {d['composite_top_pct']:+.2f} | {d['single_mom_top_pct']:+.2f} | {d['single_gpa_top_pct']:+.2f} | "
                             f"{d['single_iss_top_pct']:+.2f} | {d['beats_all_singles']} |" for b, d in div.items()))
    n_configs = len(arms) * len(BANDS) * len(HORIZONS)
    return result, "\n\n".join(parts), n_configs
