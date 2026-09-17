"""TSMOM-v1 research plumbing: fold geometry, cost model, signal/weights, portfolio
construction. No vendor data except the end-to-end test, which skips if at_dsr isn't built."""
from __future__ import annotations

import json

import numpy as np
import pandas as pd
import pytest

from autotrader.research import tsmom, walkforward
from autotrader.research.etf_panel import EtfPanel


def synth_etf_panel(T: int = 1200, N: int = 4, seed: int = 6, drift: np.ndarray | None = None) -> EtfPanel:
    rng = np.random.default_rng(seed)
    dates = pd.bdate_range("2016-09-19", periods=T).to_numpy().astype("datetime64[D]")
    d = np.zeros(N) if drift is None else drift
    rets = d[None, :] + rng.normal(0, 0.008, (T, N))
    close = 100.0 * np.cumprod(1 + rets, axis=0)
    spread = rng.uniform(0.0005, 0.002, (T, N))
    high = close * (1 + spread / 2)
    low = close * (1 - spread / 2)
    px = {"open": close.copy(), "high": high, "low": low, "close": close.copy(), "closeadj": close.copy(),
          "volume": np.full((T, N), 1e6)}
    return EtfPanel(dates=dates, symbols=np.array([f"E{j}" for j in range(N)]), px=px, end="2099-01-01")


# ------------------------------------------------------------------- fold geometry ---

def test_purge_embargo_folds_basic_geometry():
    folds = walkforward.purge_embargo_folds(T=1000, train_sessions=200, test_sessions=100, purge_sessions=10, embargo_sessions=10)
    assert len(folds) >= 1
    f0 = folds[0]
    assert f0.train_start == 0 and f0.train_end == 200
    assert f0.test_start == 210 and f0.test_end == 310
    if len(folds) > 1:
        f1 = folds[1]
        assert f1.test_start - f0.test_end == 10          # embargo gap between consecutive test windows
        assert f1.train_start == 110                        # step = test_sessions + embargo_sessions = 110


def test_purge_embargo_folds_none_when_too_short():
    assert walkforward.purge_embargo_folds(T=50, train_sessions=200, test_sessions=100, purge_sessions=10, embargo_sessions=10) == []


def test_sub_period_stability_splits_evenly():
    r = np.array([0.01] * 30 + [-0.02] * 30 + [0.03] * 30)
    thirds = walkforward.sub_period_stability(r, 3)
    assert len(thirds) == 3
    assert thirds[0] > 0 and thirds[1] < 0 and thirds[2] > 0


# ------------------------------------------------------------------------- cost ---

def test_corwin_schultz_recovers_a_planted_spread():
    rng = np.random.default_rng(1)
    T = 300
    close = 100.0 * np.cumprod(1 + rng.normal(0, 0.001, T))     # low-vol path so the spread dominates the H-L range
    true_spread = 0.004                                          # 40bp
    high = close * (1 + true_spread / 2)
    low = close * (1 - true_spread / 2)
    est = tsmom.corwin_schultz_spread(high, low)
    est = est[np.isfinite(est)]
    assert np.isnan(tsmom.corwin_schultz_spread(high, low)[0])
    assert abs(np.median(est) - true_spread) < true_spread * 0.5   # order-of-magnitude recovery, not exact


def test_corwin_schultz_floors_negative_estimates_at_zero():
    high = np.array([100.0, 100.1, 100.05, 100.2])
    low = np.array([99.9, 100.0, 99.95, 100.1])
    est = tsmom.corwin_schultz_spread(high, low)
    assert np.all(est[np.isfinite(est)] >= 0)


# ------------------------------------------------------------------------ signal ---

def test_compute_weights_gross_never_exceeds_one_even_if_every_leg_trends():
    p = synth_etf_panel(T=400, N=6, drift=np.full(6, 0.002))     # every leg trending hard, same direction
    formations = p.month_end_indices(start="2017-06-01")
    W = tsmom.compute_weights(p, formations, lookback=252)
    gross = np.abs(W).sum(axis=1)
    assert np.all(gross <= 1.0 + 1e-9)


def test_compute_weights_sign_matches_trend_direction():
    p = synth_etf_panel(T=400, N=2, drift=np.array([0.003, -0.003]))
    formations = p.month_end_indices(start="2017-06-01")
    W = tsmom.compute_weights(p, formations, lookback=252)
    last = W[-1]
    assert last[0] > 0 and last[1] < 0


def test_compute_weights_long_only_floors_negative_momentum():
    p = synth_etf_panel(T=400, N=2, drift=np.array([0.003, -0.003]))
    formations = p.month_end_indices(start="2017-06-01")
    W = tsmom.compute_weights(p, formations, lookback=252, long_only=True)
    assert np.all(W >= 0)


def test_compute_weights_has_no_lookahead():
    p = synth_etf_panel(T=400, N=2, drift=np.array([0.002, 0.002]))
    formations = p.month_end_indices(start="2017-06-01")
    W_before = tsmom.compute_weights(p, formations, lookback=252)
    f_last = int(formations[-1])
    # Mutate every price strictly AFTER the last formation date; the weight AT that formation must be unchanged.
    for k in p.px:
        p.px[k][f_last + 1:] *= 5.0
    p._cache.clear()
    W_after = tsmom.compute_weights(p, formations, lookback=252)
    assert np.allclose(W_before[-1], W_after[-1])


# --------------------------------------------------------------------- portfolio ---

def test_portfolio_returns_charges_cost_exactly_on_the_rebalance_day():
    p = synth_etf_panel(T=400, N=2)
    formations = p.month_end_indices(start="2017-06-01")
    W = tsmom.compute_weights(p, formations, lookback=252)
    rf = np.zeros(p.T)
    spr = tsmom.spread_bps_series(p)
    book_with_cost, costs = tsmom.portfolio_returns(p, formations, W, rf, spr, commission_bps=1000.0)  # huge cost, easy to see
    book_no_cost, _ = tsmom.portfolio_returns(p, formations, W, rf, spr, commission_bps=0.0)
    k = len(formations) - 3   # a formation with an actual weight change
    f = int(formations[k])
    entry = f + 1   # first session the new weights are intraday-active
    if costs[k] > 0 and np.isfinite(book_with_cost[entry]) and np.isfinite(book_no_cost[entry]):
        assert book_with_cost[entry] < book_no_cost[entry]
        assert book_with_cost[entry] == pytest.approx(book_no_cost[entry] - costs[k], abs=1e-6)


# --------------------------------------------------------------- boundary/regression ---

def test_transition_day_is_split_between_old_and_new_weights_not_dropped():
    """Regression: the exit day used to be attributed whole to the OLD weights (h4_trend.py's
    convention, wrongly copied), silently dropping the new position's first intraday move."""
    p = synth_etf_panel(T=400, N=2)
    formations = p.month_end_indices(start="2017-06-01")
    W = tsmom.compute_weights(p, formations, lookback=252)
    w_intraday, w_overnight = tsmom._weight_series(p, formations, W)
    k = len(formations) - 3
    f = int(formations[k])
    entry = f + 1
    # The incoming weights must be intraday-active starting exactly at the open of f+1 --
    # not one session later -- and the prior day must NOT already carry them overnight.
    assert np.array_equal(w_intraday[entry], W[k])
    assert np.array_equal(w_overnight[entry], W[k - 1] if k > 0 else np.zeros(p.N))


def test_eligible_formations_excludes_undercooked_rows():
    """Regression: a hand-picked FIRST_FORMATION date could fall short of the true
    252+60-session warm-up, letting an all-zero row through as if it were a real signal."""
    p = synth_etf_panel(T=500, N=3)
    formations = tsmom.eligible_formations(p)
    assert formations[0] >= tsmom.MAX_LOOKBACK + tsmom.VOL_LOOKBACK
    W = tsmom.compute_weights(p, formations, lookback=tsmom.MAX_LOOKBACK)
    assert np.any(W[0] != 0)          # the very first eligible formation must be a real signal, not idle cash


def test_missing_spread_falls_back_to_conservative_not_free():
    row = np.array([np.nan, 12.0, np.nan, 30.0])
    filled = tsmom._fill_missing_spread(row)
    assert filled[0] == 30.0 and filled[2] == 30.0            # NaNs fall back to the widest quoted spread that day
    assert np.array_equal(filled[[1, 3]], row[[1, 3]])
    assert np.all(tsmom._fill_missing_spread(np.full(4, np.nan)) == 50.0)   # nothing available -> fixed conservative default


def test_etf_panel_cache_round_trips_at_full_precision(tmp_path):
    from autotrader.research.etf_panel import EtfPanel, FIELDS, _manifest
    dates = pd.bdate_range("2020-01-01", periods=5).to_numpy().astype("datetime64[D]")
    px = {k: np.array([[1.0000001, 2.0000003]] * 5) for k in FIELDS}
    p = EtfPanel(dates=dates, symbols=np.array(["A", "B"]), px=px, end="2099-01-01")
    npz = tmp_path / "t.npz"
    np.savez(npz, dates=p.dates, symbols=p.symbols, **p.px)
    (tmp_path / "t.json").write_text(json.dumps(_manifest(["A", "B"], "2099-01-01")))
    z = np.load(npz, allow_pickle=False)
    for k in FIELDS:
        assert np.array_equal(z[k].astype(np.float64), px[k])   # no float32 downcast in the round trip


# --------------------------------------------------------------------------- run ---

def test_run_end_to_end_structure(monkeypatch):
    p = synth_etf_panel(T=1400, N=6, drift=np.array([0.0008, -0.0006, 0.0, 0.0005, -0.0003, 0.0002]))
    monkeypatch.setattr(tsmom, "TRAIN_SESSIONS", 300)
    monkeypatch.setattr(tsmom, "TEST_SESSIONS", 100)
    monkeypatch.setattr(tsmom, "PURGE_SESSIONS", 10)
    monkeypatch.setattr(tsmom, "EMBARGO_SESSIONS", 10)
    ff = pd.DataFrame({"mkt_rf": 0.0, "smb": 0.0, "hml": 0.0, "rf": 0.0}, index=pd.bdate_range("2016-01-01", periods=2000))
    try:
        from autotrader.research.sweep import find_dsr_cli
        find_dsr_cli()
    except FileNotFoundError:
        pytest.skip("at_dsr not built; see CLAUDE.md for the cygwin build command")
    result, markdown, n_configs = tsmom.run(p, ff_daily=ff)
    assert n_configs == 4 == len(tsmom.VARIANTS)
    assert result["hypothesis"] == "TSMOM-v1"
    assert set(result["checks"]) == {"dsr_positive", "folds_net_positive", "sub_period_stability", "min_sample_folds"}
    assert "TSMOM-v1" in markdown and "Verdict" in markdown
    assert result["n_folds"] >= 1
