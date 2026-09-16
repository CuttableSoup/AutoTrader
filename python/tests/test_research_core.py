"""Stage 0 research plumbing: seal, ledger, statistics, point-in-time panel. No vendor data."""
from __future__ import annotations

import math

import numpy as np
import pandas as pd
import pytest

from autotrader.research import seal, stats
from autotrader.research.factors import compound_monthly, monthly_from_daily, parse_french_csv
from autotrader.research.ledger import Ledger
from autotrader.research.panel import Panel


# ------------------------------------------------------------------ helpers ---

def make_panel(T: int = 400, N: int = 3, shares: float = 1e8, price: float = 10.0, filings: list | None = None) -> Panel:
    dates = np.arange(np.datetime64("2020-01-01"), np.datetime64("2020-01-01") + np.timedelta64(T * 2, "D"), np.timedelta64(2, "D")).astype("datetime64[D]")[:T]
    close = np.full((T, N), price)
    px = {"open": close.copy(), "close": close.copy(), "closeadj": close.copy(), "closeunadj": close.copy(),
          "volume": np.full((T, N), 1e6)}
    rows = filings if filings is not None else [
        {"ticker": f"S{j}", "dimension": "ARQ", "date": pd.Timestamp("2019-06-01"), "sharesbas": shares, "sharefactor": 1.0}
        for j in range(N)]
    fund = pd.DataFrame(rows, columns=["ticker", "dimension", "date", "sharesbas", "sharefactor", "eps"])
    return Panel(dates=dates, symbols=np.array([f"S{j}" for j in range(N)]), px=px,
                 spy_open_tr=np.full(T, 100.0), spy_close_tr=np.full(T, 100.0),
                 first_price=np.full(N, np.datetime64("2010-01-01")), sector=np.array(["X"] * N),
                 fundamentals=fund, end="2099-01-01")


# --------------------------------------------------------------------- seal ---

def test_seal_requires_reason_and_logs_unseal(tmp_path):
    led = Ledger(tmp_path / "t.jsonl")
    assert seal.data_end(ledger=led) == seal.SEAL_DATE
    with pytest.raises(seal.SealError):
        seal.data_end(unseal=True, reason="  ", ledger=led)
    assert led.read() == []
    assert seal.data_end(unseal=True, reason="final hold-out run of H1", hypothesis="H1", ledger=led) == seal.OPEN_END
    (e,) = led.read()
    assert e["kind"] == "unseal" and e["hypothesis"] == "H1" and e["n_configs"] == 0


def test_check_window():
    seal.check_window(seal.SEAL_DATE, unseal=False)
    with pytest.raises(seal.SealError):
        seal.check_window("2025-09-16", unseal=False)
    seal.check_window("2025-09-16", unseal=True)


# ------------------------------------------------------------------- ledger ---

def test_ledger_counts_configs_and_variance(tmp_path):
    led = Ledger(tmp_path / "t.jsonl")
    led.append(kind="walkforward", hypothesis="a", n_configs=84, data_window="w", notes="", var_sr_trials=0.0016)
    led.append(kind="pretest", hypothesis="b", n_configs=12, data_window="w", notes="")
    assert led.count() == 96
    assert led.max_var_sr_trials() == pytest.approx(0.0016)
    with pytest.raises(ValueError):
        led.append(kind="bogus", hypothesis="c", n_configs=1, data_window="w", notes="")


# -------------------------------------------------------------------- stats ---

def test_newey_west_zero_lags_equals_plain_t_with_population_variance():
    x = np.array([0.5, -0.2, 0.9, 0.1, 0.4, -0.3, 0.7, 0.2])
    expected = x.mean() / math.sqrt(x.var(ddof=0) / len(x))
    assert stats.newey_west_t(x, 0) == pytest.approx(expected)


def test_newey_west_penalises_positive_autocorrelation():
    rng = np.random.default_rng(1)
    e = rng.normal(0.1, 1.0, 600)
    x = np.convolve(e, np.ones(3) / 3, mode="valid")      # MA(2): overlapping windows
    assert abs(stats.newey_west_t(x, 3)) < abs(stats.newey_west_t(x, 0))


def test_nw_lags():
    assert stats.nw_lags(60) == 3
    assert stats.nw_lags(5) == 1


def test_assign_buckets_ranks_and_missing():
    s = np.array([5.0, np.nan, 1.0, 3.0, 2.0, 4.0])
    b = stats.assign_buckets(s, 5)
    assert b.tolist() == [4, -1, 0, 2, 1, 3]
    assert (stats.assign_buckets(np.array([1.0, 2.0]), 5) == -1).all()


def test_planted_signal_is_monotone_and_noise_is_not():
    rng = np.random.default_rng(7)
    score = rng.normal(size=5000)
    fwd_signal = 0.5 * score + rng.normal(size=5000)
    fwd_noise = rng.normal(size=5000)
    b = stats.assign_buckets(score, 10)
    means_sig = [fwd_signal[b == k].mean() for k in range(10)]
    means_noise = [fwd_noise[b == k].mean() for k in range(10)]
    assert stats.spearman_monotonicity(means_sig) >= 0.8
    assert stats.spearman_monotonicity(means_noise) < 0.8


def test_winsorized_z_and_drawdown_and_turnover():
    z = stats.winsorized_z(np.array([1.0, 2.0, 3.0, 1000.0, np.nan]))
    assert np.isnan(z[-1]) and abs(np.nanmean(z)) < 1e-9
    assert stats.max_drawdown(np.array([0.1, -0.5, 0.2])) == pytest.approx(0.5)
    assert stats.one_sided_turnover({"a", "b"}, {"b", "c", "d", "e"}) == pytest.approx(0.75)


# ------------------------------------------------------------------ factors ---

def test_parse_french_csv_and_monthly():
    text = ("This file was created by CMPT_ME_BEME_RETS_DAILY\n\n,Mkt-RF,SMB,HML,RF\n"
            "20240102,  -0.50,   0.10,   0.20,   0.02\n20240103,   1.00,  -0.10,   0.00,   0.02\n"
            "20240201,   2.00,   0.00,   0.00,   0.02\n\nCopyright 2024 Kenneth R. French\n")
    df = parse_french_csv(text)
    assert len(df) == 3 and df.loc["2024-01-02", "mkt_rf"] == pytest.approx(-0.005)
    m = monthly_from_daily(df)
    assert m["mkt_rf"].iloc[0] == pytest.approx((1 - 0.005) * 1.01 - 1)


def test_compound_monthly_needs_15_sessions():
    r = np.full((30, 1), 0.01)
    r[20:, 0] = np.nan
    months, out = compound_monthly(r, np.array([0] * 20 + [1] * 10))
    assert out[0, 0] == pytest.approx(1.01 ** 20 - 1) and np.isnan(out[1, 0])


# -------------------------------------------------------------------- panel ---

def test_fundamental_invisible_on_filing_date_visible_next_session():
    p = make_panel(T=10, N=1, filings=[{"ticker": "S0", "dimension": "ARQ", "date": pd.Timestamp("2020-01-05"),
                                        "sharesbas": 5e7, "sharefactor": 1.0}])
    sh = p.shares()[:, 0]
    filing = p.date_index("2020-01-05")          # 2020-01-05 is a session in the every-2-days calendar
    assert p.dates[filing] == np.datetime64("2020-01-05")
    assert np.isnan(sh[filing]) and sh[filing + 1] == 5e7


def test_name_enters_universe_only_when_it_crosses_the_floor():
    """Regression for the look-ahead selection in data/sharadar: membership must use data at t only."""
    p = make_panel(T=100, N=1, price=4.0)
    ramp = np.r_[np.full(40, 4.0), np.linspace(4.0, 12.0, 60)]   # flat warm-up (ADV defined), then $400M -> $1.2B
    for k in ("open", "close", "closeadj", "closeunadj"):
        p.px[k][:, 0] = ramp
    p.px["volume"][:, 0] = 1e7                                   # ADV never binds; the cap/price floor must
    mask = p.universe_mask()[:, 0]
    first_in = int(np.argmax(mask))
    assert mask.any() and not mask[:first_in].any()
    assert p.market_cap()[first_in, 0] >= 5e8 and p.px["closeunadj"][first_in, 0] >= 5.0
    assert p.market_cap()[first_in - 1, 0] < 5e8 or p.px["closeunadj"][first_in - 1, 0] < 5.0


def test_listing_age_and_bands():
    p = make_panel(T=60, N=2, price=30.0)                  # $3B
    p.first_price[1] = p.dates[30]
    m = p.universe_mask()
    assert m[40:, 0].all() and not m[:, 1].any()           # S1 listed < 365 days for the whole panel
    bands = p.band_masks()
    assert bands["2B-5B"][40:, 0].all() and not bands["5B+"][:, 0].any()


def test_month_end_indices_skip_incomplete_final_month():
    p = make_panel(T=40)
    idx = p.month_end_indices()
    months = p.dates.astype("datetime64[M]")
    assert all(months[i] != months[i + 1] for i in idx)
