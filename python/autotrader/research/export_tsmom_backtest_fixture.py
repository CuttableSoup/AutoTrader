"""One-off export of a TSMOM gross-return golden fixture for the C++ backtester parity
test (cpp/tests/test_tsmom_backtest.cpp), which checks
cpp/src/backtester/tsmom_backtester.cpp's multi-session return attribution (the
open/close intraday/overnight split across formation transitions -- the thing
export_tsmom_fixture.py's single-formation weight check does NOT exercise) against
python/autotrader/research/tsmom.py::portfolio_returns.

Scope, deliberately narrower than a full gate re-run (see docs/DECISIONS.md's Phase 2
entry, decision 3: no genericized walk_forward.cpp, no re-litigating whether the
strategy works -- that's decided): this compares the GROSS return series only, with
the idle-capital risk-free-rate credit excluded on both sides (a backtest-realism
detail orthogonal to whether the signal/return-attribution math was ported correctly,
and one that would otherwise require also porting a risk-free-rate data pipeline into
C++ for no parity-relevant benefit). Costs are excluded on both sides too, per the
project's existing decision that the cost model stays a Python/research concern.

Reads only the development panel (end=SEAL_DATE) -- a code-parity check, not a new
trial; never touches the sealed hold-out, not logged to docs/trials.jsonl.

Not part of the pytest suite; run manually whenever the fixture needs regenerating:

    .venv/Scripts/python -m autotrader.research.export_tsmom_backtest_fixture
"""
from __future__ import annotations

import json
from pathlib import Path

import numpy as np

from autotrader.research.etf_panel import build_etf_panel
from autotrader.research.seal import SEAL_DATE
from autotrader.research.tsmom import UNIVERSE, VOL_LOOKBACK, compute_weights
from autotrader.schemas import find_project_root

MOMENTUM_LOOKBACK = 252
WARMUP = MOMENTUM_LOOKBACK + VOL_LOOKBACK        # 312
COMPARE_SESSIONS = 600                            # ~2.5 years, spans several formation transitions
OUT_PATH = "cpp/tests/fixtures/tsmom_backtest_golden.json"


def root() -> Path:
    return find_project_root(Path(__file__).parent)


def gross_book_returns_no_rf(panel, formations: np.ndarray, W: np.ndarray) -> np.ndarray:
    """Re-derivation of tsmom.py::portfolio_returns' return-attribution split, minus the
    idle-capital risk-free credit and minus turnover cost (both out of scope here -- see
    module docstring). Kept as an independent statement of the formula, not a call into
    portfolio_returns itself, so the fixture isn't just echoing the function under test.
    """
    open_tr, close_tr = panel.open_tr(), panel.close_tr()
    T = panel.T
    with np.errstate(invalid="ignore", divide="ignore"):
        oc_ret = close_tr / open_tr - 1.0
        co_ret = np.zeros_like(open_tr)
        co_ret[1:] = open_tr[1:] / close_tr[:-1] - 1.0

    w_intraday = np.zeros((T, panel.N))
    for k, f in enumerate(formations):
        start = int(f) + 1
        end = int(formations[k + 1]) + 1 if k + 1 < len(formations) else T
        w_intraday[start:end] = W[k]
    w_overnight = np.zeros((T, panel.N))
    w_overnight[1:] = w_intraday[:-1]

    with np.errstate(invalid="ignore"):
        book = np.nansum(w_intraday * oc_ret, axis=1) + np.nansum(w_overnight * co_ret, axis=1)
    return book


def main() -> None:
    panel = build_etf_panel(UNIVERSE, end=SEAL_DATE)   # cache hit, no network
    all_formations = panel.month_end_indices()
    eligible = all_formations[all_formations >= WARMUP]
    if len(eligible) < 4:
        raise RuntimeError(f"only {len(eligible)} eligible formations; need at least 4 for a meaningful window")

    window_end = int(panel.T)
    window_start = max(0, window_end - COMPARE_SESSIONS)
    export_start = max(0, window_start - WARMUP)   # extra warmup so the first formation inside the window is well-formed

    # Every formation from export_start onward (compute_weights itself skips any that are
    # individually still short on history, matching what evaluate_tsmom_signal does).
    formations_in_scope = all_formations[all_formations >= export_start]
    W = compute_weights(panel, formations_in_scope, MOMENTUM_LOOKBACK, long_only=False, vol_lookback=VOL_LOOKBACK)
    book_full = gross_book_returns_no_rf(panel, formations_in_scope, W)

    # Compare only the [window_start, window_end) slice -- everything before it exists
    # purely to give the first in-window formation its correct warmup.
    compare_dates = panel.dates[window_start:window_end]
    compare_returns = book_full[window_start:window_end]

    fixture = {
        "_doc": "Golden gross-return fixture for cpp/tests/test_tsmom_backtest.cpp, exported "
                "from the reviewed Python implementation (tsmom.py) by "
                "export_tsmom_backtest_fixture.py. Idle-capital risk-free credit and turnover "
                "cost are excluded on both sides (see module docstring). Sessions before "
                "warmup_start exist only to give the C++ side correct warmup; only "
                "[compare_start_date, last date] is actually compared.",
        "momentum_lookback": MOMENTUM_LOOKBACK,
        "vol_lookback": VOL_LOOKBACK,
        "compare_start_date": str(panel.dates[window_start]),
        "symbols": list(UNIVERSE),
        "asset_classes": [_asset_class(s) for s in UNIVERSE],
        "dates": [str(d) for d in panel.dates[export_start:window_end]],
        "open_tr_cents": _to_cents(panel.open_tr()[export_start:window_end]),
        "close_tr_cents": _to_cents(panel.close_tr()[export_start:window_end]),
        "expected": [
            {"date": str(d), "book_return": float(r)}
            for d, r in zip(compare_dates, compare_returns)
            if np.isfinite(r)
        ],
    }

    out_path = root() / OUT_PATH
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(fixture))
    size_kb = out_path.stat().st_size / 1024
    print(f"wrote {out_path} ({len(fixture['dates'])} sessions x {len(UNIVERSE)} symbols, "
          f"{len(fixture['expected'])} compared days, {size_kb:.0f} KB)")


def _to_cents(arr: np.ndarray) -> list[list[int | None]]:
    out = []
    for row in arr:
        out.append([int(round(p * 100)) if np.isfinite(p) else None for p in row])
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


if __name__ == "__main__":
    main()
