"""Stage 1 harness and hypothesis code on synthetic panels. No vendor data."""
from __future__ import annotations

import subprocess

import numpy as np
import pandas as pd
import pytest

from autotrader.research import h1_resid_mom, h2_composite, h3_pead, h4_trend, pretest
from autotrader.research.panel import Panel


def synth_panel(T: int = 900, N: int = 200, drift: np.ndarray | None = None, seed: int = 3) -> Panel:
    rng = np.random.default_rng(seed)
    dates = pd.bdate_range("2017-01-02", periods=T).to_numpy().astype("datetime64[D]")
    d = np.zeros(N) if drift is None else drift
    rets = d[None, :] + rng.normal(0, 0.01, (T, N))
    close = 50.0 * np.cumprod(1 + rets, axis=0)
    px = {"open": close.copy(), "close": close.copy(), "closeadj": close.copy(), "closeunadj": close.copy(),
          "volume": np.full((T, N), 1e6)}
    fund = pd.DataFrame([{"ticker": f"S{j}", "dimension": "ARQ", "date": q, "sharesbas": 2e8, "sharefactor": 1.0}
                         for j in range(N) for q in pd.date_range("2016-06-01", periods=24, freq="QS")])
    return Panel(dates=dates, symbols=np.array([f"S{j}" for j in range(N)]), px=px,
                 spy_open_tr=np.full(T, 100.0), spy_close_tr=np.full(T, 100.0),
                 first_price=np.full(N, np.datetime64("2010-01-01")), sector=np.array(["X"] * N),
                 fundamentals=fund, end="2099-01-01")


# ----------------------------------------------------------------- harness ---

def test_forward_excess_timing():
    p = synth_panel(T=50, N=2)
    f, h = 10, 5
    got = pretest.forward_excess_pct(p, f, h)
    expected = (p.px["closeadj"][f + 1 + h] / p.px["open"][f + 1] - 1) * 100
    assert np.allclose(got, expected)
    assert np.isnan(pretest.forward_excess_pct(p, 45, 5)).all()


def test_forward_excess_values_delisted_name_at_last_close():
    p = synth_panel(T=50, N=1)
    last = p.px["closeadj"][20, 0]
    for k in p.px:
        p.px[k][21:, 0] = np.nan
    assert pretest.forward_excess_pct(p, 15, 20)[0] == pytest.approx((last / p.px["open"][16, 0] - 1) * 100)


def test_planted_drift_passes_and_noise_fails():
    N = 200
    score = np.linspace(-1, 1, N)
    planted = synth_panel(drift=0.0008 * score)
    formations = planted.month_end_indices(start="2018-01-01")
    s = pretest.run_monthly_sort(planted, formations, lambda f, e: score, 10, [60], ["all"])
    res = pretest.summarize_sort(s, 60, 21)["all"]["60"]
    assert res["pass"], res["fail_reasons"]

    noise = synth_panel(drift=np.zeros(N), seed=11)
    s = pretest.run_monthly_sort(noise, formations, lambda f, e: score, 10, [60], ["all"])
    res = pretest.summarize_sort(s, 60, 21)["all"]["60"]
    assert not res["pass"]


def test_prereg_gate_requires_committed_unmodified_file(tmp_path):
    def git(*a):
        subprocess.run(["git", *a], cwd=tmp_path, check=True, capture_output=True)
    git("init", "-q")
    git("config", "user.email", "t@t")
    git("config", "user.name", "t")
    f = tmp_path / "H9-test.md"
    f.write_text("spec v1")
    with pytest.raises(pretest.PreregError):
        pretest.prereg_commit(f)
    git("add", f.name)
    git("commit", "-q", "-m", "prereg")
    assert len(pretest.prereg_commit(f)) == 40
    f.write_text("spec v2, edited after seeing data")
    with pytest.raises(pretest.PreregError):
        pretest.prereg_commit(f)


# ---------------------------------------------------------------------- H1 ---

def test_residual_momentum_removes_factor_exposure_and_ranks_idiosyncratic_drift():
    M, N = 40, 102
    rng = np.random.default_rng(5)
    md = h1_resid_mom.MonthlyData.__new__(h1_resid_mom.MonthlyData)
    md.factors = rng.normal(0, 0.04, (M, 3))
    md.rf = np.zeros(M)
    noise = rng.normal(0, 0.03, (M, N))
    ret = 1.5 * md.factors[:, [0]] + noise                        # every stock has beta 1.5 to the market
    ret[M - 12:M - 1, 0] += 0.06                                   # S0: idiosyncratic drift in months m-11..m-1
    ret[M - 1, 1] += 0.40                                          # S1: large move in the skipped month m only
    md.ret = ret
    s = h1_resid_mom.residual_momentum_scores(md, M - 1, np.ones(N, dtype=bool))
    p90 = np.percentile(s[2:], 90)
    assert s[0] > p90                                              # planted drift ranks in the top decile
    assert s[1] < p90                                              # a month-m spike does not


def test_residual_momentum_needs_24_months():
    md = h1_resid_mom.MonthlyData.__new__(h1_resid_mom.MonthlyData)
    md.factors = np.zeros((36, 3)) + 0.01
    md.rf = np.zeros(36)
    md.ret = np.full((36, 1), np.nan)
    md.ret[-23:, 0] = 0.01
    assert np.isnan(h1_resid_mom.residual_momentum_scores(md, 35, np.array([True]))[0])


# ---------------------------------------------------------------------- H2 ---

def test_composite_needs_two_components_and_is_equal_weight():
    elig = np.ones(6, dtype=bool)
    sig = {"mom": np.array([1, 2, 3, 4, 5, 6.0]), "gpa": np.array([6, 5, 4, 3, 2, 1.0]),
           "iss": np.array([np.nan, np.nan, 1, 2, 3, 4.0])}
    c = h2_composite.composite(sig, elig)
    assert np.isfinite(c).all()
    sig["gpa"][:2] = np.nan
    c = h2_composite.composite(sig, elig)
    assert np.isnan(c[:2]).all() and np.isfinite(c[2:]).all()


# ---------------------------------------------------------------------- H3 ---

def test_surprise_uses_prior_quarters_only():
    cal = pd.Series(pd.to_datetime([f"{y}-{m:02d}-{d}" for y in range(2015, 2020) for m, d in ((3, 31), (6, 30), (9, 30), (12, 31))]))
    eps = pd.Series(np.arange(len(cal), dtype=float) * 0.1 + np.tile([0, 0.02, -0.01, 0.03], 5))
    eps.iloc[-1] += 1.0                                            # a large final surprise
    s = h3_pead.surprise(eps, cal)
    assert np.isnan(s[:8]).all()                                   # needs 4 lags + 4 prior differences
    assert s[-1] > 5 and np.isfinite(s[8:-1]).all()


# ---------------------------------------------------------------------- H4 ---

def test_sma200_rule_is_lagged_two_sessions():
    p = synth_panel(T=400, N=1)
    p.spy_close_tr = np.r_[np.full(300, 100.0), np.full(100, 200.0)]
    expo = h4_trend.rule_sma200(p)
    assert not expo[301] and expo[302]                             # close 300 is the first above-average close


def test_overlay_verdict_logic():
    rng = np.random.default_rng(2)
    book = rng.normal(0.0004, 0.01, 1000)
    book[500:560] = -0.02                                          # a crash
    expo = np.ones(1000, dtype=bool)
    expo[500:560] = False
    r = h4_trend.evaluate(book, expo, np.zeros(1000))
    assert r["pass"] and r["overlay"]["max_dd"] < r["base"]["max_dd"]
    assert not h4_trend.evaluate(book, np.ones(1000, dtype=bool), np.zeros(1000))["pass"]
