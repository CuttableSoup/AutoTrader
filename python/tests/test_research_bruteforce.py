"""Brute-force sweep plumbing: PBO/CSCV and the new signal primitives. No vendor data."""
from __future__ import annotations

import numpy as np
import pandas as pd
import pytest

from autotrader.research import pbo, signals
from autotrader.research.h1_resid_mom import MonthlyData
from autotrader.research.panel import Panel, load_event_dates, load_insider_transactions


def synth_panel(T: int = 400, N: int = 5, seed: int = 3) -> Panel:
    rng = np.random.default_rng(seed)
    dates = pd.bdate_range("2018-01-02", periods=T).to_numpy().astype("datetime64[D]")
    rets = rng.normal(0, 0.01, (T, N))
    close = 50.0 * np.cumprod(1 + rets, axis=0)
    px = {"open": close.copy(), "close": close.copy(), "closeadj": close.copy(), "closeunadj": close.copy(),
          "volume": np.full((T, N), 1e6)}
    fund = pd.DataFrame([{"ticker": f"S{j}", "dimension": "ARQ", "date": q, "sharesbas": 2e8, "sharefactor": 1.0,
                          "assets": 1e9 * (1.0 + 0.01 * j), "equity": 4e8, "debt": 3e8, "netinc": 5e7, "ncfo": 6e7}
                         for j in range(N) for q in pd.date_range("2016-06-01", periods=24, freq="QS")])
    return Panel(dates=dates, symbols=np.array([f"S{j}" for j in range(N)]), px=px,
                 spy_open_tr=np.full(T, 100.0), spy_close_tr=np.full(T, 100.0),
                 first_price=np.full(N, np.datetime64("2010-01-01")), sector=np.array(["X"] * N),
                 fundamentals=fund, end="2099-01-01")


# ---------------------------------------------------------------------- PBO ---

def test_pbo_near_half_for_pure_noise():
    # A single noise realisation has wide PBO variance by itself (CSCV's own known limitation:
    # with a fixed draw, which column looks best in-sample is itself a noisy statistic). Average
    # several independent draws, which is what the theory (PBO ~ 0.5 for no true skill) actually predicts.
    vals = []
    for seed in range(10):
        rng = np.random.default_rng(seed)
        returns = rng.normal(0, 0.01, (240, 30))
        res = pbo.cscv_pbo(returns, n_splits=8)
        vals.append(res["pbo"])
    assert res["n_configs"] == 30 and res["n_periods"] == 240
    assert 0.35 < float(np.mean(vals)) < 0.65


def test_pbo_low_for_a_planted_consistent_edge():
    rng = np.random.default_rng(1)
    returns = rng.normal(0, 0.01, (240, 30))
    returns[:, 0] += 0.01                      # config 0: real edge present in every block
    res = pbo.cscv_pbo(returns, n_splits=8)
    assert res["pbo"] < 0.30


def test_cscv_rejects_odd_or_too_many_splits():
    returns = np.zeros((10, 3))
    with pytest.raises(ValueError):
        pbo.cscv_pbo(returns, n_splits=3)
    with pytest.raises(ValueError):
        pbo.cscv_pbo(returns, n_splits=20)
    with pytest.raises(ValueError):
        pbo.cscv_pbo(np.zeros((10, 1)), n_splits=2)


def test_sharpe_like_handles_degenerate_input():
    assert signals is not None  # smoke: module imports cleanly alongside pbo
    assert pbo.sharpe_like(np.array([1.0])) == float("-inf")
    assert pbo.sharpe_like(np.array([0.0, 0.0, 0.0])) == 0.0


# ----------------------------------------------------------- price/technical ---

def test_reversal_1m_is_negated_1m_return():
    p = synth_panel(T=60, N=1)
    p.px["closeadj"][:, 0] = np.linspace(10, 20, 60)      # monotone rise -> positive 1m return
    got = signals.reversal_1m(p, 40)
    expected = -(p.px["closeadj"][40, 0] / p.px["closeadj"][19, 0] - 1.0)
    assert got[0] == pytest.approx(expected)
    assert np.isnan(signals.reversal_1m(p, 5)).all()


def test_high52w_proximity_is_zero_at_the_high():
    p = synth_panel(T=300, N=1)
    p.px["closeadj"][:, 0] = np.r_[np.linspace(10, 50, 260), np.full(40, 10.0)]
    got = signals.high52w_proximity(p, 259)
    assert got[0] == pytest.approx(0.0, abs=1e-9)         # session 259 IS the trailing high
    assert np.isnan(signals.high52w_proximity(p, 100)).all()


def test_amihud_illiquidity_ranks_low_volume_names_higher():
    p = synth_panel(T=60, N=2)
    p.px["volume"][:, 0] = 1e5    # thin
    p.px["volume"][:, 1] = 1e8    # deep
    got = signals.amihud_illiquidity(p, 40)
    assert got[0] > got[1]


# --------------------------------------------------------------------- fundamental ---

def test_asset_growth_negates_raw_growth():
    p = synth_panel(T=300, N=1)
    a = p.asof_matrix("ARQ", "assets")
    got = signals.asset_growth(p, 280)
    raw = a[280, 0] / a[280 - 252, 0] - 1.0
    assert got[0] == pytest.approx(-raw)


def test_accruals_negates_raw_measure():
    p = synth_panel(T=10, N=1)
    ni, cfo, a = (p.asof_matrix("ARQ", c)[5, 0] for c in ("netinc", "ncfo", "assets"))
    got = signals.accruals(p, 5)
    assert got[0] == pytest.approx(-((ni - cfo) / a))


def test_book_to_market_is_equity_over_mcap():
    p = synth_panel(T=10, N=1)
    got = signals.book_to_market(p, 5)
    expected = p.asof_matrix("ARQ", "equity")[5, 0] / p.market_cap()[5, 0]
    assert got[0] == pytest.approx(expected)


def test_leverage_change_needs_a_year_of_history():
    p = synth_panel(T=300, N=1)
    assert np.isnan(signals.leverage_change(p, 100)).all()
    got = signals.leverage_change(p, 280)
    assert np.isfinite(got).all()      # debt/assets are constant in the fixture -> change is exactly zero
    assert got[0] == pytest.approx(0.0, abs=1e-9)


# ------------------------------------------------------------------- event-based ---

def test_insider_net_buying_empty_table_is_nan_missing_activity_is_zero():
    p = synth_panel(T=10, N=2)
    empty = pd.DataFrame(columns=["ticker", "filingdate", "signed_dollars"])
    assert np.isnan(signals.insider_net_buying(p, 5, empty)).all()

    tx = pd.DataFrame({"ticker": ["S0"], "filingdate": [p.dates[5] - np.timedelta64(10, "D")], "signed_dollars": [1e6]})
    got = signals.insider_net_buying(p, 5, tx, window_days=90)
    assert got[1] == 0.0                                   # S1: table has data, but nothing for S1 -> a real zero
    assert got[0] == pytest.approx(1e6 / p.market_cap()[5, 0])


def test_recent_material_event_flag_windowed():
    p = synth_panel(T=10, N=2)
    events = {"S0": np.array([p.dates[5] - np.timedelta64(3, "D")], dtype="datetime64[D]")}
    got = signals.recent_material_event_flag(p, 5, events, lookback_days=10)
    assert got[0] == -1.0 and got[1] == 0.0
    got_tight = signals.recent_material_event_flag(p, 5, events, lookback_days=1)
    assert got_tight[0] == 0.0


def test_idio_vol_ff3_is_negated_residual_std():
    M, N = 40, 20
    rng = np.random.default_rng(9)
    md = MonthlyData.__new__(MonthlyData)
    md.factors = rng.normal(0, 0.04, (M, 3))
    md.rf = np.zeros(M)
    md.ret = 1.2 * md.factors[:, [0]] + rng.normal(0, 0.02, (M, N))
    md.ret[M - 12:M - 1, 0] *= 5.0                         # S0: much noisier idiosyncratic residuals
    md.month_of_session = np.full(1, M - 1)
    p = synth_panel(T=1, N=N)
    got = signals.idio_vol_ff3(p, 0, md)
    assert got[0] < np.nanmedian(got[1:])                  # noisiest name scores LOWEST (negated)


# -------------------------------------------------------------------- panel loaders ---

def test_load_event_dates_filters_by_code_set(tmp_path):
    csv = tmp_path / "events-1-2026-01-01.csv"
    csv.write_text("ticker,date,eventcodes\nAAA,2020-01-05,11|23\nAAA,2020-02-01,81\nBBB,2020-01-05,22\n")
    out = load_event_dates(np.array(["AAA", "BBB"]), {"11", "22"}, events_csv=csv)
    assert list(out.keys()) == ["AAA", "BBB"]
    assert out["AAA"][0] == np.datetime64("2020-01-05")


def test_load_event_dates_missing_file_returns_empty(tmp_path):
    assert load_event_dates(np.array(["AAA"]), {"11"}, raw_dir=tmp_path) == {}


def test_load_insider_transactions_keeps_only_open_market_buys_and_sells(tmp_path):
    csv = tmp_path / "insiders-10-2026-01-01.csv"
    csv.write_text("ticker,filingdate,transactioncode,transactionshares,transactionpricepershare\n"
                   "AAA,2020-01-05,P,1000,10.0\nAAA,2020-01-06,A,500,0\nAAA,2020-01-07,S,200,12.0\n")
    df = load_insider_transactions(np.array(["AAA"]), insiders_csv=csv)
    assert len(df) == 2
    assert set(df["signed_dollars"]) == {10000.0, -2400.0}


def test_load_insider_transactions_missing_file_returns_empty_frame(tmp_path):
    df = load_insider_transactions(np.array(["AAA"]), raw_dir=tmp_path)
    assert df.empty and list(df.columns) == ["ticker", "filingdate", "signed_dollars"]
