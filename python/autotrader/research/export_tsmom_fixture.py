"""One-off export of a TSMOM golden fixture for the C++ parity test
(cpp/tests/test_tsmom_signal.cpp), which checks the C++ port of the primary
spec's per-instrument signal math against this already-reviewed Python
implementation (tsmom.py::compute_weights).

Reads only the development panel (end=SEAL_DATE, docs/prereg/TSMOM-v1.md) --
this is a code-parity check, not a new trial, so it never touches the sealed
hold-out and is not logged to docs/trials.jsonl.

Not part of the pytest suite; run manually whenever the fixture needs
regenerating (should be rare -- TSMOM-v1 is a frozen spec):

    .venv/Scripts/python -m autotrader.research.export_tsmom_fixture
"""
from __future__ import annotations

import json
from pathlib import Path

import numpy as np

from autotrader.research.etf_panel import build_etf_panel
from autotrader.research.seal import SEAL_DATE
from autotrader.research.tsmom import UNIVERSE, VOL_LOOKBACK
from autotrader.schemas import find_project_root

MOMENTUM_LOOKBACK = 252
WARMUP = MOMENTUM_LOOKBACK + VOL_LOOKBACK   # 312: the actual binding requirement (momentum dominates)
OUT_PATH = "cpp/tests/fixtures/tsmom_weights_golden.json"


def root() -> Path:
    return find_project_root(Path(__file__).parent)


def per_instrument(c: np.ndarray, r: np.ndarray, f: int) -> list[dict]:
    """Per-symbol mom_sign/vol_annual/raw_weight/target_weight at formation index f,
    a direct re-derivation of tsmom.py::compute_weights' inner loop for one formation
    (not a call to compute_weights itself, so the fixture is an independent statement
    of what the primary spec's formula computes, not just an echo of the function under
    test on both sides).
    """
    with np.errstate(invalid="ignore", divide="ignore"):
        mom = np.sign(c[f] / c[f - MOMENTUM_LOOKBACK] - 1.0)
    vol = np.nanstd(r[f - VOL_LOOKBACK + 1:f + 1], axis=0, ddof=1) * np.sqrt(252)
    with np.errstate(invalid="ignore", divide="ignore"):
        raw_w = np.where(vol > 0, mom / vol, 0.0)
    raw_w = np.nan_to_num(raw_w, nan=0.0)
    gross = np.sum(np.abs(raw_w))
    scale = min(1.0, 1.0 / gross) if gross > 0 else 0.0
    w = raw_w * scale

    out = []
    for j, sym in enumerate(UNIVERSE):
        # Trailing WARMUP+1 prices ending at f (inclusive), oldest first -- everything the
        # C++ side needs to independently recompute mom_sign/vol_annual/raw_weight from
        # scratch on its own synthetic date axis (the real calendar dates don't matter to
        # the math, only the count and order of sessions do).
        prices = c[f - WARMUP:f + 1, j]
        assert len(prices) == WARMUP + 1
        out.append({
            "symbol": sym,
            "asset_class": _asset_class(sym),
            "prices_cents": [int(round(p * 100)) for p in prices],
            "mom_sign": int(mom[j]),
            "vol_annual": float(vol[j]),
            "raw_weight": float(raw_w[j]),
            "target_weight": float(w[j]),
        })
    return out


def _asset_class(sym: str) -> str:
    equity = {"SPY", "QQQ", "IWM", "EFA", "EEM"}
    rates = {"TLT", "IEF", "SHY", "LQD", "HYG"}
    commodities = {"GLD", "SLV", "USO", "DBC"}
    if sym in equity:
        return "EQUITY"
    if sym in rates:
        return "RATES_CREDIT"
    if sym in commodities:
        return "COMMODITIES"
    return "CURRENCIES"


def main() -> None:
    panel = build_etf_panel(UNIVERSE, end=SEAL_DATE)   # cache hit against data/cache/research/, no network
    c = panel.close_tr()
    r = panel.daily_returns()
    formations = panel.month_end_indices()
    eligible = formations[formations >= WARMUP]
    if len(eligible) < 3:
        raise RuntimeError(f"only {len(eligible)} eligible formations in the development panel; need at least 3")

    # First eligible, one from the middle, and the last -- spanning the panel rather than
    # clustering near either end, same spirit as the fold table in docs/TSMOM-RESULT.md.
    chosen = [int(eligible[0]), int(eligible[len(eligible) // 2]), int(eligible[-1])]

    fixture = {
        "_doc": "Golden fixture for cpp/tests/test_tsmom_signal.cpp, exported from the reviewed Python "
                "implementation (tsmom.py) by export_tsmom_fixture.py. Dates are development-panel-relative "
                "(end=SEAL_DATE); C++ replays the price counts onto its own synthetic date axis, so no real "
                "calendar dates are needed or stored.",
        "momentum_lookback": MOMENTUM_LOOKBACK,
        "vol_lookback": VOL_LOOKBACK,
        "formations": [],
    }
    for f in chosen:
        fixture["formations"].append({
            "formation_date": str(panel.dates[f]),
            "n_prices_per_symbol": WARMUP + 1,
            "instruments": per_instrument(c, r, f),
        })

    out_path = root() / OUT_PATH
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(fixture, indent=2))
    print(f"wrote {out_path} ({len(chosen)} formations x {len(UNIVERSE)} symbols)")


if __name__ == "__main__":
    main()
